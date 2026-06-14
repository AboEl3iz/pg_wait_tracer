/* test_sampler.c — Unit tests for the sampled provider's core (Phase A2)
 *
 * Exercises the BPF-free sampler core (pgwt_sampler_read_targets and
 * pgwt_sampler_build_batch) against a controlled target: this process's own
 * memory (read via process_vm_readv on getpid()) and a child process with a
 * known 4-byte value at a known address (read via the per-pid pread
 * fallback). Built with -DPGWT_SERVER against the server objects so it needs
 * no BPF skeleton (buildable without bpftool, and in CI), matching
 * test_trace_v2.
 */
#define _GNU_SOURCE
#include "sampler.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, fmt, ...) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); } \
} while(0)

/* ── Test 1: read this process's own memory via process_vm_readv ──────── */

static void test_self_read(void)
{
    printf("--- self-read via process_vm_readv ---\n");

    /* Three known values; one is 0 (on-CPU) to confirm reads, not just
     * encoding, handle it. */
    volatile uint32_t v0 = WEI(PG_WAIT_IO, 0x12);     /* IO:something */
    volatile uint32_t v1 = 0;                          /* on CPU */
    volatile uint32_t v2 = WEI(PG_WAIT_LOCK, 0x03);   /* Lock:something */

    struct pgwt_sample_target targets[3] = {
        { .pid = getpid(), .wait_event_addr = (uint64_t)(uintptr_t)&v0, .query_id = 111 },
        { .pid = getpid(), .wait_event_addr = (uint64_t)(uintptr_t)&v1, .query_id = 222 },
        { .pid = getpid(), .wait_event_addr = (uint64_t)(uintptr_t)&v2, .query_id = 333 },
    };

    uint32_t vals[3] = { 0xdead, 0xdead, 0xdead };
    uint64_t faults = 0;
    int got = pgwt_sampler_read_targets(targets, 3, vals, &faults);

    CHECK(got == 3, "expected 3 reads, got %d", got);
    CHECK(vals[0] == v0, "vals[0]=0x%x expected 0x%x", vals[0], v0);
    CHECK(vals[1] == 0,  "vals[1]=0x%x expected 0", vals[1]);
    CHECK(vals[2] == v2, "vals[2]=0x%x expected 0x%x", vals[2], v2);
    CHECK(faults == 0, "expected no fallback faults, got %llu",
          (unsigned long long)faults);
}

/* ── Test 2: build_batch skips on-CPU and encodes fields ──────────────── */

static void test_build_batch(void)
{
    printf("--- build_batch encoding + on-CPU skip ---\n");

    struct pgwt_sample_target targets[3] = {
        { .pid = 1001, .wait_event_addr = 0x1000, .query_id = 111 },
        { .pid = 1002, .wait_event_addr = 0x2000, .query_id = 222 },
        { .pid = 1003, .wait_event_addr = 0x3000, .query_id = 333 },
    };
    uint32_t vals[3] = {
        WEI(PG_WAIT_IO, 0x01),  /* recorded */
        0,                       /* on CPU — skipped */
        WEI(PG_WAIT_LWLOCK, 0x07),
    };

    struct pgwt_trace_event out[3];
    memset(out, 0xAA, sizeof(out));
    uint64_t ts = 0x123456789ABCULL;
    int n = pgwt_sampler_build_batch(targets, vals, 3, ts, out);

    CHECK(n == 2, "expected 2 records (1 on-CPU skipped), got %d", n);

    /* Record 0 = target 0 */
    CHECK(out[0].timestamp_ns == ts, "ts mismatch");
    CHECK(out[0].pid == 1001, "pid=%u expected 1001", out[0].pid);
    CHECK(out[0].new_event == vals[0], "new_event=0x%x expected 0x%x",
          out[0].new_event, vals[0]);
    CHECK(out[0].old_event == 0, "old_event must be 0 for samples");
    CHECK(out[0].duration_ns == 0, "duration must be 0 for samples");
    CHECK(out[0].query_id == 111, "query_id=%llu expected 111",
          (unsigned long long)out[0].query_id);

    /* Record 1 = target 2 (target 1 was on-CPU) */
    CHECK(out[1].pid == 1003, "pid=%u expected 1003", out[1].pid);
    CHECK(out[1].new_event == vals[2], "new_event=0x%x expected 0x%x",
          out[1].new_event, vals[2]);
    CHECK(out[1].query_id == 333, "query_id=%llu expected 333",
          (unsigned long long)out[1].query_id);
}

/* ── Test 3: per-pid pread fallback on a bad leading entry ────────────── */

static void test_fallback(void)
{
    printf("--- per-pid pread fallback ---\n");

    /* A valid value we want recovered. */
    volatile uint32_t good = WEI(PG_WAIT_CLIENT, 0x00);

    /* Target 0 points at an unmapped address: process_vm_readv faults at the
     * first iovec, returning a partial (0 bytes) result, so the whole batch
     * falls to the per-pid pread path. Target 1 is a valid self address and
     * must still be recovered by pread(/proc/self/mem). */
    void *bad = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(bad != MAP_FAILED, "mmap PROT_NONE failed");
    munmap(bad, 4096);   /* now definitely unmapped */

    struct pgwt_sample_target targets[2] = {
        { .pid = getpid(), .wait_event_addr = (uint64_t)(uintptr_t)bad, .query_id = 1 },
        { .pid = getpid(), .wait_event_addr = (uint64_t)(uintptr_t)&good, .query_id = 2 },
    };

    uint32_t vals[2] = { 0xdead, 0xdead };
    uint64_t faults = 0;
    int got = pgwt_sampler_read_targets(targets, 2, vals, &faults);

    /* The good entry must be recovered via pread even though entry 0 faulted. */
    CHECK(vals[1] == good, "fallback vals[1]=0x%x expected 0x%x", vals[1], good);
    CHECK(got >= 1, "expected at least the good entry recovered, got %d", got);
    CHECK(faults >= 1, "expected >=1 fallback fault recorded, got %llu",
          (unsigned long long)faults);
}

/* ── Test 4: child process read via fallback (different pid) ───────────── */

static void test_child_read(void)
{
    printf("--- read child process value (cross-pid) ---\n");

    /* Shared anonymous mmap: parent writes a known value, child sees it at
     * its own (possibly different) virtual address, reports the address back
     * via pipe, then sleeps so the parent can sample it. */
    void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(page != MAP_FAILED, "mmap shared failed");
    if (page == MAP_FAILED)
        return;

    volatile uint32_t *slot = page;
    *slot = WEI(PG_WAIT_IPC, 0x42);

    int pfd[2];
    if (pipe(pfd) != 0) { CHECK(0, "pipe failed"); return; }

    pid_t child = fork();
    if (child == 0) {
        /* Child: report the slot's address (same VA — MAP_SHARED inherited),
         * then idle until killed. */
        uint64_t addr = (uint64_t)(uintptr_t)slot;
        ssize_t w = write(pfd[1], &addr, sizeof(addr));
        (void)w;
        for (;;) pause();
        _exit(0);
    }
    CHECK(child > 0, "fork failed");

    uint64_t child_addr = 0;
    ssize_t r = read(pfd[0], &child_addr, sizeof(child_addr));
    CHECK(r == (ssize_t)sizeof(child_addr), "did not get child address");

    struct pgwt_sample_target targets[1] = {
        { .pid = child, .wait_event_addr = child_addr, .query_id = 7 },
    };
    uint32_t vals[1] = { 0xdead };
    uint64_t faults = 0;
    int got = pgwt_sampler_read_targets(targets, 1, vals, &faults);

    CHECK(got == 1, "expected to read child value, got %d", got);
    CHECK(vals[0] == WEI(PG_WAIT_IPC, 0x42),
          "child vals[0]=0x%x expected 0x%x", vals[0], WEI(PG_WAIT_IPC, 0x42));

    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    close(pfd[0]);
    close(pfd[1]);
    munmap(page, 4096);
}

int main(void)
{
    test_self_read();
    test_build_batch();
    test_fallback();
    test_child_read();

    printf("\n%d/%d checks passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
