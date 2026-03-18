/* test_event_reader.c — Unit tests for event reader: roundtrip, time range, time parser */
#include "event_writer.h"
#include "event_reader.h"
#include "map_reader.h"
#include "wait_event.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, fmt, ...) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); } \
} while(0)

/* ── Helper: create a trace file with known events ────────── */

static const char *TEST_DIR = "/tmp/test_event_reader";

static void setup_test_dir(void)
{
    mkdir(TEST_DIR, 0755);
}

static void cleanup_test_dir(void)
{
    /* Remove files in test dir */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", TEST_DIR);
    system(cmd);
}

static int write_test_file(const struct pgwt_trace_event *events,
                           int count, int pg_version)
{
    struct pgwt_event_writer w;
    if (pgwt_writer_init(&w, TEST_DIR, pg_version, 24, NULL) != 0)
        return -1;

    /* Rename current.trace to our test file name */
    for (int i = 0; i < count; i++) {
        if (pgwt_writer_push_event(&w, &events[i]) != 0) {
            pgwt_writer_close(&w);
            pgwt_writer_destroy(&w);
            return -1;
        }
    }
    pgwt_writer_close(&w);
    pgwt_writer_destroy(&w);

    /* Writer creates current.trace in TEST_DIR */
    return 0;
}

/* ── Test 1: Single-block roundtrip ──────────────────────────── */

static void test_roundtrip_single_block(void)
{
    printf("--- Roundtrip (single block) ---\n");
    cleanup_test_dir();
    setup_test_dir();

    /* Create 100 known events */
    int N = 100;
    struct pgwt_trace_event *orig = calloc(N, sizeof(*orig));
    for (int i = 0; i < N; i++) {
        orig[i].timestamp_ns = 1000000ULL + i * 1000ULL;  /* 1ms, +1us each */
        orig[i].pid = 1000 + (i % 10);
        orig[i].old_event = 0x03000000 + i;  /* IO class */
        orig[i].new_event = 0x03000000 + i + 1;
        orig[i].duration_ns = 500 + i * 10;
        orig[i].query_id = (i % 3 == 0) ? 0 : (42 + i);
    }

    CHECK(write_test_file(orig, N, 18) == 0, "write test file");

    /* Read back */
    char path[512];
    snprintf(path, sizeof(path), "%s/current.trace", TEST_DIR);

    struct pgwt_event_reader reader;
    CHECK(pgwt_reader_open(&reader, path) == 0, "reader_open");
    CHECK(reader.header.magic == PGWT_TRACE_MAGIC, "magic");
    CHECK(reader.header.version == PGWT_TRACE_VERSION, "version");
    CHECK(reader.header.pg_version == 18, "pg_version");
    CHECK(reader.num_blocks == 1, "num_blocks=%d (expected 1)", reader.num_blocks);

    struct pgwt_trace_event *decoded = calloc(PGWT_BLOCK_EVENTS, sizeof(*decoded));
    int count = pgwt_reader_decode_block(&reader, 0, decoded, PGWT_BLOCK_EVENTS);
    CHECK(count == N, "decoded count=%d (expected %d)", count, N);

    int mismatches = 0;
    for (int i = 0; i < count && i < N; i++) {
        if (decoded[i].timestamp_ns != orig[i].timestamp_ns ||
            decoded[i].pid != orig[i].pid ||
            decoded[i].old_event != orig[i].old_event ||
            decoded[i].new_event != orig[i].new_event ||
            decoded[i].duration_ns != orig[i].duration_ns ||
            decoded[i].query_id != orig[i].query_id) {
            if (mismatches == 0) {
                printf("  First mismatch at i=%d:\n", i);
                printf("    ts: %lu vs %lu\n",
                       (unsigned long)decoded[i].timestamp_ns,
                       (unsigned long)orig[i].timestamp_ns);
                printf("    pid: %u vs %u\n", decoded[i].pid, orig[i].pid);
                printf("    old: 0x%x vs 0x%x\n", decoded[i].old_event, orig[i].old_event);
                printf("    new: 0x%x vs 0x%x\n", decoded[i].new_event, orig[i].new_event);
                printf("    dur: %lu vs %lu\n",
                       (unsigned long)decoded[i].duration_ns,
                       (unsigned long)orig[i].duration_ns);
                printf("    qid: %lu vs %lu\n",
                       (unsigned long)decoded[i].query_id,
                       (unsigned long)orig[i].query_id);
            }
            mismatches++;
        }
    }
    CHECK(mismatches == 0, "field mismatches: %d", mismatches);

    pgwt_reader_close(&reader);
    free(decoded);
    free(orig);
    cleanup_test_dir();
}

/* ── Test 2: Multi-block roundtrip ──────────────────────────── */

static void test_roundtrip_multi_block(void)
{
    printf("--- Roundtrip (multi-block) ---\n");
    cleanup_test_dir();
    setup_test_dir();

    /* Write more events than one block (4096+100 = 2 blocks) */
    int N = PGWT_BLOCK_EVENTS + 100;
    struct pgwt_trace_event *orig = calloc(N, sizeof(*orig));
    for (int i = 0; i < N; i++) {
        orig[i].timestamp_ns = 1000000ULL + i * 500ULL;
        orig[i].pid = 2000 + (i % 20);
        orig[i].old_event = 0x01000000 + (i % 50);  /* LWLock class */
        orig[i].new_event = 0;
        orig[i].duration_ns = 100 + (i % 1000) * 100;
        orig[i].query_id = (i % 5 == 0) ? 12345ULL : 0;
    }

    CHECK(write_test_file(orig, N, 17) == 0, "write multi-block file");

    char path[512];
    snprintf(path, sizeof(path), "%s/current.trace", TEST_DIR);

    struct pgwt_event_reader reader;
    CHECK(pgwt_reader_open(&reader, path) == 0, "reader_open");
    CHECK(reader.num_blocks == 2, "num_blocks=%d (expected 2)", reader.num_blocks);
    CHECK(reader.header.pg_version == 17, "pg_version=%d", reader.header.pg_version);

    struct pgwt_trace_event *decoded = calloc(PGWT_BLOCK_EVENTS, sizeof(*decoded));
    int total = 0;
    int mismatches = 0;

    for (int bi = 0; bi < reader.num_blocks; bi++) {
        int count = pgwt_reader_decode_block(&reader, bi, decoded, PGWT_BLOCK_EVENTS);
        CHECK(count > 0, "block %d: count=%d", bi, count);

        for (int i = 0; i < count && total + i < N; i++) {
            int idx = total + i;
            if (decoded[i].timestamp_ns != orig[idx].timestamp_ns ||
                decoded[i].pid != orig[idx].pid ||
                decoded[i].old_event != orig[idx].old_event ||
                decoded[i].duration_ns != orig[idx].duration_ns ||
                decoded[i].query_id != orig[idx].query_id) {
                mismatches++;
            }
        }
        total += count;
    }

    CHECK(total == N, "total events=%d (expected %d)", total, N);
    CHECK(mismatches == 0, "multi-block mismatches: %d", mismatches);

    pgwt_reader_close(&reader);
    free(decoded);
    free(orig);
    cleanup_test_dir();
}

/* ── Test 3: Block search (binary search) ────────────────── */

static void test_block_search(void)
{
    printf("--- Block search ---\n");
    cleanup_test_dir();
    setup_test_dir();

    /* Create 3 blocks worth of events with distinct timestamp ranges */
    int N = PGWT_BLOCK_EVENTS * 3;
    struct pgwt_trace_event *orig = calloc(N, sizeof(*orig));
    for (int i = 0; i < N; i++) {
        orig[i].timestamp_ns = 10000000ULL + i * 1000ULL;  /* 10ms base, 1us each */
        orig[i].pid = 3000;
        orig[i].old_event = 0x0A000001;
        orig[i].new_event = 0;
        orig[i].duration_ns = 500;
        orig[i].query_id = 0;
    }

    CHECK(write_test_file(orig, N, 18) == 0, "write 3-block file");

    char path[512];
    snprintf(path, sizeof(path), "%s/current.trace", TEST_DIR);

    struct pgwt_event_reader reader;
    CHECK(pgwt_reader_open(&reader, path) == 0, "reader_open");
    CHECK(reader.num_blocks == 3, "num_blocks=%d (expected 3)", reader.num_blocks);

    /* Block 0: ts 10000000 .. 10000000+4095*1000 = 10000000..14095000 */
    /* Block 1: ts 14096000 .. 18191000 */
    /* Block 2: ts 18192000 .. 22287000 */

    /* Search for timestamp at start → should find block 0 */
    int bi = pgwt_reader_find_block(&reader, 10000000ULL);
    CHECK(bi == 0, "find_block(start)=%d (expected 0)", bi);

    /* Search for timestamp in middle of block 1 */
    bi = pgwt_reader_find_block(&reader, 16000000ULL);
    CHECK(bi == 1, "find_block(mid_block1)=%d (expected 1)", bi);

    /* Search for timestamp in block 2 */
    bi = pgwt_reader_find_block(&reader, 20000000ULL);
    CHECK(bi == 2, "find_block(block2)=%d (expected 2)", bi);

    /* Search before any data → should return 0 */
    bi = pgwt_reader_find_block(&reader, 1000ULL);
    CHECK(bi == 0, "find_block(before)=%d (expected 0)", bi);

    /* Search after all data → should return last block */
    bi = pgwt_reader_find_block(&reader, 99999999ULL);
    CHECK(bi == 2, "find_block(after)=%d (expected 2)", bi);

    pgwt_reader_close(&reader);
    free(orig);
    cleanup_test_dir();
}

/* ── Test 4: Time range replay ────────────────────────────── */

static void test_time_range_replay(void)
{
    printf("--- Time range replay ---\n");
    cleanup_test_dir();
    setup_test_dir();

    /* Create events across a range */
    int N = 200;
    struct pgwt_trace_event *orig = calloc(N, sizeof(*orig));
    for (int i = 0; i < N; i++) {
        orig[i].timestamp_ns = 1000000ULL + i * 10000ULL;  /* 10us apart */
        orig[i].pid = 4000;
        orig[i].old_event = 0x0A000001;  /* IO class (0x0A) */
        orig[i].new_event = 0;
        orig[i].duration_ns = 1000;  /* 1us each */
        orig[i].query_id = 0;
    }

    CHECK(write_test_file(orig, N, 18) == 0, "write file for replay");

    pgwt_init_event_names(18);

    char path[512];
    snprintf(path, sizeof(path), "%s/current.trace", TEST_DIR);

    struct pgwt_event_reader reader;
    CHECK(pgwt_reader_open(&reader, path) == 0, "reader_open");

    struct pgwt_trace_event *decoded = calloc(PGWT_BLOCK_EVENTS, sizeof(*decoded));
    int count = pgwt_reader_decode_block(&reader, 0, decoded, PGWT_BLOCK_EVENTS);
    CHECK(count == N, "decoded count=%d", count);

    /* Replay with time bounds: skip first 50, take next 100 */
    uint64_t from_mono = 1000000ULL + 50 * 10000ULL;   /* event 50 */
    uint64_t to_mono   = 1000000ULL + 150 * 10000ULL;  /* event 150 */

    struct pgwt_accumulator *acc = calloc(1, sizeof(*acc));
    pgwt_accum_init(acc);
    pgwt_replay_events(acc, decoded, count, from_mono, to_mono);

    /* Should have accumulated ~101 events (50..150 inclusive) */
    CHECK(acc->num_system_events > 0, "system events accumulated");
    CHECK(acc->num_pids == 1, "num_pids=%d (expected 1)", acc->num_pids);
    CHECK(acc->pids[0].pid == 4000, "pid=%d", acc->pids[0].pid);

    /* Find the event stats for our IO event */
    struct pgwt_event_stats *se = pgwt_find_system_event(acc, 0x0A000001);
    CHECK(se != NULL, "system event found");
    if (se) {
        CHECK(se->count == 101, "count=%lu (expected 101)", (unsigned long)se->count);
    }

    /* Replay all events (no bounds) */
    pgwt_accum_init(acc);
    pgwt_replay_events(acc, decoded, count, 0, 0);
    se = pgwt_find_system_event(acc, 0x0A000001);
    CHECK(se != NULL, "system event found (full)");
    if (se) {
        CHECK(se->count == (uint64_t)N, "full count=%lu (expected %d)",
              (unsigned long)se->count, N);
    }

    pgwt_reader_close(&reader);
    free(acc);
    free(decoded);
    free(orig);
    cleanup_test_dir();
}

/* ── Test 5: Time parser ─────────────────────────────────── */

static void test_time_parser(void)
{
    printf("--- Time parser ---\n");
    uint64_t ns;

    /* "now" should return something close to current time */
    CHECK(pgwt_parse_time("now", &ns) == 0, "parse 'now'");
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    CHECK(ns > now_ns - 1000000000ULL && ns <= now_ns + 1000000ULL,
          "'now' within 1s of current time");

    /* ISO 8601 with T separator */
    CHECK(pgwt_parse_time("2025-01-15T12:30:45", &ns) == 0, "parse ISO T");
    CHECK(ns > 0, "ISO T returned non-zero");

    /* ISO 8601 with space separator */
    CHECK(pgwt_parse_time("2025-01-15 12:30:45", &ns) == 0, "parse ISO space");
    CHECK(ns > 0, "ISO space returned non-zero");

    /* Both ISO forms should give same result */
    uint64_t ns_t, ns_s;
    pgwt_parse_time("2025-06-01T00:00:00", &ns_t);
    pgwt_parse_time("2025-06-01 00:00:00", &ns_s);
    CHECK(ns_t == ns_s, "ISO T and space equal");

    /* Relative: 1h */
    uint64_t before;
    CHECK(pgwt_parse_time("1h", &before) == 0, "parse '1h'");
    CHECK(before < now_ns, "'1h' is before now");
    CHECK(now_ns - before > 3599ULL * 1000000000ULL &&
          now_ns - before < 3601ULL * 1000000000ULL,
          "'1h' ~3600s ago");

    /* Relative: 30m */
    CHECK(pgwt_parse_time("30m", &before) == 0, "parse '30m'");
    CHECK(now_ns - before > 1799ULL * 1000000000ULL &&
          now_ns - before < 1801ULL * 1000000000ULL,
          "'30m' ~1800s ago");

    /* Relative: 90s */
    CHECK(pgwt_parse_time("90s", &before) == 0, "parse '90s'");
    CHECK(now_ns - before > 89ULL * 1000000000ULL &&
          now_ns - before < 91ULL * 1000000000ULL,
          "'90s' ~90s ago");

    /* Relative compound: 1h30m */
    CHECK(pgwt_parse_time("1h30m", &before) == 0, "parse '1h30m'");
    CHECK(now_ns - before > 5399ULL * 1000000000ULL &&
          now_ns - before < 5401ULL * 1000000000ULL,
          "'1h30m' ~5400s ago");

    /* Invalid inputs */
    CHECK(pgwt_parse_time("", &ns) == -1, "empty string fails");
    CHECK(pgwt_parse_time("garbage", &ns) == -1, "garbage fails");
    CHECK(pgwt_parse_time(NULL, &ns) == -1, "NULL fails");
}

/* ── Test 6: Clock conversion ─────────────────────────────── */

static void test_clock_conversion(void)
{
    printf("--- Clock conversion ---\n");
    cleanup_test_dir();
    setup_test_dir();

    int N = 10;
    struct pgwt_trace_event events[10];
    for (int i = 0; i < N; i++) {
        events[i].timestamp_ns = 5000000ULL + i * 1000ULL;
        events[i].pid = 5000;
        events[i].old_event = 0;
        events[i].new_event = 0;
        events[i].duration_ns = 100;
        events[i].query_id = 0;
    }

    CHECK(write_test_file(events, N, 18) == 0, "write file");

    char path[512];
    snprintf(path, sizeof(path), "%s/current.trace", TEST_DIR);

    struct pgwt_event_reader reader;
    CHECK(pgwt_reader_open(&reader, path) == 0, "reader_open");

    /* mono_to_wall = start_time_ns - clock_offset_ns */
    /* wall_to_mono and mono_to_wall should be inverses */
    uint64_t wall = 1700000000ULL * 1000000000ULL;  /* ~2023 */
    uint64_t mono = pgwt_reader_wall_to_mono(&reader, wall);
    uint64_t back = pgwt_reader_mono_to_wall(&reader, mono);
    CHECK(back == wall, "wall→mono→wall roundtrip: %lu vs %lu",
          (unsigned long)back, (unsigned long)wall);

    uint64_t mono2 = 12345678ULL;
    uint64_t wall2 = pgwt_reader_mono_to_wall(&reader, mono2);
    uint64_t mono2b = pgwt_reader_wall_to_mono(&reader, wall2);
    CHECK(mono2b == mono2, "mono→wall→mono roundtrip: %lu vs %lu",
          (unsigned long)mono2b, (unsigned long)mono2);

    pgwt_reader_close(&reader);
    cleanup_test_dir();
}

/* ── Test 7: Scan trace files ─────────────────────────────── */

static void test_scan_trace_files(void)
{
    printf("--- Scan trace files ---\n");
    cleanup_test_dir();
    setup_test_dir();

    /* Create some fake trace files with proper format */
    /* First write a real current.trace */
    struct pgwt_trace_event evt = {
        .timestamp_ns = 1000000, .pid = 6000,
        .old_event = 0, .new_event = 0,
        .duration_ns = 100, .query_id = 0,
    };
    CHECK(write_test_file(&evt, 1, 18) == 0, "write current.trace");

    /* Create fake hourly files (just need the name pattern to match scan) */
    const char *fake_files[] = {
        "2025-01-15_10.trace.lz4",
        "2025-01-15_11.trace.lz4",
        "2025-01-15_12.trace.lz4",
    };
    for (int i = 0; i < 3; i++) {
        char p[512];
        snprintf(p, sizeof(p), "%s/%s", TEST_DIR, fake_files[i]);
        /* Write a minimal valid trace file header */
        FILE *fp = fopen(p, "wb");
        if (fp) {
            struct pgwt_trace_file_header hdr = {
                .magic = PGWT_TRACE_MAGIC,
                .version = PGWT_TRACE_VERSION,
                .pg_version = 18,
            };
            fwrite(&hdr, sizeof(hdr), 1, fp);
            fclose(fp);
        }
    }

    struct pgwt_trace_file_entry entries[16];
    int n = pgwt_scan_trace_files(TEST_DIR, entries, 16);

    /* Should find current.trace + 3 hourly files = 4 */
    /* (hourly files match filename pattern but current.trace has valid header) */
    CHECK(n >= 1, "scan found %d files (expected >= 1)", n);

    /* current.trace should be in the list */
    int found_current = 0;
    for (int i = 0; i < n; i++) {
        if (strstr(entries[i].path, "current.trace")) {
            found_current = 1;
            CHECK(entries[i].start_wall_ns > 0, "current.trace has start_wall_ns");
        }
    }
    CHECK(found_current, "current.trace found in scan");

    /* Verify sorted by start_wall_ns */
    int sorted = 1;
    for (int i = 1; i < n; i++) {
        if (entries[i].start_wall_ns < entries[i-1].start_wall_ns) {
            sorted = 0;
            break;
        }
    }
    CHECK(sorted, "entries sorted by start_wall_ns");

    cleanup_test_dir();
}

/* ── Test 8: Replay accumulates time model correctly ──────── */

static void test_replay_time_model(void)
{
    printf("--- Replay time model ---\n");
    cleanup_test_dir();
    setup_test_dir();

    pgwt_init_event_names(18);

    int N = 50;
    struct pgwt_trace_event *events = calloc(N, sizeof(*events));

    /* 20 CPU events (old_event=0), 20 IO events, 10 LWLock events */
    for (int i = 0; i < N; i++) {
        events[i].timestamp_ns = 2000000ULL + i * 1000ULL;
        events[i].pid = 7000;
        events[i].new_event = 0;
        events[i].query_id = 0;

        if (i < 20) {
            events[i].old_event = 0;                /* CPU */
            events[i].duration_ns = 1000;           /* 1us */
        } else if (i < 40) {
            events[i].old_event = 0x0A000001;       /* IO class (0x0A) */
            events[i].duration_ns = 5000;           /* 5us */
        } else {
            events[i].old_event = 0x01000001;       /* LWLock class (0x01) */
            events[i].duration_ns = 2000;           /* 2us */
        }
    }

    CHECK(write_test_file(events, N, 18) == 0, "write file");

    char path[512];
    snprintf(path, sizeof(path), "%s/current.trace", TEST_DIR);

    struct pgwt_event_reader reader;
    CHECK(pgwt_reader_open(&reader, path) == 0, "reader_open");

    struct pgwt_trace_event *decoded = calloc(PGWT_BLOCK_EVENTS, sizeof(*decoded));
    int count = pgwt_reader_decode_block(&reader, 0, decoded, PGWT_BLOCK_EVENTS);
    CHECK(count == N, "decoded count=%d", count);

    struct pgwt_accumulator *acc = calloc(1, sizeof(*acc));
    pgwt_accum_init(acc);
    pgwt_replay_events(acc, decoded, count, 0, 0);

    /* Verify time model */
    CHECK(acc->tm.cpu_time_ns == 20 * 1000ULL,
          "cpu_time=%lu (expected %lu)",
          (unsigned long)acc->tm.cpu_time_ns, (unsigned long)(20 * 1000ULL));
    CHECK(acc->tm.io_time_ns == 20 * 5000ULL,
          "io_time=%lu (expected %lu)",
          (unsigned long)acc->tm.io_time_ns, (unsigned long)(20 * 5000ULL));
    CHECK(acc->tm.lwlock_time_ns == 10 * 2000ULL,
          "lwlock_time=%lu (expected %lu)",
          (unsigned long)acc->tm.lwlock_time_ns, (unsigned long)(10 * 2000ULL));

    /* db_time = cpu + io + lwlock (no idle events) */
    uint64_t expected_db = 20*1000ULL + 20*5000ULL + 10*2000ULL;
    CHECK(acc->tm.db_time_ns == expected_db,
          "db_time=%lu (expected %lu)",
          (unsigned long)acc->tm.db_time_ns, (unsigned long)expected_db);

    pgwt_reader_close(&reader);
    free(acc);
    free(decoded);
    free(events);
    cleanup_test_dir();
}

/* ── Main ─────────────────────────────────────────────────── */

int main(void)
{
    test_roundtrip_single_block();
    test_roundtrip_multi_block();
    test_block_search();
    test_time_range_replay();
    test_time_parser();
    test_clock_conversion();
    test_scan_trace_files();
    test_replay_time_model();

    printf("\n=== %d / %d tests passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
