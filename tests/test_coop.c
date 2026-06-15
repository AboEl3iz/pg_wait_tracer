/* test_coop.c — Unit tests for the cooperative provider stub (Phase A6)
 *
 * The cooperative tier is interface-frozen here, not implemented (it ships in
 * the separate extension track). This test pins the FROZEN CONTRACT the
 * extension must satisfy, and proves the stub refuses activation cleanly:
 *
 *   - the coop provider registers with name "coop";
 *   - it advertises PGWT_FIDELITY_EXACT (cooperative source reports every
 *     transition);
 *   - the full vtable is present (start/stop/poll/self_metrics non-NULL);
 *   - start() returns -1 (clean "not available in this build") with no crash;
 *   - stop()/poll() succeed and self_metrics() is a safe no-op while unarmed.
 *
 * Built with -DPGWT_SERVER against provider_coop.c so it needs no BPF skeleton
 * (buildable without bpftool, and in CI's server-only jobs), matching
 * test_sampler / test_anomaly / test_trace_v2.
 */
#define _GNU_SOURCE
#include "provider_coop.h"
#include "daemon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, fmt, ...) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); } \
} while(0)

/* The stub exports its single not-available message so we can assert on it. */
extern const char pgwt_coop_unavailable_msg[];

int main(void)
{
    printf("=== test_coop (A6 cooperative provider stub) ===\n");

    const struct pgwt_capture_provider *p = &pgwt_provider_coop;

    /* --- contract: identity + fidelity --- */
    printf("--- frozen contract ---\n");
    CHECK(p->name != NULL && strcmp(p->name, "coop") == 0,
          "provider name should be \"coop\", got %s",
          p->name ? p->name : "(null)");
    CHECK(p->fidelity == PGWT_FIDELITY_EXACT,
          "coop must advertise EXACT fidelity, got %d", (int)p->fidelity);

    /* --- contract: full vtable present (extension must implement all four) --- */
    CHECK(p->start != NULL, "start() must be implemented");
    CHECK(p->stop != NULL, "stop() must be implemented");
    CHECK(p->poll != NULL, "poll() must be implemented");
    CHECK(p->self_metrics != NULL, "self_metrics() must be implemented");

    /* --- behavior: start() refuses cleanly, no crash --- */
    printf("--- clean not-available ---\n");
    /* struct pgwt_daemon is large (embeds the snapshot ring etc.) — heap it. */
    struct pgwt_daemon *dp = calloc(1, sizeof(*dp));
    if (!dp) { printf("  FAIL: calloc daemon\n"); return 1; }
    struct pgwt_daemon *d = dp;
    d->verbose = false;

    int rc = p->start(d);
    CHECK(rc == -1, "start() must return -1 (not available), got %d", rc);

    /* verbose path also must not crash */
    d->verbose = true;
    rc = p->start(d);
    CHECK(rc == -1, "start() (verbose) must return -1, got %d", rc);

    /* the not-available message is present and mentions the extension */
    CHECK(pgwt_coop_unavailable_msg[0] != '\0',
          "not-available message must be non-empty");
    CHECK(strstr(pgwt_coop_unavailable_msg, "not available") != NULL,
          "message should say \"not available\": \"%s\"",
          pgwt_coop_unavailable_msg);

    /* --- behavior: stop/poll/metrics are safe no-ops while unarmed --- */
    printf("--- safe no-ops while unarmed ---\n");
    CHECK(p->stop(d) == 0, "stop() must succeed (idempotent no-op)");
    CHECK(p->poll(d) == 0, "poll() must succeed (nothing to drain)");

    struct pgwt_metrics m;
    memset(&m, 0xab, sizeof(m));   /* poison; no-op metrics must not touch it */
    struct pgwt_metrics before = m;
    p->self_metrics(d, &m);
    CHECK(memcmp(&m, &before, sizeof(m)) == 0,
          "self_metrics() must be a no-op (no coop counters yet)");

    free(dp);

    /* --- start()/stop()/poll() must tolerate a NULL daemon (defensive) --- */
    printf("--- NULL-daemon tolerance ---\n");
    CHECK(p->start(NULL) == -1, "start(NULL) must return -1 without crashing");
    CHECK(p->stop(NULL) == 0,   "stop(NULL) must succeed without crashing");
    CHECK(p->poll(NULL) == 0,   "poll(NULL) must succeed without crashing");

    printf("=== %d/%d checks passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
