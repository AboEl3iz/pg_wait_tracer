/* test_concurrent_rw.c — Tests concurrent read-write on current.trace
 *
 * Forks a writer and reader process that operate on the same trace file
 * simultaneously. Verifies that the meta file approach eliminates race
 * conditions between daemon writes and server reads.
 *
 * Writer: appends blocks every 100ms, writes meta file after each flush.
 * Reader: opens file every 200ms, reads committed blocks, verifies data.
 *
 * Build: make -C tests test_concurrent_rw
 * Run:   tests/test_concurrent_rw
 */
#include "event_writer.h"
#include "event_reader.h"
#include "pg_wait_tracer.h"
#include "wait_event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

#define TEST_DIR "/tmp/pgwt_concurrent_rw_test"
#define NUM_WRITE_CYCLES 30
#define EVENTS_PER_BLOCK 5000  /* > PGWT_BLOCK_EVENTS to force multiple blocks */
#define WRITE_INTERVAL_MS 100
#define READ_INTERVAL_MS 200
#define TEST_DURATION_MS 5000

static int passed = 0;
static int failed = 0;

static void check(int cond, const char *msg)
{
    if (cond) {
        printf("  PASS: %s\n", msg);
        passed++;
    } else {
        printf("  FAIL: %s\n", msg);
        failed++;
    }
}

/* Writer process: appends blocks to current.trace with meta file */
static void writer_process(void)
{
    pgwt_init_event_names(18);

    struct pgwt_event_writer w;
    int ret = pgwt_writer_init(&w, TEST_DIR, 18, 0, NULL);
    if (ret != 0) {
        fprintf(stderr, "writer: init failed\n");
        _exit(1);
    }

    /* Generate events with realistic timestamps */
    uint64_t ts = 1000000000000ULL;  /* 1000s in ns */
    uint32_t pid = 1000;

    for (int cycle = 0; cycle < NUM_WRITE_CYCLES; cycle++) {
        /* Push enough events to fill a block */
        for (int i = 0; i < EVENTS_PER_BLOCK; i++) {
            struct pgwt_trace_event ev = {
                .timestamp_ns = ts,
                .pid = pid + (i % 4),
                .old_event = (i % 2 == 0) ? 0 : 0x0A000015,  /* CPU or IO:DataFileRead */
                .new_event = (i % 2 == 0) ? 0x0A000015 : 0,
                .duration_ns = 1000 + (i * 100),
                .query_id = 12345,
            };
            pgwt_writer_push_event(&w, &ev);
            ts += ev.duration_ns;
        }

        /* Sleep to simulate real daemon timing */
        usleep(WRITE_INTERVAL_MS * 1000);
    }

    pgwt_writer_close(&w);
    pgwt_writer_destroy(&w);
    _exit(0);
}

/* Reader process: repeatedly reads current.trace and verifies data */
static void reader_process(void)
{
    pgwt_init_event_names(18);

    int read_count = 0;
    int success_count = 0;
    int garbage_ts_count = 0;
    int blocks_seen_max = 0;

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (1) {
        /* Check timeout */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 +
                          (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed_ms >= TEST_DURATION_MS)
            break;

        /* Try to read the trace file */
        char path[512];
        snprintf(path, sizeof(path), "%s/current.trace", TEST_DIR);

        struct pgwt_event_reader reader;
        int ret = pgwt_reader_open(&reader, path);
        read_count++;

        if (ret == 0) {
            success_count++;

            if (reader.num_blocks > blocks_seen_max)
                blocks_seen_max = reader.num_blocks;

            /* Verify all block timestamps are sane */
            for (int b = 0; b < reader.num_blocks; b++) {
                uint64_t ts = reader.block_index[b].timestamp_ns;
                /* Timestamps should be in our test range (~1000s) */
                if (ts < 500000000000ULL || ts > 2000000000000ULL) {
                    garbage_ts_count++;
                    fprintf(stderr,
                            "reader: block %d has garbage timestamp %llu\n",
                            b, (unsigned long long)ts);
                }
            }

            /* Try decoding the last block */
            if (reader.num_blocks > 0) {
                struct pgwt_trace_event events[PGWT_BLOCK_EVENTS];
                int n = pgwt_reader_decode_block(&reader,
                            reader.num_blocks - 1, events, PGWT_BLOCK_EVENTS);
                if (n < 0) {
                    fprintf(stderr,
                            "reader: decode of last block failed\n");
                }
            }

            pgwt_reader_close(&reader);
        }

        usleep(READ_INTERVAL_MS * 1000);
    }

    /* Write results to a temp file for the parent to read */
    char result_path[512];
    snprintf(result_path, sizeof(result_path), "%s/reader_results.txt",
             TEST_DIR);
    FILE *rf = fopen(result_path, "w");
    if (rf) {
        fprintf(rf, "%d %d %d %d\n",
                read_count, success_count, garbage_ts_count, blocks_seen_max);
        fclose(rf);
    }
    _exit(0);
}

int main(void)
{
    printf("=== test_concurrent_rw ===\n");

    /* Clean up from previous runs */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", TEST_DIR, TEST_DIR);
    system(cmd);

    /* Fork writer */
    pid_t writer_pid = fork();
    if (writer_pid == 0) {
        writer_process();
        _exit(0);
    }

    /* Give writer a head start (1 block) */
    usleep(200 * 1000);

    /* Fork reader */
    pid_t reader_pid = fork();
    if (reader_pid == 0) {
        reader_process();
        _exit(0);
    }

    /* Wait for both */
    int writer_status, reader_status;
    waitpid(writer_pid, &writer_status, 0);
    waitpid(reader_pid, &reader_status, 0);

    check(WIFEXITED(writer_status) && WEXITSTATUS(writer_status) == 0,
          "writer process exited cleanly");
    check(WIFEXITED(reader_status) && WEXITSTATUS(reader_status) == 0,
          "reader process exited cleanly");

    /* Read reader results */
    char result_path[512];
    snprintf(result_path, sizeof(result_path), "%s/reader_results.txt",
             TEST_DIR);
    FILE *rf = fopen(result_path, "r");
    check(rf != NULL, "reader produced results file");

    if (rf) {
        int reads, successes, garbage, max_blocks;
        if (fscanf(rf, "%d %d %d %d",
                   &reads, &successes, &garbage, &max_blocks) == 4) {

            printf("  reader: %d reads, %d successful, %d garbage timestamps, "
                   "max %d blocks\n", reads, successes, garbage, max_blocks);

            check(reads >= 10,
                  "reader did >= 10 read attempts");
            check(successes >= 5,
                  "reader had >= 5 successful reads");
            check(garbage == 0,
                  "ZERO garbage timestamps (meta file works)");
            check(max_blocks >= 5,
                  "reader saw >= 5 blocks (writer produced data)");
        }
        fclose(rf);
    }

    /* Check meta file exists */
    char meta_path[512];
    snprintf(meta_path, sizeof(meta_path), "%s/current.trace.meta", TEST_DIR);
    struct stat st;
    check(stat(meta_path, &st) == 0, "current.trace.meta exists");

    if (stat(meta_path, &st) == 0) {
        FILE *mf = fopen(meta_path, "r");
        int count = 0;
        if (mf && fscanf(mf, "%d", &count) == 1) {
            check(count > 0, "meta file has positive block count");
            printf("  meta file: %d committed blocks\n", count);
        }
        if (mf) fclose(mf);
    }

    /* Cleanup */
    snprintf(cmd, sizeof(cmd), "rm -rf %s", TEST_DIR);
    system(cmd);

    printf("\n%d/%d tests passed\n", passed, passed + failed);
    return failed > 0 ? 1 : 0;
}
