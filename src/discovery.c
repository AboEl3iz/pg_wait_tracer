/* discovery.c — Postmaster PID, ELF symbol offset, /proc helpers,
 *                PG version detection, st_query_id offset detection */
#include "discovery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pwd.h>
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
        if (!strstr(line, binary_basename))
            continue;
        /* The load base is the LOWEST-address mapping of the binary.
         * /proc/<pid>/maps is sorted by start address, so the first mapping
         * that names the binary is the load base — regardless of its
         * permissions. We must NOT require a specific permission bit here:
         *
         *   - PIE / ET_DYN (Rocky 9, Ubuntu, Debian PGDG): the first mapping
         *     is the read-only ELF-header page (r--p), so the old "first r--p"
         *     match happened to be correct.
         *   - non-PIE / ET_EXEC (Rocky 8 / RHEL 8 PGDG postgres): the first
         *     mapping is the text segment (r-xp) and the r--p mapping is
         *     rodata, far above the load base. Matching "first r--p" there
         *     returned the rodata address, producing a wildly wrong symbol VA
         *     (every wait_event read came back 0 → no events captured).
         *
         * Taking the first matching mapping is correct for both. */
        base = strtoull(line, NULL, 16);
        break;
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

/* ── PG<17 MyProc-based wait_event_info resolution ────────────
 *
 * PG17 added the `my_wait_event_info` global (a uint32* that points directly
 * at the backend's wait_event_info). Before PG17 there is no such global;
 * backends write `MyProc->wait_event_info` directly. So on PG<17 we resolve
 * the `MyProc` PGPROC* global instead, read it per backend to get that
 * backend's PGPROC, and add offsetof(PGPROC, wait_event_info).
 *
 * The watched FIELD (wait_event_info) has existed in PGPROC since PG 9.6, and
 * a watchpoint on its address catches writes regardless of which pointer the
 * code wrote through — so the full + sampled pipelines are identical once the
 * address is resolved; only this resolution differs by version. */

int pgwt_detect_pgproc_wait_offset(int pg_major)
{
    struct utsname un;
    if (uname(&un) != 0)
        return 0;
    if (strcmp(un.machine, "x86_64") != 0)
        return 0;   /* offsets are layout-/ABI-specific; only x86_64 known */

    switch (pg_major) {
    /* Header-derived (offsetof(PGPROC, wait_event_info), postgresql<v>-devel):
     *   PG13.23 → 684  (compiled probe, confirmed live vs pg_stat_activity).
     * PG14/15/16 deliberately omitted here until their offset is header-derived
     * and validated; the runtime guard (pgwt_validate_wait_addr) refuses to
     * attach if an offset is wrong, so a missing entry fails safe rather than
     * tracing garbage. */
    case 13: return 684;
    default: return 0;
    }
}

uint64_t pgwt_resolve_wait_addr_via_myproc(pid_t pid, uint64_t my_proc_global_addr,
                                           int pgproc_wait_offset)
{
    if (my_proc_global_addr == 0 || pgproc_wait_offset <= 0)
        return 0;
    /* *MyProc → this backend's PGPROC; + offset → wait_event_info field. */
    uint64_t pgproc = pgwt_read_pointer(pid, my_proc_global_addr);
    if (pgproc == 0)
        return 0;   /* MyProc not yet assigned (backend still in early init) */
    return pgproc + (uint64_t)pgproc_wait_offset;
}

int pgwt_validate_wait_addr(pid_t pid, uint64_t wait_addr)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    uint32_t wei = 0;
    ssize_t r = pread(fd, &wei, sizeof(wei), (off_t)wait_addr);
    close(fd);
    if (r != (ssize_t)sizeof(wei))
        return -1;

    /* Class byte is the high byte. CPU (0x00000000) is valid (not waiting);
     * otherwise the class must be a known wait-event class. PG wait classes
     * occupy 0x01..0x0B (LWLock..InjectionPoint); anything else means the
     * offset is wrong (custom build / mis-detected layout). */
    if (wei == 0)
        return 1;
    unsigned cls = (wei >> 24) & 0xFF;
    if (cls >= 0x01 && cls <= 0x0B)
        return 1;
    return 0;
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

/* ── st_activity_raw Offset Detection ─────────────────────── */

/* Tier 1: Try DWARF debug info via readelf */
static int detect_activity_offset_dwarf(const char *pg_binary)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "readelf --debug-dump=info '%s' 2>/dev/null | awk '"
             "/DW_TAG_structure_type/ { s=0 } "
             "/DW_AT_name.*PgBackendStatus/ { s=1 } "
             "s && /DW_AT_name.*st_activity_raw/ { "
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
static int detect_activity_offset_known(int pg_major)
{
    struct utsname un;
    if (uname(&un) != 0)
        return 0;

    if (strcmp(un.machine, "x86_64") == 0) {
        switch (pg_major) {
        case 18: return 248;
        /* PG17 and earlier: field name and offset may differ.
         * Return 0 for now — DWARF detection is preferred. */
        default: return 0;
        }
    }
    return 0;
}

int pgwt_detect_activity_offset(const char *pg_binary, int pg_major)
{
    if (pg_major < 17) {
        /* st_activity_raw pointer was introduced in PG18 (refactored from inline).
         * PG17 may have a different layout. Skip for safety. */
        return 0;
    }

    /* Tier 1: DWARF debug symbols */
    int offset = detect_activity_offset_dwarf(pg_binary);
    if (offset > 0)
        return offset;

    /* Tier 2: Known offset table */
    offset = detect_activity_offset_known(pg_major);
    if (offset > 0)
        return offset;

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

        /* comm can contain spaces and parens, so find last ')' */
        char stat_line[512];
        if (!fgets(stat_line, sizeof(stat_line), f)) { fclose(f); continue; }
        fclose(f);
        char *last_paren = strrchr(stat_line, ')');
        if (!last_paren) continue;
        int ppid;
        char state;
        if (sscanf(last_paren + 1, " %c %d", &state, &ppid) != 2) continue;

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

/* ── PGDATA Inference ─────────────────────────────────────── */

int pgwt_infer_pgdata(pid_t pid, char *buf, size_t bufsz)
{
    char link[64];
    snprintf(link, sizeof(link), "/proc/%d/cwd", pid);

    ssize_t n = readlink(link, buf, bufsz - 1);
    if (n < 0) {
        fprintf(stderr, "readlink(%s): %s\n", link, strerror(errno));
        return -1;
    }
    buf[n] = '\0';
    return 0;
}

/* ── Full Discovery ───────────────────────────────────────── */

#include "daemon.h"
#include "wait_event.h"

int pgwt_discover(struct pgwt_daemon *d)
{
    pid_t pm_pid = 0;

    /* Resolve postmaster PID: pgdata > pre-set PID > auto-discover */
    if (d->pgdata[0]) {
        pm_pid = pgwt_find_postmaster_pid(d->pgdata);
        if (pm_pid == 0) {
            fprintf(stderr, "FATAL: cannot read postmaster PID from %s/postmaster.pid\n",
                    d->pgdata);
            return -1;
        }
    } else if (d->postmaster_pid > 0) {
        /* PID pre-set by caller (e.g. --pid) */
        pm_pid = d->postmaster_pid;
    } else {
        pm_pid = pgwt_auto_discover_postmaster(d->verbose);
        if (pm_pid == 0) {
            if (d->verbose)
                fprintf(stderr, "INFO: no PostgreSQL instance found\n");
            return -1;
        }
    }

    /* Verify postmaster is alive */
    if (kill(pm_pid, 0) != 0) {
        fprintf(stderr, "FATAL: postmaster PID %d not found (not running?)\n", pm_pid);
        return -1;
    }
    d->postmaster_pid = pm_pid;

    /* Discover postgres binary */
    char binary[256];
    if (pgwt_find_pg_binary(pm_pid, binary, sizeof(binary)) != 0) {
        fprintf(stderr, "FATAL: cannot resolve postgres binary for PID %d\n", pm_pid);
        return -1;
    }
    /* Store on heap — struct char[] fields get corrupted by adjacent overflow */
    free(d->pg_binary_saved);
    d->pg_binary_saved = strdup(binary);
    if (d->verbose)
        fprintf(stderr, "INFO: postgres binary: %s\n", binary);

    /* Detect PostgreSQL major version */
    d->pg_major_version = pgwt_detect_pg_version(binary);
    if (d->pg_major_version == 0) {
        fprintf(stderr, "WARN: cannot detect PostgreSQL version, assuming PG18\n");
        d->pg_major_version = 18;
    } else if (d->verbose) {
        fprintf(stderr, "INFO: detected PostgreSQL %d\n", d->pg_major_version);
    }

    /* Initialize version-aware event name tables */
    pgwt_init_event_names(d->pg_major_version);

    /* Load dynamic event names from the running PG instance (PG17+).
     * This overrides hardcoded tables with the actual names from this
     * PG build, ensuring forward-compatibility with PG19+. */
    if (d->pg_major_version >= 17) {
        /* Extract bindir from binary path */
        char bindir[256];
        snprintf(bindir, sizeof(bindir), "%s", binary);
        char *slash = strrchr(bindir, '/');
        if (slash) *slash = '\0';

        /* Read port from postmaster.pid (line 4) */
        int pg_port = 5432;
        {
            char pidpath[512];
            snprintf(pidpath, sizeof(pidpath), "%s/postmaster.pid", d->pgdata);
            FILE *pf = fopen(pidpath, "r");
            if (pf) {
                char ln[128];
                for (int i = 0; i < 4 && fgets(ln, sizeof(ln), pf); i++) {
                    if (i == 3) pg_port = atoi(ln);
                }
                fclose(pf);
            }
        }

        /* Use the owner of the postmaster process as the psql user */
        char pg_user[64] = "postgres";
        {
            char statpath[64];
            snprintf(statpath, sizeof(statpath), "/proc/%d/status", pm_pid);
            FILE *sf = fopen(statpath, "r");
            if (sf) {
                char sln[256];
                while (fgets(sln, sizeof(sln), sf)) {
                    int uid;
                    if (sscanf(sln, "Uid:\t%d", &uid) == 1) {
                        struct passwd *pw = getpwuid(uid);
                        if (pw)
                            snprintf(pg_user, sizeof(pg_user), "%s", pw->pw_name);
                        break;
                    }
                }
                fclose(sf);
            }
        }

        if (pgwt_load_event_names_from_pg(bindir, pg_port, pg_user) == 0) {
            if (d->verbose)
                fprintf(stderr, "INFO: loaded dynamic event names from PG%d\n",
                        d->pg_major_version);
            /* Write sidecar for pgwt-server to use later */
            if (d->trace_dir)
                pgwt_write_names_json(d->trace_dir);
        } else if (d->verbose) {
            fprintf(stderr, "INFO: dynamic event name query failed, using hardcoded tables\n");
        }
    }

    /* Extract basename for /proc/pid/maps matching */
    const char *base = strrchr(binary, '/');
    base = base ? base + 1 : binary;

    /* Resolve the wait_event_info access path. Per-version strategy:
     *   PG17+  → the `my_wait_event_info` global (a uint32* pointing AT the
     *            field). UNCHANGED — must not regress.
     *   PG<17  → no such global; resolve the `MyProc` PGPROC* global and add
     *            offsetof(PGPROC, wait_event_info) per backend (use_myproc). */
    d->use_myproc = (d->pg_major_version > 0 && d->pg_major_version < 17);

    if (d->use_myproc) {
        d->pgproc_wait_offset = pgwt_detect_pgproc_wait_offset(d->pg_major_version);
        if (d->pgproc_wait_offset <= 0) {
            fprintf(stderr,
                    "FATAL: no known offsetof(PGPROC, wait_event_info) for PG%d "
                    "on this architecture.\n"
                    "  PG<17 needs a header-derived offset (see "
                    "tools/gen_pg13_wait_events.py / discovery.c).\n",
                    d->pg_major_version);
            return -1;
        }
        d->my_wait_ptr_addr = pgwt_resolve_symbol(binary, "MyProc", pm_pid, base);
        if (d->my_wait_ptr_addr == 0) {
            fprintf(stderr, "FATAL: cannot resolve 'MyProc' in %s (PID %d)\n",
                    binary, pm_pid);
            return -1;
        }
        if (d->verbose)
            fprintf(stderr,
                    "INFO: PG%d MyProc VA: 0x%lx, offsetof(PGPROC,wait_event_info)=%d\n",
                    d->pg_major_version, (unsigned long)d->my_wait_ptr_addr,
                    d->pgproc_wait_offset);
    } else {
        d->my_wait_ptr_addr = pgwt_resolve_symbol(binary, "my_wait_event_info",
                                                   pm_pid, base);
        if (d->my_wait_ptr_addr == 0) {
            fprintf(stderr, "FATAL: cannot resolve 'my_wait_event_info' in %s (PID %d)\n",
                    binary, pm_pid);
            return -1;
        }
        if (d->verbose)
            fprintf(stderr, "INFO: my_wait_event_info VA: 0x%lx\n",
                    (unsigned long)d->my_wait_ptr_addr);

        /* Verify pointer is readable */
        uint64_t ptr_val = pgwt_read_pointer(pm_pid, d->my_wait_ptr_addr);
        if (d->verbose)
            fprintf(stderr, "INFO: my_wait_event_info value (postmaster): 0x%lx\n",
                    (unsigned long)ptr_val);
    }

    /* Discover MyBEEntry address (for query_id attribution) */
    d->my_be_entry_addr = pgwt_resolve_symbol(binary, "MyBEEntry",
                                               pm_pid, base);
    if (d->my_be_entry_addr == 0) {
        if (d->view == PGWT_VIEW_QUERY_EVENT) {
            fprintf(stderr, "FATAL: symbol 'MyBEEntry' not found — query_event view unavailable\n");
            return -1;
        }
        if (d->verbose)
            fprintf(stderr, "WARN: symbol 'MyBEEntry' not found — query_event view disabled\n");
    } else if (d->verbose) {
        fprintf(stderr, "INFO: MyBEEntry VA: 0x%lx\n",
                (unsigned long)d->my_be_entry_addr);
    }

    /* Detect st_query_id offset */
    d->st_query_id_offset = pgwt_detect_query_id_offset(binary, d->pg_major_version);
    if (d->st_query_id_offset > 0) {
        if (d->verbose)
            fprintf(stderr, "INFO: st_query_id offset: %d (PG%d)\n",
                    d->st_query_id_offset, d->pg_major_version);
    } else {
        if (d->view == PGWT_VIEW_QUERY_EVENT) {
            fprintf(stderr, "FATAL: st_query_id offset not found for PG%d — "
                    "query_event view unavailable\n"
                    "  Hint: install postgresql-%d-dbgsym for DWARF-based detection\n",
                    d->pg_major_version, d->pg_major_version);
            return -1;
        }
        if (d->verbose)
            fprintf(stderr, "INFO: st_query_id offset not available — "
                    "query_event view disabled\n");
    }

    /* Detect st_activity_raw offset (for query text capture) */
    d->st_activity_offset = pgwt_detect_activity_offset(binary, d->pg_major_version);
    if (d->st_activity_offset > 0) {
        if (d->verbose)
            fprintf(stderr, "INFO: st_activity_raw offset: %d (PG%d)\n",
                    d->st_activity_offset, d->pg_major_version);
    } else {
        if (d->verbose)
            fprintf(stderr, "INFO: st_activity_raw offset not available — "
                    "query text capture disabled\n");
    }

    return 0;
}
