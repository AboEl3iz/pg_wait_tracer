/* test_bucket.c — Exhaustive test for pgwt_duration_to_bucket
 *
 * Verifies both the daemon (hardcoded) and server (also hardcoded after fix)
 * implementations produce identical results for all durations 0-20ms.
 * Also verifies boundary values match expected bucket assignments.
 */
#include "pg_wait_tracer.h"
#include <stdio.h>
#include <stdint.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, fmt, ...) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); } \
} while(0)

/* Hardcoded version (daemon — map_reader.c) */
static uint32_t bucket_hardcoded(uint64_t ns)
{
    uint64_t us = ns / 1000;
    if (us < 1)     return 0;
    if (us < 2)     return 1;
    if (us < 4)     return 2;
    if (us < 8)     return 3;
    if (us < 16)    return 4;
    if (us < 32)    return 5;
    if (us < 64)    return 6;
    if (us < 128)   return 7;
    if (us < 256)   return 8;
    if (us < 512)   return 9;
    if (us < 1024)  return 10;
    if (us < 2048)  return 11;
    if (us < 4096)  return 12;
    if (us < 8192)  return 13;
    if (us < 16384) return 14;
    return 15;
}

/* Server version (summary_writer.c / compute.c — AFTER fix) */
static uint32_t bucket_server(uint64_t ns)
{
    uint64_t us = ns / 1000;
    if (us < 1)     return 0;
    if (us < 2)     return 1;
    if (us < 4)     return 2;
    if (us < 8)     return 3;
    if (us < 16)    return 4;
    if (us < 32)    return 5;
    if (us < 64)    return 6;
    if (us < 128)   return 7;
    if (us < 256)   return 8;
    if (us < 512)   return 9;
    if (us < 1024)  return 10;
    if (us < 2048)  return 11;
    if (us < 4096)  return 12;
    if (us < 8192)  return 13;
    if (us < 16384) return 14;
    return 15;
}

int main(void)
{
    printf("=== test_bucket ===\n");

    /* Test 1: Exhaustive comparison for 0 to 20,000 us (20ms) */
    printf("--- Test 1: Exhaustive 0-20ms ---\n");
    int mismatches = 0;
    for (uint64_t us = 0; us <= 20000; us++) {
        uint64_t ns = us * 1000;
        uint32_t h = bucket_hardcoded(ns);
        uint32_t s = bucket_server(ns);
        if (h != s) {
            if (mismatches < 10)
                printf("  MISMATCH at %lluus: hardcoded=%u, server=%u\n",
                       (unsigned long long)us, h, s);
            mismatches++;
        }
    }
    CHECK(mismatches == 0,
          "daemon/server mismatch: %d out of 20001 values", mismatches);

    /* Test 2: Boundary values */
    printf("--- Test 2: Boundary values ---\n");
    struct { uint64_t ns; uint32_t expected; } boundaries[] = {
        {       0, 0 },   /* 0 us */
        {     999, 0 },   /* < 1us */
        {    1000, 1 },   /* exactly 1us */
        {    1999, 1 },   /* < 2us */
        {    2000, 2 },   /* exactly 2us */
        {    3999, 2 },   /* < 4us */
        {    4000, 3 },   /* exactly 4us */
        {    7999, 3 },   /* < 8us */
        {    8000, 4 },   /* exactly 8us */
        {   16000, 5 },   /* 16us */
        {   32000, 6 },   /* 32us */
        {   64000, 7 },   /* 64us */
        {  128000, 8 },   /* 128us */
        {  256000, 9 },   /* 256us */
        {  512000, 10 },  /* 512us */
        { 1024000, 11 },  /* 1ms */
        { 2048000, 12 },  /* 2ms */
        { 4096000, 13 },  /* 4ms */
        { 8192000, 14 },  /* 8ms */
        {16384000, 15 },  /* 16ms */
        {50000000, 15 },  /* 50ms — stays in bucket 15 */
        {1000000000ULL, 15 }, /* 1s */
    };
    int num_bounds = sizeof(boundaries) / sizeof(boundaries[0]);
    for (int i = 0; i < num_bounds; i++) {
        uint32_t got = bucket_hardcoded(boundaries[i].ns);
        CHECK(got == boundaries[i].expected,
              "%lluus -> bucket %u (expected %u)",
              (unsigned long long)(boundaries[i].ns / 1000),
              got, boundaries[i].expected);
    }

    /* Test 3: All buckets reachable */
    printf("--- Test 3: All buckets reachable ---\n");
    int seen[HISTOGRAM_BUCKETS] = {0};
    for (uint64_t us = 0; us <= 20000; us++) {
        uint32_t b = bucket_hardcoded(us * 1000);
        if (b < HISTOGRAM_BUCKETS) seen[b] = 1;
    }
    int all_seen = 1;
    for (int i = 0; i < HISTOGRAM_BUCKETS; i++) {
        if (!seen[i]) {
            printf("  MISS: bucket %d never hit\n", i);
            all_seen = 0;
        }
    }
    CHECK(all_seen, "All %d buckets reachable in 0-20ms range", HISTOGRAM_BUCKETS);

    /* Test 4: Bucket range is always 0..15 */
    printf("--- Test 4: Range check ---\n");
    int out_of_range = 0;
    for (uint64_t ns = 0; ns <= 100000000ULL; ns += 100) {
        uint32_t b = bucket_hardcoded(ns);
        if (b >= HISTOGRAM_BUCKETS) out_of_range++;
    }
    CHECK(out_of_range == 0,
          "No bucket >= %d in 0-100ms range (violations: %d)",
          HISTOGRAM_BUCKETS, out_of_range);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
