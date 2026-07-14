/* test_golden_fixture.c — On-disk format compatibility gate (Phase T7 / TST-10)
 *
 * The other format tests (test_trace_v2, test_durability) are SAME-CODE
 * round-trips: they write with today's writer and read with today's reader, so
 * they cannot catch a change that silently alters the on-disk bytes while
 * keeping the write/read pair self-consistent. This test decodes a COMMITTED
 * golden trace — bytes generated once at the current format revision and frozen
 * in git — and asserts the current reader recovers the exact pinned event
 * stream (a checksum over every decoded column of every block). If a future
 * change alters how existing files decode, this fails.
 *
 * The fixture lives in tests/fixtures/golden/rev<N>/ where <N> is the trace
 * format version (PGWT_TRACE_VERSION). On an INTENTIONAL format bump, do NOT
 * edit this test's expected value or overwrite the fixture: ADD a new
 * rev<N+1>/ fixture (see tests/fixtures/golden/README.md) so both the old and
 * the new on-disk shape stay covered — a reader must keep decoding files
 * written by every shipped version.
 *
 * Built with -DPGWT_SERVER against the server objects (no BPF skeleton),
 * runs anywhere.
 */
#include "event_reader.h"
#include "event_writer.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <libgen.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, fmt, ...) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); } \
} while (0)

/* FNV-1a over a byte range, folded into a running 64-bit accumulator. */
static uint64_t fnv1a(uint64_t h, const void *data, size_t len)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* Fold one field so the checksum is layout-independent (no struct padding). */
static uint64_t mix_u64(uint64_t h, uint64_t v) { return fnv1a(h, &v, sizeof(v)); }
static uint64_t mix_u32(uint64_t h, uint32_t v) { return fnv1a(h, &v, sizeof(v)); }

/* Committed golden trace directory, relative to this test binary (which lives
 * in tests/). Resolved against the executable's own location so the test works
 * from any CWD — `make -C tests check` runs it from tests/, but run_all.sh
 * invokes it by absolute path from the caller's CWD. */
#define GOLDEN_REL "fixtures/golden/rev2/trace"

/* Fill `out` with the fixture dir resolved next to argv0's directory. */
static void golden_dir(const char *argv0, char *out, size_t outsz)
{
    char buf[600];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    const char *base;
    char argv0copy[600];
    if (n > 0) {
        buf[n] = '\0';
        base = dirname(buf);   /* modifies buf in place */
    } else {
        snprintf(argv0copy, sizeof(argv0copy), "%s", argv0);
        base = dirname(argv0copy);
    }
    snprintf(out, outsz, "%s/%s", base, GOLDEN_REL);
}

/* Pinned decode of the rev2 golden fixture. Regenerate ONLY when intentionally
 * bumping the format (then add a NEW rev dir + a new expected value — never
 * silently change these):
 *
 *   total blocks, total events, and the FNV-1a checksum over every block's
 *   (block_type, sample_period_ns) header field and every decoded record's
 *   (timestamp_ns, pid, old_event, new_event, duration_ns, query_id, flags).
 *
 * The concrete numbers are asserted below; they are filled in from the first
 * run's report (printed when a mismatch occurs) and then frozen.
 */
#define GOLDEN_REV2_BLOCKS   2
#define GOLDEN_REV2_EVENTS   14
#define GOLDEN_REV2_CHECKSUM 0x7d8b576cb43cf60cULL

static void test_golden_rev2(const char *argv0)
{
    char dir[700];
    golden_dir(argv0, dir, sizeof(dir));
    printf("--- golden rev2 decode + checksum (%s) ---\n", dir);

    char trace_path[800];
    snprintf(trace_path, sizeof(trace_path), "%s/current.trace", dir);

    struct pgwt_event_reader r;
    if (pgwt_reader_open(&r, trace_path) != 0) {
        printf("  FAIL: cannot open golden trace %s "
               "(regenerate — see fixtures/golden/README.md)\n",
               trace_path);
        tests_run++;
        return;
    }

    CHECK(r.header.version == PGWT_TRACE_VERSION,
          "header version %u == %u", r.header.version, PGWT_TRACE_VERSION);

    uint64_t h = 0xcbf29ce484222325ULL;   /* FNV-1a offset basis */
    int total_events = 0;

    struct pgwt_trace_event evbuf[PGWT_BLOCK_EVENTS];
    for (int b = 0; b < r.num_blocks; b++) {
        struct pgwt_block_info info;
        int n = pgwt_reader_decode_block_info(&r, b, evbuf,
                                              PGWT_BLOCK_EVENTS, &info);
        if (n < 0) {
            printf("  FAIL: decode_block(%d) failed\n", b);
            tests_run++;
            continue;
        }
        h = mix_u32(h, (uint32_t)info.block_type);
        h = mix_u64(h, info.sample_period_ns);
        for (int i = 0; i < n; i++) {
            const struct pgwt_trace_event *e = &evbuf[i];
            h = mix_u64(h, e->timestamp_ns);
            h = mix_u32(h, e->pid);
            h = mix_u32(h, e->old_event);
            h = mix_u32(h, e->new_event);
            h = mix_u64(h, e->duration_ns);
            h = mix_u64(h, e->query_id);
            h = mix_u32(h, e->flags);
        }
        total_events += n;
    }

    printf("  decoded: blocks=%d events=%d checksum=0x%016llxULL\n",
           r.num_blocks, total_events, (unsigned long long)h);

    CHECK(r.num_blocks == GOLDEN_REV2_BLOCKS,
          "block count %d == %d", r.num_blocks, GOLDEN_REV2_BLOCKS);
    CHECK(total_events == GOLDEN_REV2_EVENTS,
          "event count %d == %d", total_events, GOLDEN_REV2_EVENTS);
    CHECK(h == GOLDEN_REV2_CHECKSUM,
          "checksum 0x%016llx == 0x%016llx (on-disk decode drifted — if this "
          "is an INTENTIONAL format change, add a new rev fixture; do NOT edit "
          "this value)", (unsigned long long)h,
          (unsigned long long)GOLDEN_REV2_CHECKSUM);

    pgwt_reader_close(&r);
}

int main(int argc, char **argv)
{
    printf("=== test_golden_fixture ===\n");
    test_golden_rev2(argc > 0 ? argv[0] : "");
    printf("\n%d/%d checks passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
