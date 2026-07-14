/* escalation_budget.h — Pure rolling-hour budget accounting (Track T, T3)
 *
 * The escalation engine's budget math is split out here as a BPF-free,
 * daemon-free core so it is exhaustively unit-testable (tests/test_escalation_
 * budget.c) against a scripted clock — the same pattern anomaly.c / sampler.c
 * use for their pure cores. It owns:
 *
 *   - the rolling-hour "consumed seconds" ledger (prune + compact),
 *   - the grant decision (unlimited / deny-all / finite-clamp) that makes
 *     repeated extensions cap total full-fidelity time at EXACTLY the budget
 *     (ESC-1),
 *   - never-unbilled ledger management under overflow (ESC-5),
 *   - honest "remaining" that reports the truth for every budget mode (ESC-6).
 *
 * All functions operate on `struct pgwt_escalation` (declared in escalation.h);
 * none touch BPF, the daemon, or the trace writer.
 */
#ifndef PGWT_ESCALATION_BUDGET_H
#define PGWT_ESCALATION_BUDGET_H

#include <stdbool.h>
#include <stdint.h>

struct pgwt_escalation;   /* full definition in escalation.h */

/* Sum full-fidelity ns consumed within [now - rolling_window, now], clamping
 * each segment to that window. The currently-open segment (end_ns == 0) is
 * counted up to now. Prunes (compacts) fully-stale segments as a side effect,
 * preserving chronological order. */
uint64_t pgwt_esc_budget_consumed_ns(struct pgwt_escalation *e, uint64_t now);

/* Full-fidelity ns still available in the current rolling hour. Returns
 * UINT64_MAX for an unlimited budget, 0 for a deny-all (budget == 0) engine. */
uint64_t pgwt_esc_budget_remaining_ns(struct pgwt_escalation *e, uint64_t now);

/* Decide whether a window of want_ns may be granted at `now` under the budget.
 * On success returns 0 and *grant_ns receives the (possibly budget-clamped)
 * grant. On denial returns -1 and *why (if non-NULL) receives a static reason.
 *
 * The currently-open segment is already folded into consumed, so the future
 * commitment charged is exactly *grant_ns and the invariant
 *   consumed_now + grant_ns <= budget
 * holds — which, applied on every extension, caps the window's total
 * full-fidelity time at the budget no matter how often it is extended (ESC-1).
 * unlimited => always grant want_ns; budget == 0 => always deny (ESC-6). */
int pgwt_esc_budget_decide(struct pgwt_escalation *e, uint64_t now,
                           uint64_t want_ns, uint64_t *grant_ns,
                           const char **why);

/* True when an active window's rolling-hour consumption has reached/passed the
 * budget and the window must be closed mid-flight (ESC-1 mid-window clamp).
 * Always false for unlimited / deny-all engines. */
bool pgwt_esc_budget_over(struct pgwt_escalation *e, uint64_t now);

/* Open a ledger segment at `now`. Prunes stale segments first; if the ledger
 * is still full, coalesces the two oldest segments (billing the gap between
 * them — conservative, never under-bills) so a window is NEVER opened
 * unbilled (ESC-5). */
void pgwt_esc_ledger_open(struct pgwt_escalation *e, uint64_t now);

/* Close the most recently opened (end_ns == 0) segment at `now`. */
void pgwt_esc_ledger_close(struct pgwt_escalation *e, uint64_t now);

#endif /* PGWT_ESCALATION_BUDGET_H */
