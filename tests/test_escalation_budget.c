/* test_escalation_budget.c — Unit tests for the pure escalation budget core
 * (Phase T3: ESC-1/5/6).
 *
 * Drives escalation_budget.c (BPF-free, daemon-free) with a scripted clock to
 * pin the budget invariants the plan's definition-of-done demands:
 *   - ESC-1: worst-case full-fidelity seconds per hour <= budget under
 *     ADVERSARIAL requests — the extend-every-second attack caps at EXACTLY
 *     the budget (committed-remainder-aware charge + deadline clamp).
 *   - ESC-5: ledger overflow never opens an UNBILLED window (coalesce bills
 *     conservatively).
 *   - ESC-6: --escalation-budget 0 = deny-all; 'unlimited' = no cap; both
 *     report the truth.
 *   - rolling-hour aging frees budget back.
 *
 * Compiles escalation_budget.c directly — no skeleton, no daemon, no BPF —
 * runnable in CI's server-only jobs (same pattern as test_sampler/test_anomaly).
 */
#define _GNU_SOURCE
#include "escalation.h"
#include "escalation_budget.h"

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

#define SEC 1000000000ULL
#define HOUR (3600ULL * SEC)

static void init_engine(struct pgwt_escalation *e, uint64_t budget_ns,
                        bool unlimited)
{
    memset(e, 0, sizeof(*e));
    e->rolling_window_ns = HOUR;
    e->budget_ns = budget_ns;
    e->budget_unlimited = unlimited;
}

/* ── Test 1: a fresh grant within budget is billed exactly ─────────────── */
static void test_fresh_grant(void)
{
    printf("--- fresh grant within budget bills exactly ---\n");
    struct pgwt_escalation e;
    init_engine(&e, 10 * SEC, false);

    uint64_t now = 100 * SEC;
    uint64_t grant = 0;
    const char *why = NULL;
    int rc = pgwt_esc_budget_decide(&e, now, 4 * SEC, &grant, &why);
    CHECK(rc == 0, "decide within budget should succeed (why=%s)",
          why ? why : "");
    CHECK(grant == 4 * SEC, "grant=%.1fs expected 4", (double)grant / 1e9);

    pgwt_esc_ledger_open(&e, now);
    /* window runs to now+grant, then closes */
    pgwt_esc_ledger_close(&e, now + grant);

    uint64_t consumed = pgwt_esc_budget_consumed_ns(&e, now + grant);
    CHECK(consumed == 4 * SEC, "consumed=%.1fs expected 4",
          (double)consumed / 1e9);
    uint64_t rem = pgwt_esc_budget_remaining_ns(&e, now + grant);
    CHECK(rem == 6 * SEC, "remaining=%.1fs expected 6", (double)rem / 1e9);
}

/* ── Test 2: extend-every-second caps total at EXACTLY the budget (ESC-1) ─ */
static void test_extend_every_second_caps(void)
{
    printf("--- ESC-1: extend-every-second caps at exactly the budget ---\n");
    const uint64_t budget = 10 * SEC;
    struct pgwt_escalation e;
    init_engine(&e, budget, false);

    uint64_t start = 500 * SEC;
    uint64_t grant = 0;
    const char *why = NULL;

    /* Fresh escalate at t=start for a huge window — must clamp to budget. */
    int rc = pgwt_esc_budget_decide(&e, start, HOUR, &grant, &why);
    CHECK(rc == 0, "initial escalate should be granted");
    CHECK(grant == budget, "initial grant clamped to budget (%.1fs)",
          (double)grant / 1e9);
    pgwt_esc_ledger_open(&e, start);
    uint64_t deadline = start + grant;

    /* Now hammer an extension every second with a huge request. The clamp must
     * hold the deadline at exactly start+budget no matter how many extends. */
    int denied_after = -1;
    for (int k = 1; k <= 20; k++) {
        uint64_t now = start + (uint64_t)k * SEC;
        grant = 0;
        why = NULL;
        rc = pgwt_esc_budget_decide(&e, now, HOUR, &grant, &why);
        if (rc == 0) {
            uint64_t nd = now + grant;
            if (nd > deadline)
                deadline = nd;
        } else if (denied_after < 0) {
            denied_after = k;   /* budget hit here */
        }
    }
    CHECK(deadline == start + budget,
          "deadline pinned at start+budget: got +%.1fs expected +%.1fs",
          (double)(deadline - start) / 1e9, (double)budget / 1e9);
    CHECK(denied_after >= 10 && denied_after <= 11,
          "extensions denied once budget consumed (~t=10s, got t=%ds)",
          denied_after);

    /* Close at the (clamped) deadline; total billed must be EXACTLY budget. */
    pgwt_esc_ledger_close(&e, deadline);
    uint64_t consumed = pgwt_esc_budget_consumed_ns(&e, deadline);
    CHECK(consumed == budget,
          "total full-fidelity billed = %.3fs, must equal budget %.1fs",
          (double)consumed / 1e9, (double)budget / 1e9);
}

/* ── Test 3: mid-window over-budget detection (ESC-1 clamp backstop) ────── */
static void test_budget_over(void)
{
    printf("--- ESC-1: budget_over trips once consumption reaches budget ---\n");
    struct pgwt_escalation e;
    init_engine(&e, 5 * SEC, false);
    uint64_t start = 0;
    pgwt_esc_ledger_open(&e, start);

    CHECK(!pgwt_esc_budget_over(&e, start + 3 * SEC),
          "3s into a 5s budget is not over");
    CHECK(pgwt_esc_budget_over(&e, start + 5 * SEC),
          "5s into a 5s budget IS over");
    CHECK(pgwt_esc_budget_over(&e, start + 8 * SEC),
          "8s into a 5s budget is over");
}

/* ── Test 4: budget 0 = deny-all; unlimited = no cap (ESC-6) ────────────── */
static void test_zero_and_unlimited(void)
{
    printf("--- ESC-6: budget 0 denies all; unlimited never caps ---\n");

    struct pgwt_escalation zero;
    init_engine(&zero, 0, false);
    uint64_t grant = 12345;
    const char *why = NULL;
    int rc = pgwt_esc_budget_decide(&zero, 0, 60 * SEC, &grant, &why);
    CHECK(rc == -1, "budget 0 must DENY every escalation");
    CHECK(why != NULL && strstr(why, "disabled") != NULL,
          "denial reason mentions 'disabled' (got: %s)", why ? why : "(null)");
    CHECK(pgwt_esc_budget_remaining_ns(&zero, 0) == 0,
          "budget 0 reports 0 remaining (honest)");
    CHECK(!pgwt_esc_budget_over(&zero, 0),
          "budget 0 never reports over (no window ever opens)");

    struct pgwt_escalation unl;
    init_engine(&unl, 0, true);   /* budget_ns ignored when unlimited */
    grant = 0;
    rc = pgwt_esc_budget_decide(&unl, 0, HOUR, &grant, &why);
    CHECK(rc == 0, "unlimited grants any request");
    CHECK(grant == HOUR, "unlimited grants the full request (%.0fs)",
          (double)grant / 1e9);
    CHECK(pgwt_esc_budget_remaining_ns(&unl, 0) == UINT64_MAX,
          "unlimited reports UINT64_MAX remaining (the ∞ sentinel)");
    /* Even after opening/closing many windows, unlimited never caps. */
    pgwt_esc_ledger_open(&unl, 0);
    pgwt_esc_ledger_close(&unl, HOUR);
    rc = pgwt_esc_budget_decide(&unl, HOUR, HOUR, &grant, &why);
    CHECK(rc == 0 && !pgwt_esc_budget_over(&unl, HOUR),
          "unlimited still grants after heavy use");
}

/* ── Test 5: ledger overflow is BILLED, never unbilled (ESC-5) ──────────── */
static void test_ledger_overflow_billed(void)
{
    printf("--- ESC-5: ledger overflow coalesces, never opens unbilled ---\n");
    /* Unlimited budget so the ONLY thing under test is ledger capacity, not
     * budget denial. Pack many short windows within the rolling hour. */
    struct pgwt_escalation e;
    init_engine(&e, 0, true);

    /* Open+close PGWT_ESC_LEDGER_MAX + 50 windows, each 1s long, 2s apart, all
     * within the rolling hour (base at 0). That's 306 windows into 256 slots. */
    uint64_t base = SEC;
    int total = PGWT_ESC_LEDGER_MAX + 50;
    for (int i = 0; i < total; i++) {
        uint64_t open = base + (uint64_t)i * 2 * SEC;
        pgwt_esc_ledger_open(&e, open);
        CHECK(e.ledger_count <= PGWT_ESC_LEDGER_MAX,
              "ledger_count never exceeds MAX (got %d at i=%d)",
              e.ledger_count, i);
        pgwt_esc_ledger_close(&e, open + SEC);
    }

    /* Every window was 1s of real capture => >= total seconds must be billed
     * (coalescing over-bills the gaps, so consumed is AT LEAST the true sum).
     * The key property: NOTHING was silently dropped/unbilled. */
    uint64_t now = base + (uint64_t)total * 2 * SEC;
    uint64_t consumed = pgwt_esc_budget_consumed_ns(&e, now);
    CHECK(consumed >= (uint64_t)total * SEC,
          "consumed=%.1fs >= true captured %.1fs (never under-bills)",
          (double)consumed / 1e9, (double)total);
    CHECK(e.ledger_count <= PGWT_ESC_LEDGER_MAX,
          "final ledger_count=%d within MAX", e.ledger_count);

    /* And an overflow open still records a genuinely-open segment. */
    pgwt_esc_ledger_open(&e, now);
    int open_segs = 0;
    for (int i = 0; i < e.ledger_count; i++)
        if (e.ledger[i].end_ns == 0)
            open_segs++;
    CHECK(open_segs == 1, "exactly one open segment after overflow open (got %d)",
          open_segs);
}

/* ── Test 6: rolling-hour aging frees budget back ──────────────────────── */
static void test_rolling_aging(void)
{
    printf("--- rolling hour: stale segments age out and free budget ---\n");
    struct pgwt_escalation e;
    init_engine(&e, 10 * SEC, false);

    /* Spend the whole budget at t≈0 (one 10s window). */
    pgwt_esc_ledger_open(&e, 0);
    pgwt_esc_ledger_close(&e, 10 * SEC);
    CHECK(pgwt_esc_budget_remaining_ns(&e, 10 * SEC) == 0,
          "budget exhausted right after the window");

    /* At exactly +1h the whole [0,10s] window is still inside the rolling
     * hour (window_start == 0): still fully spent. */
    CHECK(pgwt_esc_budget_remaining_ns(&e, HOUR) == 0,
          "still exhausted while the window is fully inside the rolling hour");

    /* Halfway aged (now = +1h+5s): only [5s,10s] remains in-window = 5s spent,
     * so 5s of budget has been freed. */
    CHECK(pgwt_esc_budget_remaining_ns(&e, HOUR + 5 * SEC) == 5 * SEC,
          "half the window aged out frees half the budget");

    /* Past the window end + 1h: fully aged out, budget fully restored. */
    uint64_t rem = pgwt_esc_budget_remaining_ns(&e, HOUR + 11 * SEC);
    CHECK(rem == 10 * SEC, "budget fully restored after aging (%.1fs)",
          (double)rem / 1e9);
}

/* ── Test 7: an EXTENSION only charges the committed remainder once (ESC-1) */
static void test_extension_no_double_charge(void)
{
    printf("--- ESC-1: extension charges committed remainder exactly once ---\n");
    struct pgwt_escalation e;
    init_engine(&e, 60 * SEC, false);

    uint64_t start = 0;
    uint64_t grant = 0;
    const char *why = NULL;

    /* Escalate 20s. */
    pgwt_esc_budget_decide(&e, start, 20 * SEC, &grant, &why);
    CHECK(grant == 20 * SEC, "first grant 20s");
    pgwt_esc_ledger_open(&e, start);

    /* 5s later, extend to +20s from now (deadline 25s). Consumed so far is the
     * open [0,5] = 5s; a further 20s is available well within the 60s budget,
     * so grant must be the full 20s (no phantom double-charge of the 15s still
     * committed). */
    uint64_t now = 5 * SEC;
    grant = 0;
    int rc = pgwt_esc_budget_decide(&e, now, 20 * SEC, &grant, &why);
    CHECK(rc == 0 && grant == 20 * SEC,
          "extension grants full 20s within budget (got %.1fs)",
          (double)grant / 1e9);

    /* Close at the extended deadline (25s). Total billed = 25s, NOT 45s. */
    pgwt_esc_ledger_close(&e, now + grant);
    uint64_t consumed = pgwt_esc_budget_consumed_ns(&e, now + grant);
    CHECK(consumed == 25 * SEC,
          "total billed = %.1fs (single window [0,25]), expected 25",
          (double)consumed / 1e9);
}

int main(void)
{
    test_fresh_grant();
    test_extend_every_second_caps();
    test_budget_over();
    test_zero_and_unlimited();
    test_ledger_overflow_billed();
    test_rolling_aging();
    test_extension_no_double_charge();

    printf("\n%d/%d checks passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
