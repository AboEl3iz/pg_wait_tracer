/* anomaly.h — Anomaly-triggered escalation rules (Track A, D5 / Phase A5)
 *
 * In --mode tiered the always-on sampled stream is watched by a small rules
 * engine. When the live samples show an incident, the daemon AUTO-escalates to
 * full-fidelity capture (via the A4 escalation engine) so the full data for
 * the incident is on disk without a human reacting in time.
 *
 * Two rules, both evaluated once per sampler tick on metrics derived from that
 * tick's sample batch (no re-reading the trace):
 *
 *   1. AAS-vs-baseline — maintain a rolling baseline of AAS (active sessions,
 *      idle-excluded per pgwt_is_idle_event). Fire when
 *      `aas > k * rolling_baseline` is sustained for N consecutive ticks.
 *      Config: --anomaly-aas-factor k, --anomaly-aas-ticks N.
 *   2. Lock-class fraction — fire when the Lock-class share of the tick's
 *      ACTIVE (non-idle) samples exceeds a threshold, sustained for N ticks.
 *      Config: --anomaly-lock-fraction.
 *
 * Hysteresis + cooldown: once an auto-escalation fires, a minimum cooldown
 * (--anomaly-cooldown-s) must elapse before another auto-escalation can fire,
 * so a flapping metric cannot burst-burn the budget. An anomaly while already
 * escalated EXTENDS the window (pgwt_escalate's extend path), it never stacks.
 *
 * Budget: a firing rule calls pgwt_escalate(..., PGWT_ESC_REASON_ANOMALY,...).
 * The A4 engine enforces the rolling-hour budget; an over-budget anomaly
 * trigger is dropped SILENTLY (logged, no error to anyone) — unlike a manual
 * escalate, which returns a denial to its caller.
 *
 * Observability: every NEAR-trigger (a metric crossed its threshold but the
 * sustained count was not yet met, or the fire was blocked by cooldown or
 * budget) is logged so thresholds can be tuned from real data.
 *
 * The rule logic is split out as a PURE function (pgwt_anomaly_eval) that
 * takes a tick's (aas, lock_fraction) and the current time and returns a
 * decision, mutating only the rule state. It touches neither BPF nor the
 * escalation engine, so it is unit-testable by feeding scripted sample
 * streams (see tests/test_anomaly.c). The daemon-side wrapper
 * (pgwt_anomaly_tick) derives the metrics from the live batch, logs the
 * decision, and performs the escalate on a FIRE.
 */
#ifndef PGWT_ANOMALY_H
#define PGWT_ANOMALY_H

#include <stdbool.h>
#include <stdint.h>

struct pgwt_daemon;
struct pgwt_trace_event;

/* What the rule engine decided this tick. */
enum pgwt_anomaly_action {
    PGWT_ANOMALY_NONE = 0,   /* nothing crossed; quiet tick */
    PGWT_ANOMALY_NEAR,       /* a threshold crossed but did not fire (see why) */
    PGWT_ANOMALY_FIRE,       /* a rule fired — escalate to full fidelity */
};

/* Why a near-trigger did not fire (bitmask; informational, for logging). */
enum pgwt_anomaly_near {
    PGWT_NEAR_NONE        = 0,
    PGWT_NEAR_AAS_SUSTAIN = 1 << 0, /* AAS over threshold, sustain count not met */
    PGWT_NEAR_LOCK_SUSTAIN = 1 << 1,/* lock-fraction over, sustain count not met */
    PGWT_NEAR_COOLDOWN    = 1 << 2, /* would fire but in cooldown */
    PGWT_NEAR_BASELINE    = 1 << 3, /* baseline not warm yet (no AAS rule) */
};

/* Which rule(s) triggered the fire (bitmask). Recorded for logs/diagnostics. */
enum pgwt_anomaly_rule {
    PGWT_RULE_NONE = 0,
    PGWT_RULE_AAS  = 1 << 0,
    PGWT_RULE_LOCK = 1 << 1,
};

struct pgwt_anomaly_decision {
    enum pgwt_anomaly_action action;
    unsigned near_mask;     /* enum pgwt_anomaly_near (when action == NEAR) */
    unsigned fired_mask;    /* enum pgwt_anomaly_rule (when action == FIRE) */
    double   aas;           /* metrics evaluated this tick (for logging) */
    double   baseline;      /* current rolling baseline AAS */
    double   lock_fraction; /* lock-class share of active samples this tick */
};

struct pgwt_anomaly {
    bool   enabled;           /* armed only in --mode tiered */

    /* Config (thresholds). Conservative defaults; all overridable. */
    double aas_factor;        /* fire when aas > factor * baseline */
    int    aas_ticks;         /* consecutive ticks the AAS rule must hold */
    double lock_fraction;     /* fire when lock share > this */
    int    lock_ticks;        /* consecutive ticks the lock rule must hold */
    uint64_t cooldown_ns;     /* min gap between auto-escalations */
    int    escalation_s;      /* duration requested per auto-escalation */

    /* Rolling-baseline (EWMA) state for the AAS rule. The baseline tracks the
     * NORMAL load; it is NOT updated while a metric is anomalous (so a long
     * incident does not poison the baseline and silence the rule). */
    double  baseline_aas;     /* exponentially-weighted moving average */
    double  baseline_alpha;   /* EWMA smoothing factor (per tick) */
    int     baseline_warmup;  /* ticks observed before baseline is trustworthy */
    int     warmup_needed;    /* ticks of normal data required to warm up */

    /* Hysteresis: consecutive-over-threshold counters. */
    int     aas_over_streak;
    int     lock_over_streak;

    /* Cooldown: monotonic ns of the last auto-escalation (0 = never). */
    uint64_t last_fire_ns;

    /* Lifetime stats (control-socket metrics). */
    uint64_t fires_total;       /* auto-escalations actually requested */
    uint64_t near_total;        /* near-triggers logged */
    uint64_t dropped_budget;    /* fires the budget silently dropped */
    uint64_t dropped_cooldown;  /* fires the cooldown suppressed */
};

/* Default thresholds (conservative). */
#define PGWT_ANOMALY_DEF_AAS_FACTOR   3.0
#define PGWT_ANOMALY_DEF_AAS_TICKS    3
#define PGWT_ANOMALY_DEF_LOCK_FRAC    0.30
#define PGWT_ANOMALY_DEF_LOCK_TICKS   3
#define PGWT_ANOMALY_DEF_COOLDOWN_S   120
#define PGWT_ANOMALY_DEF_ESCALATE_S   60

/* Initialize rule state + config. enabled is true only for tiered mode.
 * sample_rate_hz tunes the EWMA so the baseline reacts over roughly a fixed
 * wall-clock horizon regardless of tick rate. */
void pgwt_anomaly_init(struct pgwt_anomaly *a, bool enabled,
                       int sample_rate_hz);

/* ── Pure rule evaluation (no BPF, no escalation, unit-testable) ────────── */

/* Evaluate the rules for one tick. `aas` is the number of active (non-idle)
 * samples this tick; `lock_fraction` is the Lock-class share of those active
 * samples (0 when there are none). `now_ns` is a monotonic timestamp.
 *
 * Mutates only *a (baseline EWMA, streak counters, cooldown clock on a FIRE,
 * lifetime stats). Returns the decision. It does NOT call pgwt_escalate — the
 * caller does that on PGWT_ANOMALY_FIRE so the rule logic stays pure.
 *
 * Cooldown is charged here on FIRE so the test can assert flap suppression
 * without a live escalation engine: a FIRE updates last_fire_ns; a would-fire
 * inside cooldown is downgraded to NEAR(COOLDOWN). The BUDGET drop is NOT
 * modeled here (the engine owns the budget) — the daemon wrapper observes a
 * silent budget denial and records dropped_budget. */
struct pgwt_anomaly_decision
pgwt_anomaly_eval(struct pgwt_anomaly *a, double aas, double lock_fraction,
                  uint64_t now_ns);

/* Derive (aas, lock_fraction) from a tick's encoded sample batch. `samples`
 * is the build_batch output (idle/on-CPU handling already applied by the
 * sampler for on-CPU; idle waits like ClientRead are still present and are
 * excluded here). Pure — exposed for the unit test. */
void pgwt_anomaly_metrics_from_batch(const struct pgwt_trace_event *samples,
                                     int n, double *out_aas,
                                     double *out_lock_fraction);

/* ── Daemon-side wrapper (links the rules to the escalation engine) ─────── */

/* Called by the sampler each tick AFTER building its batch. Derives the
 * metrics, evaluates the rules, logs near-triggers, and on a FIRE requests an
 * anomaly escalation (silently dropped if over budget). No-op when disabled. */
void pgwt_anomaly_tick(struct pgwt_daemon *d,
                       const struct pgwt_trace_event *samples, int n);

#endif /* PGWT_ANOMALY_H */
