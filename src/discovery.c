/* discovery.c — Postmaster PID, ELF symbol offset, /proc helpers,
 *                PG version detection, st_query_id offset detection */
#include "discovery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/utsname.h>
#include <libelf.h>
#include <gelf.h>

pid_t pgwt_find_postmaster_pid(const char *pgdata)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/postmaster.pid", pgdata);

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
        return 0;
    }

    pid_t pid = 0;
    if (fscanf(f, "%d", &pid) != 1)
        pid = 0;
    fclose(f);
    return pid;
}

int pgwt_find_pg_binary(pid_t pid, char *buf, size_t bufsz)
{
    char link[64];
    snprintf(link, sizeof(link), "/proc/%d/exe", pid);

    ssize_t n = readlink(link, buf, bufsz - 1);
    if (n < 0) {
        fprintf(stderr, "readlink(%s): %s\n", link, strerror(errno));
        return -1;
    }
    buf[n] = '\0';
    return 0;
}

uint64_t pgwt_find_symbol_offset(const char *binary, const char *symbol)
{
    uint64_t result = 0;

    if (elf_version(EV_CURRENT) == EV_NONE) {
        fprintf(stderr, "libelf init failed: %s\n", elf_errmsg(-1));
        return 0;
    }

    int fd = open(binary, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", binary, strerror(errno));
        return 0;
    }

    Elf *elf = elf_begin(fd, ELF_C_READ, NULL);
    if (!elf) {
        fprintf(stderr, "elf_begin(%s): %s\n", binary, elf_errmsg(-1));
        close(fd);
        return 0;
    }

    Elf_Scn *scn = NULL;
    while ((scn = elf_nextscn(elf, scn)) != NULL) {
        GElf_Shdr shdr;
        if (gelf_getshdr(scn, &shdr) == NULL)
            continue;
        if (shdr.sh_type != SHT_SYMTAB && shdr.sh_type != SHT_DYNSYM)
            continue;

        Elf_Data *data = elf_getdata(scn, NULL);
        if (!data)
            continue;

        int nsyms = shdr.sh_size / shdr.sh_entsize;
        for (int i = 0; i < nsyms; i++) {
            GElf_Sym sym;
            if (gelf_getsym(data, i, &sym) == NULL)
                continue;
            const char *name = elf_strptr(elf, shdr.sh_link, sym.st_name);
            if (name && strcmp(name, symbol) == 0) {
                result = sym.st_value;
                goto done;
            }
        }
    }

done:
    elf_end(elf);
    close(fd);

    if (result == 0)
        fprintf(stderr, "Symbol '%s' not found in %s\n", symbol, binary);

    return result;
}

/* Get the ELF base virtual address (lowest PT_LOAD p_vaddr).
 * For PIE (ET_DYN) this is typically 0; for non-PIE (ET_EXEC) it's
 * the actual load address (e.g. 0x400000). */
static uint64_t elf_base_vaddr(const char *binary)
{
    if (elf_version(EV_CURRENT) == EV_NONE)
        return 0;

    int fd = open(binary, O_RDONLY);
    if (fd < 0)
        return 0;

    Elf *elf = elf_begin(fd, ELF_C_READ, NULL);
    if (!elf) {
        close(fd);
        return 0;
    }

    uint64_t base = UINT64_MAX;
    size_t phnum = 0;
    elf_getphdrnum(elf, &phnum);
    for (size_t i = 0; i < phnum; i++) {
        GElf_Phdr phdr;
        if (gelf_getphdr(elf, i, &phdr) == NULL)
            continue;
        if (phdr.p_type == PT_LOAD && phdr.p_vaddr < base)
            base = phdr.p_vaddr;
    }

    elf_end(elf);
    close(fd);
    return (base == UINT64_MAX) ? 0 : base;
}

uint64_t pgwt_resolve_symbol(const char *binary, const char *symbol,
                             pid_t pid, const char *binary_basename)
{
    uint64_t sym_val = pgwt_find_symbol_offset(binary, symbol);
    if (sym_val == 0)
        return 0;

    uint64_t load_base = pgwt_find_load_base(pid, binary_basename);
    if (load_base == 0)
        return 0;

    uint64_t elf_base = elf_base_vaddr(binary);
    /* Runtime VA = sym_value - elf_base_vaddr + runtime_load_base
     * For PIE:     elf_base=0, so VA = sym_value + load_base
     * For non-PIE: elf_base=load_base (e.g. both 0x400000), so VA = sym_value */
    return sym_val - elf_base + load_base;
}

uint64_t pgwt_find_load_base(pid_t pid, const char *binary_basename)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
        return 0;
    }

    uint64_t base = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, binary_basename) && strstr(line, "r--p")) {
            /* First r--p mapping of the binary is the load base */
            base = strtoull(line, NULL, 16);
            break;
        }
    }
    fclose(f);

    if (base == 0)
        fprintf(stderr, "Load base for '%s' not found in /proc/%d/maps\n",
                binary_basename, pid);
    return base;
}

uint64_t pgwt_read_pointer(pid_t pid, uint64_t addr)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", pid);

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;

    uint64_t val = 0;
    if (pread(fd, &val, sizeof(val), addr) != sizeof(val))
        val = 0;
    close(fd);
    return val;
}

/* ── PG Version Detection ─────────────────────────────────── */

int pgwt_detect_pg_version(const char *pg_binary)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s --version 2>/dev/null", pg_binary);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "WARN: cannot run '%s --version'\n", pg_binary);
        return 0;
    }

    char line[256];
    int major = 0;
    if (fgets(line, sizeof(line), fp)) {
        /* Format: "postgres (PostgreSQL) 18.2" or "postgres (PostgreSQL) 14.12" */
        char *p = strstr(line, "PostgreSQL");
        if (p) {
            p += strlen("PostgreSQL");
            /* Skip ") " or just whitespace */
            while (*p && (*p == ')' || *p == ' '))
                p++;
            major = atoi(p);
        }
    }
    pclose(fp);

    if (major < 1 || major > 99) {
        fprintf(stderr, "WARN: cannot parse PG version from '%s'\n", line);
        return 0;
    }
    return major;
}

/* ── st_query_id Offset Detection ─────────────────────────── */

/* Tier 1: Try DWARF debug info via readelf */
static int detect_offset_dwarf(const char *pg_binary)
{
    /* Try the binary itself first, then the detached debug info.
     * Debian/Ubuntu: /usr/lib/debug/.build-id/XX/YYYY.debug
     * We let readelf handle the debug link automatically. */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "readelf --debug-dump=info '%s' 2>/dev/null | awk '"
             "/DW_TAG_structure_type/ { s=0 } "
             "/DW_AT_name.*PgBackendStatus/ { s=1 } "
             "s && /DW_AT_name.*st_query_id/ { "
             "  while ((getline line) > 0) { "
             "    if (line ~ /DW_AT_data_member_location/) { "
             "      match(line, /[0-9]+$/); "
             "      if (RSTART>0) { print substr(line,RSTART,RLENGTH); exit } "
             "    } "
             "    if (line ~ /DW_TAG_/) break "
             "  } "
             "}'",
             pg_binary);

    FILE *fp = popen(cmd, "r");
    if (!fp)
        return 0;

    char buf[64];
    int offset = 0;
    if (fgets(buf, sizeof(buf), fp))
        offset = atoi(buf);
    pclose(fp);

    return offset;
}

/* Tier 2: Known offsets for common PG versions on x86_64 */
static int detect_offset_known(int pg_major)
{
    struct utsname un;
    if (uname(&un) != 0)
        return 0;

    int is_x86_64 = (strcmp(un.machine, "x86_64") == 0);

    if (is_x86_64) {
        switch (pg_major) {
        case 18: return 424;
        case 17: return 424;
        /* PG14-16: st_query_id offset needs verification.
         * Return 0 for now — DWARF detection is preferred. */
        default: return 0;
        }
    }
    /* ARM64 and other architectures: rely on DWARF */
    return 0;
}

int pgwt_detect_query_id_offset(const char *pg_binary, int pg_major)
{
    if (pg_major < 14) {
        /* st_query_id was added in PG14 */
        return 0;
    }

    /* Tier 1: DWARF debug symbols */
    int offset = detect_offset_dwarf(pg_binary);
    if (offset > 0)
        return offset;

    /* Tier 2: Known offset table */
    offset = detect_offset_known(pg_major);
    if (offset > 0)
        return offset;

    /* Tier 3: Not available */
    return 0;
}

/* ── Postmaster Auto-Discovery ───────────────────────────── */

#include <dirent.h>
#include <stdbool.h>

/* Read /proc/<pid>/comm into buf. Returns 0 on success. */
static int read_proc_comm(pid_t pid, char *buf, size_t bufsz)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, bufsz, f)) { fclose(f); return -1; }
    fclose(f);
    /* Strip trailing newline */
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    return 0;
}

/* Extract PG version from exe path.
 * Patterns: /usr/pgsql-17/bin/postgres, /usr/lib/postgresql/17/bin/postgres */
static int version_from_exe(const char *exe)
{
    const char *p;

    /* PGDG RPM: /usr/pgsql-17/bin/postgres */
    p = strstr(exe, "pgsql-");
    if (p) return atoi(p + 6);

    /* Debian/Ubuntu: /usr/lib/postgresql/17/bin/postgres */
    p = strstr(exe, "postgresql/");
    if (p) return atoi(p + 11);

    return 0;
}

pid_t pgwt_auto_discover_postmaster(bool verbose)
{
    DIR *proc = opendir("/proc");
    if (!proc) {
        fprintf(stderr, "Cannot open /proc: %s\n", strerror(errno));
        return 0;
    }

    struct { pid_t pid; int version; char exe[256]; } candidates[16];
    int ncand = 0;

    struct dirent *ent;
    while ((ent = readdir(proc)) != NULL && ncand < 16) {
        pid_t pid = atoi(ent->d_name);
        if (pid <= 0)
            continue;

        /* Check if this is a postgres process */
        char comm[64];
        if (read_proc_comm(pid, comm, sizeof(comm)) != 0)
            continue;
        if (strcmp(comm, "postgres") != 0)
            continue;

        /* Read ppid from /proc/<pid>/stat (field 4) */
        char stat_path[64];
        snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
        FILE *f = fopen(stat_path, "r");
        if (!f) continue;

        int stat_pid, ppid;
        char stat_comm[256];
        char state;
        int ok = fscanf(f, "%d %255s %c %d", &stat_pid, stat_comm, &state, &ppid);
        fclose(f);
        if (ok != 4) continue;

        /* If parent is also postgres, this is a child — skip */
        char parent_comm[64];
        if (ppid > 0 && read_proc_comm(ppid, parent_comm, sizeof(parent_comm)) == 0) {
            if (strcmp(parent_comm, "postgres") == 0)
                continue;
        }

        /* This is a postmaster. Get exe path and version. */
        char exe[256];
        if (pgwt_find_pg_binary(pid, exe, sizeof(exe)) != 0)
            continue;

        int ver = version_from_exe(exe);

        candidates[ncand].pid = pid;
        candidates[ncand].version = ver;
        snprintf(candidates[ncand].exe, sizeof(candidates[ncand].exe), "%s", exe);
        ncand++;
    }
    closedir(proc);

    if (ncand == 0) {
        fprintf(stderr, "No running PostgreSQL instance found\n");
        return 0;
    }

    if (ncand == 1) {
        if (verbose)
            fprintf(stderr, "INFO: auto-discovered postmaster PID %d (PG%d) %s\n",
                    candidates[0].pid, candidates[0].version, candidates[0].exe);
        return candidates[0].pid;
    }

    /* Multiple postmasters found — list them */
    fprintf(stderr, "Multiple PostgreSQL instances found:\n");
    for (int i = 0; i < ncand; i++) {
        fprintf(stderr, "  PID %-8d PG%-4d %s\n",
                candidates[i].pid,
                candidates[i].version,
                candidates[i].exe);
    }
    return 0;
}
