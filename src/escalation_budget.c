/* escalation_budget.c — Pure rolling-hour budget accounting (Track T, T3)
 *
 * See escalation_budget.h. BPF-free and daemon-free: compiled into the daemon
 * (via escalation.c's caller) and directly into the unit test with no special
 * defines.
 */
#include "escalation_budget.h"
#include "escalation.h"

#include <string.h>

uint64_t pgwt_esc_budget_consumed_ns(struct pgwt_escalation *e, uint64_t now)
{
    uint64_t window_start = (now > e->rolling_window_ns)
                          ? now - e->rolling_window_ns : 0;
    uint64_t consumed = 0;
    int w = 0;
    for (int i = 0; i < e->ledger_count; i++) {
        uint64_t s = e->ledger[i].start_ns;
        uint64_t en = e->ledger[i].end_ns ? e->ledger[i].end_ns : now;
        if (en <= window_start)
            continue;   /* entirely stale — drop */
        if (s < window_start)
            s = window_start;
        if (en > s)
            consumed += en - s;
        /* keep this (still partly in-window) segment, preserving order */
        e->ledger[w++] = e->ledger[i];
    }
    e->ledger_count = w;
    return consumed;
}

uint64_t pgwt_esc_budget_remaining_ns(struct pgwt_escalation *e, uint64_t now)
{
    if (e->budget_unlimited)
        return UINT64_MAX;
    if (e->budget_ns == 0)
        return 0;   /* deny-all: genuinely zero remaining (ESC-6) */
    uint64_t consumed = pgwt_esc_budget_consumed_ns(e, now);
    return (e->budget_ns > consumed) ? e->budget_ns - consumed : 0;
}

int pgwt_esc_budget_decide(struct pgwt_escalation *e, uint64_t now,
                           uint64_t want_ns, uint64_t *grant_ns,
                           const char **why)
{
    if (e->budget_unlimited) {
        if (grant_ns) *grant_ns = want_ns;
        return 0;
    }
    if (e->budget_ns == 0) {
        /* ESC-6: 0 means deny-all (not "unlimited"). --escalation-budget
         * unlimited (-1) is the explicit opt-in for no limit. */
        if (why) *why = "escalation disabled (--escalation-budget 0); "
                        "use 'unlimited' to allow unbounded escalation";
        return -1;
    }
    uint64_t consumed = pgwt_esc_budget_consumed_ns(e, now);
    uint64_t avail = (e->budget_ns > consumed) ? e->budget_ns - consumed : 0;
    if (avail == 0) {
        if (why) *why = "escalation budget exhausted for this hour";
        return -1;
    }
    if (grant_ns)
        *grant_ns = (want_ns < avail) ? want_ns : avail;
    return 0;
}

bool pgwt_esc_budget_over(struct pgwt_escalation *e, uint64_t now)
{
    if (e->budget_unlimited || e->budget_ns == 0)
        return false;
    return pgwt_esc_budget_consumed_ns(e, now) >= e->budget_ns;
}

/* Merge the two oldest ledger segments into one spanning [min start, max end],
 * shifting the tail down. Bills the gap between them (conservative). Segments
 * are appended in chronological open order and pruning preserves that order,
 * so [0] and [1] are always the two oldest. */
static void coalesce_oldest(struct pgwt_escalation *e)
{
    if (e->ledger_count < 2)
        return;
    uint64_t s0 = e->ledger[0].start_ns, en0 = e->ledger[0].end_ns;
    uint64_t s1 = e->ledger[1].start_ns, en1 = e->ledger[1].end_ns;
    uint64_t start = s0 < s1 ? s0 : s1;
    uint64_t end;
    if (en0 == 0 || en1 == 0)
        end = 0;                        /* one still open — stays open */
    else
        end = en0 > en1 ? en0 : en1;
    e->ledger[0].start_ns = start;
    e->ledger[0].end_ns = end;
    memmove(&e->ledger[1], &e->ledger[2],
            (size_t)(e->ledger_count - 2) * sizeof(e->ledger[0]));
    e->ledger_count--;
}

void pgwt_esc_ledger_open(struct pgwt_escalation *e, uint64_t now)
{
    /* Prune stale first (may itself free slots). */
    pgwt_esc_budget_consumed_ns(e, now);
    if (e->ledger_count >= PGWT_ESC_LEDGER_MAX)
        coalesce_oldest(e);   /* ESC-5: guarantee a slot; never unbilled */
    if (e->ledger_count < PGWT_ESC_LEDGER_MAX) {
        e->ledger[e->ledger_count].start_ns = now;
        e->ledger[e->ledger_count].end_ns = 0;
        e->ledger_count++;
    }
}

void pgwt_esc_ledger_close(struct pgwt_escalation *e, uint64_t now)
{
    for (int i = e->ledger_count - 1; i >= 0; i--) {
        if (e->ledger[i].end_ns == 0) {
            e->ledger[i].end_ns = now;
            break;
        }
    }
}
