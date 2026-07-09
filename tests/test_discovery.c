/* test_discovery.c — unit tests for the #24 (load-base / symbol resolution)
 * regression class: pgwt_find_load_base_in_maps, pgwt_find_symbol_offset,
 * pgwt_resolve_symbol.
 *
 * This code broke in the field twice (PR #24: non-PIE EL8 load base; the
 * earlier "first r--p" bug) and had zero tests. Two test axes:
 *
 *  1. Committed /proc/<pid>/maps fixtures (tests/fixtures/maps_*.txt,
 *     provenance in tests/fixtures/README.md): EL8 non-PIE, EL9 PIE,
 *     Ubuntu PIE, and the adversarial CAP-4 case (extension .so whose path
 *     contains "postgres" mapped below the binary).
 *
 *  2. Self-resolution: this binary is built TWICE by tests/Makefile — once
 *     -pie (test_discovery_pie), once -no-pie (test_discovery_nopie), the
 *     exact axis #24 broke on. Each build resolves a known global symbol in
 *     its own image via /proc/self/exe + /proc/self/maps and asserts the
 *     computed runtime VA equals the symbol's actual address. A load-base
 *     regression on either ELF type fails here in milliseconds, no
 *     PostgreSQL needed.
 *
 * PGWT_TEST_EXPECT_PIE (1/0) is set by the Makefile per build so the test
 * also verifies it really IS the ELF type it claims to cover (guards
 * against toolchain defaults silently overriding the -pie/-no-pie flags).
 */
#include "discovery.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <elf.h>

static int run = 0, ok = 0;
#define CHECK(c, ...) do { \
        run++; \
        if (c) { ok++; } \
        else { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
    } while (0)

/* A global with external linkage in .data: the self-resolution target.
 * volatile + non-zero init keep it present and unmergeable. */
volatile uint32_t pgwt_test_fixture_symbol = 0xC0FFEE42u;

/* Locate tests/fixtures/ relative to this binary (cwd-independent). */
static void fixture_path(char *out, size_t outsz, const char *name)
{
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n < 0) { snprintf(out, outsz, "fixtures/%s", name); return; }
    exe[n] = '\0';
    snprintf(out, outsz, "%s/fixtures/%s", dirname(exe), name);
}

static uint64_t load_base_fixture(const char *name, const char *basename_)
{
    char path[600];
    fixture_path(path, sizeof(path), name);
    return pgwt_find_load_base_in_maps(path, basename_);
}

/* ELF type (ET_EXEC / ET_DYN) of a binary, for the PIE/non-PIE sanity check. */
static int elf_type(const char *path)
{
    Elf64_Ehdr eh;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t r = fread(&eh, 1, sizeof(eh), f);
    fclose(f);
    if (r != sizeof(eh) || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0)
        return -1;
    return eh.e_type;
}

int main(void)
{
    printf("=== test_discovery (%s build) ===\n",
           PGWT_TEST_EXPECT_PIE ? "PIE" : "non-PIE");

    /* ── 1. maps fixtures: pgwt_find_load_base_in_maps ─────────────── */

    /* EL8 non-PIE PGDG: first mapping of the binary is the r-xp text
     * segment at 0x400000 (no r--p below it). The old "first r--p" match
     * returned the rodata base 0xf5d000 here → wrong VAs → zero events
     * (the #24 symptom). */
    CHECK(load_base_fixture("maps_el8_nonpie.txt", "postgres") == 0x400000ULL,
          "EL8 non-PIE load base != 0x400000");

    /* EL9 PIE PGDG: first mapping is the r--p ELF-header page. */
    CHECK(load_base_fixture("maps_el9_pie.txt", "postgres") == 0x55d4c1000000ULL,
          "EL9 PIE load base != 0x55d4c1000000");

    /* Ubuntu PGDG deb, PIE. */
    CHECK(load_base_fixture("maps_ubuntu_pie.txt", "postgres") == 0x5560d1a00000ULL,
          "Ubuntu PIE load base != 0x5560d1a00000");

    /* Binary not mapped at all → 0 (caller fails loudly, never guesses). */
    CHECK(load_base_fixture("maps_ubuntu_pie.txt", "no_such_binary") == 0,
          "missing basename should return 0");

    /* ── CAP-4 adversarial case (fixed in Phase T4) ───────────────────
     * pg_stat_statements.so mapped BELOW the binary; its path
     * "/usr/lib/postgresql/17/lib/..." contains the substring "postgres".
     * The old whole-line strstr() match picked the .so's base
     * (0x4f2a80000000) → every symbol VA garbage → zero events, silently
     * (the #24 class). The exact pathname-field match must resolve the
     * BINARY's base. Hard assert — this was T0's pinned expected-failure. */
    CHECK(load_base_fixture("maps_ext_below_binary.txt", "postgres")
              == 0x5560d1a00000ULL,
          "CAP-4: extension .so below binary must not win (exact-basename "
          "match)");

    /* Same fixture through the FULL-path match — the path the daemon
     * actually uses (pgwt_resolve_symbol passes /proc/<pid>/exe). */
    CHECK(load_base_fixture("maps_ext_below_binary.txt",
                            "/usr/lib/postgresql/17/bin/postgres")
              == 0x5560d1a00000ULL,
          "CAP-4: full-path match resolves the binary's base");

    /* A full path that names the EXTENSION resolves the extension —
     * proving the comparison is exact-pathname, not basename-of-path. */
    CHECK(load_base_fixture("maps_ext_below_binary.txt",
                            "/usr/lib/postgresql/17/lib/pg_stat_statements.so")
              == 0x4f2a80000000ULL,
          "full-path match of the .so itself resolves the .so");

    /* Substring of a basename must NOT match: "postgres" is a substring of
     * "pg_stat_statements.so"'s path but of no basename in the EL9 fixture
     * except the binary's own. A name that is a strict substring of the
     * binary basename must find nothing. */
    CHECK(load_base_fixture("maps_el9_pie.txt", "postgre") == 0,
          "strict-substring basename must not match");

    /* ── 2. self-resolution: the #24 regression test proper ───────────
     * Resolve a known global in THIS binary through the real code path
     * (ELF symbol table + /proc/self/maps + PIE/non-PIE base arithmetic)
     * and compare against the symbol's actual address. */

    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    CHECK(n > 0, "readlink(/proc/self/exe) failed");
    if (n > 0) {
        exe[n] = '\0';
        const char *base = strrchr(exe, '/');
        base = base ? base + 1 : exe;

        /* The build flag must have produced the ELF type it claims. */
        int et = elf_type(exe);
#if PGWT_TEST_EXPECT_PIE
        CHECK(et == ET_DYN, "expected PIE (ET_DYN), got e_type=%d — "
              "-pie flag did not take effect", et);
#else
        CHECK(et == ET_EXEC, "expected non-PIE (ET_EXEC), got e_type=%d — "
              "-no-pie flag did not take effect", et);
#endif

        uint64_t sym_val = pgwt_find_symbol_offset(exe, "pgwt_test_fixture_symbol");
        CHECK(sym_val != 0, "pgwt_find_symbol_offset found nothing");

        /* Both match forms must find the load base: exact basename (fixture
         * form) and the full /proc/self/exe path (the daemon's form). */
        uint64_t load_base = pgwt_find_load_base(getpid(), base);
        CHECK(load_base != 0, "pgwt_find_load_base(self, basename) returned 0");
        CHECK(pgwt_find_load_base(getpid(), exe) == load_base,
              "full-path and basename load base disagree");

        uint64_t va = pgwt_resolve_symbol(exe, "pgwt_test_fixture_symbol",
                                          getpid());
        uint64_t actual = (uint64_t)(uintptr_t)&pgwt_test_fixture_symbol;
        CHECK(va == actual,
              "resolved VA 0x%llx != actual &symbol 0x%llx (load_base=0x%llx, "
              "sym_val=0x%llx) — the #24 load-base bug class",
              (unsigned long long)va, (unsigned long long)actual,
              (unsigned long long)load_base, (unsigned long long)sym_val);

        /* And the resolved address must read back the magic value through
         * /proc/<pid>/mem — exactly how the daemon consumes it. */
        uint64_t readback = pgwt_read_pointer(getpid(), va);
        CHECK((uint32_t)readback == 0xC0FFEE42u,
              "read-through-/proc/mem at resolved VA gave 0x%llx, not the magic",
              (unsigned long long)readback);
    }

    printf("\n%d/%d tests passed\n", ok, run);
    return (ok == run) ? 0 : 1;
}
