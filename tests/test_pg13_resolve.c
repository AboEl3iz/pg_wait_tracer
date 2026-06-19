/* test_pg13_resolve.c — PG13 MyProc-path resolution helpers:
 *   - offset table (pgwt_detect_pgproc_wait_offset)
 *   - the runtime "refuse on garbage offset" validation guard
 *     (pgwt_validate_wait_addr), exercised against THIS process's memory so it
 *     runs anywhere (no live PostgreSQL needed).
 *
 * The guard is the safety net that stops the tracer from tracing nonsense when
 * an offset is wrong (custom build): it reads the candidate wait_event_info
 * value and accepts only a CPU (0) or a known wait-event class byte (0x01..0x0B).
 */
#include "discovery.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

static int run = 0, ok = 0;
#define CHECK(c, msg) do { run++; if (c) ok++; else printf("  FAIL: %s\n", msg); } while (0)

int main(void)
{
    printf("=== test_pg13_resolve ===\n");

    /* Offset table: PG13 must be the header-derived 684; an unknown EOL/old
     * version must return 0 so the caller fails safe (refuses to attach). */
    int off13 = pgwt_detect_pgproc_wait_offset(13);
    /* On x86_64 this is 684; on other arches it returns 0 (no known layout). */
#if defined(__x86_64__)
    CHECK(off13 == 684, "PG13 offset should be 684 on x86_64");
#else
    CHECK(off13 == 0, "non-x86_64: no known offset");
#endif
    CHECK(pgwt_detect_pgproc_wait_offset(99) == 0, "unknown version -> 0");

    pid_t self = getpid();

    /* Valid values: CPU (0) and each known wait class byte must be accepted. */
    volatile uint32_t v_cpu   = 0x00000000u;
    volatile uint32_t v_lock  = 0x03000000u;   /* Lock:relation */
    volatile uint32_t v_sleep = 0x09000001u;   /* Timeout:PgSleep */
    volatile uint32_t v_io    = 0x0A00000Du;   /* IO:DataFileRead (PG13) */
    CHECK(pgwt_validate_wait_addr(self, (uint64_t)(uintptr_t)&v_cpu)   == 1, "CPU value valid");
    CHECK(pgwt_validate_wait_addr(self, (uint64_t)(uintptr_t)&v_lock)  == 1, "Lock class valid");
    CHECK(pgwt_validate_wait_addr(self, (uint64_t)(uintptr_t)&v_sleep) == 1, "Timeout class valid");
    CHECK(pgwt_validate_wait_addr(self, (uint64_t)(uintptr_t)&v_io)    == 1, "IO class valid");

    /* Garbage values (what a WRONG offset produces): class byte outside the
     * known 0x01..0x0B range must be REJECTED so the daemon refuses to attach. */
    volatile uint32_t v_garbage1 = 0xDEADBEEFu;  /* class 0xDE */
    volatile uint32_t v_garbage2 = 0x7F000000u;  /* class 0x7F (e.g. a pointer high byte) */
    volatile uint32_t v_garbage3 = 0x20000000u;  /* class 0x20 */
    CHECK(pgwt_validate_wait_addr(self, (uint64_t)(uintptr_t)&v_garbage1) == 0, "garbage class rejected");
    CHECK(pgwt_validate_wait_addr(self, (uint64_t)(uintptr_t)&v_garbage2) == 0, "high-byte pointer rejected");
    CHECK(pgwt_validate_wait_addr(self, (uint64_t)(uintptr_t)&v_garbage3) == 0, "class 0x20 rejected");

    /* Unreadable address -> -1 (transient read error, not a hard reject). */
    CHECK(pgwt_validate_wait_addr(self, 0x1) == -1, "unreadable addr -> -1");

    /* ── PG13 query-attribution offsets (Route B1) ──────────────────
     * Header-derived from postgresql13-devel (x86_64), confirmed via an
     * offsetof() probe against 13.23: QueryDesc.plannedstmt=8,
     * PlannedStmt.queryId=8, QueryDesc.sourceText=16. The BPF uprobe walks
     * QueryDesc->plannedstmt->queryId; sourceText gives the query text. */
    struct pgwt_pg13_query_offsets qo;
    int have13 = pgwt_detect_pg13_query_offsets(13, &qo);
#if defined(__x86_64__)
    CHECK(have13 == 1, "PG13 query offsets available on x86_64");
    CHECK(qo.querydesc_plannedstmt == 8, "QueryDesc.plannedstmt == 8");
    CHECK(qo.plannedstmt_queryid   == 8, "PlannedStmt.queryId == 8");
    CHECK(qo.querydesc_sourcetext  == 16, "QueryDesc.sourceText == 16");
#else
    CHECK(have13 == 0, "non-x86_64: PG13 query offsets unavailable");
#endif
    /* Unknown/unsupported version: no offsets, all zeroed, fail safe. */
    struct pgwt_pg13_query_offsets qo99;
    CHECK(pgwt_detect_pg13_query_offsets(99, &qo99) == 0, "unknown version -> 0");
    CHECK(qo99.querydesc_plannedstmt == 0 && qo99.plannedstmt_queryid == 0
          && qo99.querydesc_sourcetext == 0, "unknown version offsets zeroed");
    CHECK(pgwt_detect_pg13_query_offsets(13, NULL) == 0, "NULL out -> 0 (no crash)");

    /* ── pgss detection (gating) ────────────────────────────────────
     * This test process does NOT load pg_stat_statements, so detection on
     * our own pid must report "not loaded" (0). A bad pid -> -1 (read error).
     * The "loaded" case is covered by the live PG13 validation, where the
     * postmaster's maps contain pg_stat_statements.so. */
    CHECK(pgwt_detect_pgss_loaded(self) == 0, "pgss not loaded in this process -> 0");
    CHECK(pgwt_detect_pgss_loaded(0x7fffffff) == -1, "unreadable maps -> -1");

    printf("\n%d/%d tests passed\n", ok, run);
    return (ok == run) ? 0 : 1;
}
