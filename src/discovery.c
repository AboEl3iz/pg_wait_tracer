/* discovery.c — Postmaster PID, ELF symbol offset, /proc helpers */
#include "discovery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
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
