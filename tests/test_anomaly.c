/* test_anomaly.c — Unit tests for the anomaly rule engine (Phase A5)
 *
 * Drives the PURE rule core (pgwt_anomaly_eval / pgwt_anomaly_metrics_from_batch)
 * with scripted sample-stream inputs and asserts fire / no-fire across:
 *   - AAS-vs-baseline (factor, sustained N ticks, baseline warmup)
 *   - lock-class fraction (threshold, sustained N ticks)
 *   - hysteresis / cooldown (a flapping metric cannot re-fire inside cooldown)
 *   - budget-blocked-silent (modeled at the daemon layer — here we assert the
 *     pure core still FIREs, since the budget lives in the escalation engine)
 *
 * Built with -DPGWT_SERVER against anomaly.c so only the BPF-free core is
 * compiled (no skeleton, no escalation engine) — runnable in CI's server-only
 * jobs, matching test_sampler / test_trace_v2.
 */
#define _GNU_SOURCE
#include "anomaly.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, fmt, ...) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); } \
} while(0)

/* One tick in ns at 10 Hz. */
#define TICK_NS 100000000ULL

/* Feed the engine a constant (aas, lock_fraction) for `ticks`, advancing the
 * monotonic clock one tick per call. Returns the LAST decision; *fires
 * (if non-NULL) accumulates the number of FIRE decisions seen. */
static struct pgwt_anomaly_decision
feed(struct pgwt_anomaly *a, double aas, double lock_frac, int ticks,
     uint64_t *clock, int *fires)
{
    struct pgwt_anomaly_decision d;
    memset(&d, 0, sizeof(d));
    for (int i = 0; i < ticks; i++) {
        d = pgwt_anomaly_eval(a, aas, lock_frac, *clock);
        if (fires && d.action == PGWT_ANOMALY_FIRE)
            (*fires)++;
        *clock += TICK_NS;
    }
    return d;
}

/* Warm the baseline to ~level by feeding `level` AAS for enough normal ticks. */
static void warm_baseline(struct pgwt_anomaly *a, double level,
                          uint64_t *clock)
{
    feed(a, level, 0.0, a->warmup_needed + 5, clock, NULL);
}

/* ── Test 1: disabled engine never fires ──────────────────────────────── */
static void test_disabled(void)
{
    printf("--- disabled engine: never fires ---\n");
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, false, 10);
    uint64_t clk = TICK_NS;
    int fires = 0;
    struct pgwt_anomaly_decision d =
        feed(&a, 1000.0, 0.99, 50, &clk, &fires);
    CHECK(d.action == PGWT_ANOMALY_NONE, "disabled produced action %d",
          d.action);
    CHECK(fires == 0, "disabled fired %d times", fires);
}

/* ── Test 2: AAS-vs-baseline fires only after sustained N ticks ────────── */
static void test_aas_sustained(void)
{
    printf("--- AAS rule: fire after sustained N ticks ---\n");
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    a.aas_factor = 3.0;
    a.aas_ticks  = 3;
    uint64_t clk = TICK_NS;

    warm_baseline(&a, 2.0, &clk);   /* baseline ~2 active sessions */
    CHECK(a.baseline_aas > 1.0 && a.baseline_aas < 3.0,
          "baseline=%.2f expected ~2", a.baseline_aas);

    /* Spike to 10 (> 3*2=6). First two ticks: NEAR (sustain not met). */
    struct pgwt_anomaly_decision d1 = pgwt_anomaly_eval(&a, 10.0, 0.0, clk);
    clk += TICK_NS;
    CHECK(d1.action == PGWT_ANOMALY_NEAR &&
          (d1.near_mask & PGWT_NEAR_AAS_SUSTAIN),
          "tick1 expected NEAR(aas-sustain), got action=%d mask=%u",
          d1.action, d1.near_mask);

    struct pgwt_anomaly_decision d2 = pgwt_anomaly_eval(&a, 10.0, 0.0, clk);
    clk += TICK_NS;
    CHECK(d2.action == PGWT_ANOMALY_NEAR, "tick2 expected NEAR, got %d",
          d2.action);

    /* Third sustained tick: FIRE. */
    struct pgwt_anomaly_decision d3 = pgwt_anomaly_eval(&a, 10.0, 0.0, clk);
    clk += TICK_NS;
    CHECK(d3.action == PGWT_ANOMALY_FIRE &&
          (d3.fired_mask & PGWT_RULE_AAS),
          "tick3 expected FIRE(aas), got action=%d mask=%u",
          d3.action, d3.fired_mask);
}

/* ── Test 3: AAS within factor never fires ─────────────────────────────── */
static void test_aas_no_fire(void)
{
    printf("--- AAS rule: under factor never fires ---\n");
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    a.aas_factor = 3.0;
    a.aas_ticks  = 3;
    uint64_t clk = TICK_NS;
    warm_baseline(&a, 4.0, &clk);   /* baseline ~4 */

    /* AAS = 8 < 3*4 = 12 → never fires even sustained for many ticks. */
    int fires = 0;
    feed(&a, 8.0, 0.0, 50, &clk, &fires);
    CHECK(fires == 0, "AAS under factor fired %d times", fires);
}

/* ── Test 4: lock-class fraction rule ──────────────────────────────────── */
static void test_lock_fraction(void)
{
    printf("--- lock-fraction rule: fire after sustained N ticks ---\n");
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    a.lock_fraction = 0.30;
    a.lock_ticks    = 3;
    /* Disable AAS so this isolates the lock rule. */
    a.aas_factor = 0.0;
    uint64_t clk = TICK_NS;
    warm_baseline(&a, 2.0, &clk);

    /* lock_frac 0.2 < 0.3 → no fire. */
    int fires = 0;
    feed(&a, 5.0, 0.20, 10, &clk, &fires);
    CHECK(fires == 0, "lock_frac under threshold fired %d", fires);

    /* lock_frac 0.6 > 0.3, sustained 3 ticks → fire on the 3rd. */
    struct pgwt_anomaly_decision d1 = pgwt_anomaly_eval(&a, 5.0, 0.6, clk);
    clk += TICK_NS;
    struct pgwt_anomaly_decision d2 = pgwt_anomaly_eval(&a, 5.0, 0.6, clk);
    clk += TICK_NS;
    struct pgwt_anomaly_decision d3 = pgwt_anomaly_eval(&a, 5.0, 0.6, clk);
    clk += TICK_NS;
    CHECK(d1.action == PGWT_ANOMALY_NEAR &&
          (d1.near_mask & PGWT_NEAR_LOCK_SUSTAIN), "lock tick1 not NEAR");
    CHECK(d2.action == PGWT_ANOMALY_NEAR, "lock tick2 not NEAR");
    CHECK(d3.action == PGWT_ANOMALY_FIRE &&
          (d3.fired_mask & PGWT_RULE_LOCK),
          "lock tick3 expected FIRE(lock), got action=%d mask=%u",
          d3.action, d3.fired_mask);
}

/* ── Test 5: cooldown suppresses re-fire (hysteresis) ──────────────────── */
static void test_cooldown(void)
{
    printf("--- cooldown: flapping metric cannot re-fire ---\n");
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    a.lock_fraction = 0.30;
    a.lock_ticks    = 1;          /* fire immediately on cross */
    a.aas_factor    = 0.0;        /* isolate the lock rule */
    a.cooldown_ns   = 100ULL * TICK_NS;   /* 10s cooldown @10Hz */
    uint64_t clk = TICK_NS;
    warm_baseline(&a, 2.0, &clk);

    /* First over-threshold tick → FIRE. */
    struct pgwt_anomaly_decision d1 = pgwt_anomaly_eval(&a, 5.0, 0.9, clk);
    clk += TICK_NS;
    CHECK(d1.action == PGWT_ANOMALY_FIRE, "first cross expected FIRE, got %d",
          d1.action);

    /* Immediately over again (flap) but inside cooldown → NEAR(cooldown). */
    int fires_in_cooldown = 0;
    for (int i = 0; i < 50; i++) {   /* 50 ticks = 5s < 10s cooldown */
        struct pgwt_anomaly_decision d =
            pgwt_anomaly_eval(&a, 5.0, 0.9, clk);
        clk += TICK_NS;
        if (d.action == PGWT_ANOMALY_FIRE)
            fires_in_cooldown++;
        if (d.action == PGWT_ANOMALY_NEAR && (d.near_mask & PGWT_NEAR_COOLDOWN))
            CHECK(d.fired_mask & PGWT_RULE_LOCK,
                  "cooldown NEAR should carry the would-fire rule");
    }
    CHECK(fires_in_cooldown == 0, "fired %d times inside cooldown",
          fires_in_cooldown);
    CHECK(a.dropped_cooldown > 0, "no cooldown drops recorded");

    /* After cooldown elapses, a sustained cross fires again. */
    clk += 200ULL * TICK_NS;   /* jump well past cooldown */
    struct pgwt_anomaly_decision d2 = pgwt_anomaly_eval(&a, 5.0, 0.9, clk);
    CHECK(d2.action == PGWT_ANOMALY_FIRE, "post-cooldown expected FIRE, got %d",
          d2.action);
}

/* ── Test 6: baseline does not absorb a sustained anomaly ──────────────── */
static void test_baseline_protected(void)
{
    printf("--- baseline: sustained anomaly must not poison baseline ---\n");
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    a.aas_factor = 3.0;
    a.aas_ticks  = 3;
    uint64_t clk = TICK_NS;
    warm_baseline(&a, 2.0, &clk);
    double base_before = a.baseline_aas;

    /* A long incident at AAS=20. The baseline must stay near 2, so the rule
     * keeps firing (well, NEAR after the cooldown gate) instead of the bar
     * creeping up to 20 and silencing the rule. */
    feed(&a, 20.0, 0.0, 300, &clk, NULL);
    CHECK(a.baseline_aas < base_before + 1.0,
          "baseline crept to %.2f (was %.2f) — anomaly poisoned it",
          a.baseline_aas, base_before);
}

/* ── Test 7: budget-drop is silent at the engine boundary ──────────────── */
/* The pure core does NOT know about budget — it always returns FIRE; the
 * daemon wrapper observes pgwt_escalate's silent denial and records
 * dropped_budget. We assert the pure core keeps FIRing (it must, so the wrapper
 * gets a chance to attempt the escalate) and that fires_total counts attempts,
 * which is the number the budget layer then accepts-or-silently-drops. */
static void test_budget_boundary(void)
{
    printf("--- budget: pure core fires; budget handled at wrapper ---\n");
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    a.aas_factor  = 3.0;
    a.aas_ticks   = 1;
    a.cooldown_ns = 0;            /* no cooldown so each cross can fire */
    uint64_t clk = TICK_NS;
    warm_baseline(&a, 2.0, &clk);

    int fires = 0;
    /* Cross repeatedly, jumping past any cooldown each time (cooldown=0). */
    for (int i = 0; i < 5; i++) {
        struct pgwt_anomaly_decision d = pgwt_anomaly_eval(&a, 20.0, 0.0, clk);
        clk += TICK_NS;
        if (d.action == PGWT_ANOMALY_FIRE)
            fires++;
    }
    CHECK(fires >= 1, "expected the core to FIRE on a sustained spike");
    CHECK(a.fires_total == (uint64_t)fires,
          "fires_total=%llu != observed %d",
          (unsigned long long)a.fires_total, fires);
}

/* ── Test 8: metrics_from_batch derives AAS + lock fraction correctly ──── */
static void test_metrics_from_batch(void)
{
    printf("--- metrics_from_batch: AAS + lock-fraction derivation ---\n");

    /* A batch as build_batch would produce: on-CPU (event 0) is already
     * absent. Mix of Lock, LWLock, IO and idle (ClientRead / Activity). */
    struct pgwt_trace_event batch[6];
    memset(batch, 0, sizeof(batch));
    batch[0].new_event = WEI(PG_WAIT_LOCK, 0x03);     /* active, lock */
    batch[1].new_event = WEI(PG_WAIT_LOCK, 0x01);     /* active, lock */
    batch[2].new_event = WEI(PG_WAIT_LWLOCK, 0x07);   /* active, not lock */
    batch[3].new_event = WEI(PG_WAIT_IO, 0x10);       /* active, not lock */
    batch[4].new_event = WEI(PG_WAIT_CLIENT, 0);      /* idle (ClientRead) */
    batch[5].new_event = WEI(PG_WAIT_ACTIVITY, 0x02); /* idle (Activity) */

    double aas = -1, frac = -1;
    pgwt_anomaly_metrics_from_batch(batch, 6, &aas, &frac);
    /* Active = 4 (the two idle excluded); locks = 2 → fraction 0.5. */
    CHECK(aas == 4.0, "aas=%.1f expected 4", aas);
    CHECK(frac == 0.5, "lock_fraction=%.2f expected 0.50", frac);

    /* All-idle batch → AAS 0, fraction 0 (no divide-by-zero). */
    struct pgwt_trace_event idle[2];
    memset(idle, 0, sizeof(idle));
    idle[0].new_event = WEI(PG_WAIT_CLIENT, 0);
    idle[1].new_event = WEI(PG_WAIT_ACTIVITY, 0x01);
    pgwt_anomaly_metrics_from_batch(idle, 2, &aas, &frac);
    CHECK(aas == 0.0, "all-idle aas=%.1f expected 0", aas);
    CHECK(frac == 0.0, "all-idle frac=%.2f expected 0", frac);
}

/* ── Test 9: AAS + lock can fire together (combined fired_mask) ────────── */
static void test_combined_fire(void)
{
    printf("--- combined: AAS and lock fire on the same tick ---\n");
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    a.aas_factor    = 3.0;
    a.aas_ticks     = 2;
    a.lock_fraction = 0.30;
    a.lock_ticks    = 2;
    uint64_t clk = TICK_NS;
    warm_baseline(&a, 2.0, &clk);

    /* Spike AAS=10 (>6) AND lock_frac 0.8 simultaneously, sustained 2 ticks. */
    pgwt_anomaly_eval(&a, 10.0, 0.8, clk); clk += TICK_NS;
    struct pgwt_anomaly_decision d = pgwt_anomaly_eval(&a, 10.0, 0.8, clk);
    CHECK(d.action == PGWT_ANOMALY_FIRE, "combined expected FIRE, got %d",
          d.action);
    CHECK((d.fired_mask & PGWT_RULE_AAS) && (d.fired_mask & PGWT_RULE_LOCK),
          "combined fired_mask=%u expected both AAS+LOCK", d.fired_mask);
}

int main(void)
{
    test_disabled();
    test_aas_sustained();
    test_aas_no_fire();
    test_lock_fraction();
    test_cooldown();
    test_baseline_protected();
    test_budget_boundary();
    test_metrics_from_batch();
    test_combined_fire();

    printf("\n%d/%d checks passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
