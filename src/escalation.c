/* escalation.c — Tiered-mode escalation engine (Track A, D5)
 *
 * See escalation.h for the design. The engine flips the FULL tier's
 * watchpoints on (escalate) and off (deescalate) under a bounded, budgeted
 * window. The sampler is never stopped — it runs through the window so a
 * crash mid-escalation can't open a gap; A3's exact-wins merge removes the
 * overlap at read time.
 */
#include "escalation.h"
#include "escalation_budget.h"
#include "daemon.h"
#include "backend.h"
#include "discovery.h"
#include "event_writer.h"
#include "summary_writer.h"
#include "perf_event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "pg_wait_tracer.skel.h"

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Write an escalation window marker into the trace (and summary). pid = 0 so
 * no analysis confuses it with a backend; duration_ns = 0 (markers carry no
 * wait time — exactly like the query markers), and the window length + reason
 * are packed into query_id. Skipped from wait accumulation by PGWT_IS_MARKER,
 * so it never distorts the views. */
static void write_marker(struct pgwt_daemon *d, uint32_t marker_type,
                         uint64_t window_ns, int reason)
{
    if (!d->event_writer)
        return;
    uint64_t window_s = window_ns / 1000000000ULL;
    struct pgwt_trace_event ev = {
        .timestamp_ns = mono_ns(),
        .pid          = 0,
        .old_event    = marker_type,
        .new_event    = marker_type,
        .flags        = 0,
        .duration_ns  = 0,
        .query_id     = PGWT_ESC_PACK(window_s, reason),
    };
    pgwt_writer_push_event(d->event_writer, &ev);
    if (d->summary_writer)
        pgwt_summary_push_event(d->summary_writer, &ev);
}

/* ── Budget accounting (rolling hour) ─────────────────────────────────── */
/* The pure ledger/decision core lives in escalation_budget.c (unit-tested);
 * this file only wires it to the BPF-attaching escalation lifecycle. */

double pgwt_escalation_budget_remaining_s(const struct pgwt_daemon *d)
{
    const struct pgwt_escalation *e = &d->escalation;
    if (!e->enabled)
        return 0;
    /* ESC-6: report the TRUTH for every budget mode. Unlimited surfaces as
     * -1 (the "no cap" sentinel the UI renders as ∞); deny-all (budget 0)
     * reports a genuine 0; a finite budget reports the real remaining. */
    if (e->budget_unlimited)
        return -1.0;
    /* const-correct: the consumed helper prunes, so cast away const. */
    uint64_t rem = pgwt_esc_budget_remaining_ns((struct pgwt_escalation *)e,
                                                mono_ns());
    return (double)rem / 1e9;
}

double pgwt_escalation_remaining_s(const struct pgwt_daemon *d)
{
    const struct pgwt_escalation *e = &d->escalation;
    if (!e->active)
        return 0;
    uint64_t now = mono_ns();
    if (e->window_deadline_ns <= now)
        return 0;
    return (double)(e->window_deadline_ns - now) / 1e9;
}

/* ── Watchpoint attach / detach over the registry ─────────────────────── */

/* Attach watchpoints to every live, resolved backend. Backends whose address
 * the sampler has not yet resolved (freshly forked, wp_addr == 0) are left to
 * the fork->bootstrap->init lifecycle, which is active while escalated. */
static int attach_all(struct pgwt_daemon *d)
{
    uint64_t now = mono_ns();
    int n = 0;
    for (int i = 0; i < d->backends.count; i++) {
        struct pgwt_backend *be = &d->backends.entries[i];
        if (!be->is_alive || be->pid <= 0 || be->wp_addr == 0)
            continue;
        if (be->wp_fd >= 0)
            continue;
        /* Refresh attach_ts so the pre-seeded interval starts at the escalate
         * instant (matters on re-escalation, where attach_ts is stale). */
        be->attach_ts = now;
        if (pgwt_attach_backend_watchpoint(d, be) == 0)
            n++;
    }
    return n;
}

/* Detach all watchpoints (real + bootstrap) and RESET each backend's BPF
 * state_map entry to a query-id-only seed (last_event = 0, wait_event_addr
 * preserved). This clears the watchpoint-maintained open interval so the
 * de-escalated sampler reads no stale interval, while KEEPING the entry alive
 * so the query_id uprobe (which only updates existing entries) and the
 * sampler's pid->query_id join keep working after the window. The registry
 * entries themselves survive — the sampler keeps reading wp_addr. */
static void detach_all(struct pgwt_daemon *d)
{
    int state_fd = d->skel ? bpf_map__fd(d->skel->maps.state_map) : -1;
    uint64_t now = mono_ns();
    for (int i = 0; i < d->backends.count; i++) {
        struct pgwt_backend *be = &d->backends.entries[i];
        if (be->wp_fd >= 0) {
            pgwt_close_watchpoint(be->wp_fd);
            be->wp_fd = -1;
        }
        if (be->bootstrap_fd >= 0) {
            pgwt_close_watchpoint(be->bootstrap_fd);
            be->bootstrap_fd = -1;
        }
        if (state_fd >= 0 && be->is_alive && be->pid > 0) {
            uint32_t key = (uint32_t)be->pid;
            struct pgwt_pid_state st;
            uint64_t qid = 0;
            uint16_t cmd_open = 0;
            if (bpf_map_lookup_elem(state_fd, &key, &st) == 0) {
                qid = st.last_query_id;   /* preserve the join key */
                cmd_open = st.cmd_open;   /* preserve the command gate */
            }
            /* wp_live = 0: the entry is a query-id/cmd-gate seed again —
             * on_exit must NOT close an interval from it (defect 2), and
             * the next escalation's preseed re-arms it. */
            struct pgwt_pid_state seed = {
                .last_event = 0,
                .wp_live = 0,
                .cmd_open = cmd_open,
                .last_ts = now,
                .last_query_id = qid,
                .wait_event_addr = be->wp_addr,
            };
            bpf_map_update_elem(state_fd, &key, &seed, BPF_ANY);
        }
    }
}

/* ── De-escalation flush (ESC-2) ──────────────────────────────────────── */

/* Before detach_all() resets the state_map, flush every OPEN wait interval as
 * a final transition so a wait that spans the window end is recorded exactly
 * once (its exact portion up to the de-escalation instant) instead of being
 * silently dropped — the interval carried no TRANSITIONS record yet (the
 * watchpoint only emits on the wait's END), while the END marker extends the
 * exact-wins range so the covering samples were discarded too. We synthesize
 * the transition userspace-side (same path as the window markers): timestamp =
 * now (the wait-end instant), old_event = the open event, duration = now -
 * last_ts. It lands inside the marker-covered range, closing the end-of-window
 * hole on exactly the long waits the window exists to capture. */
static void flush_open_intervals(struct pgwt_daemon *d, uint64_t now)
{
    if (!d->event_writer)
        return;   /* nothing to flush into — no trace recording */
    int state_fd = d->skel ? bpf_map__fd(d->skel->maps.state_map) : -1;
    if (state_fd < 0)
        return;

    for (int i = 0; i < d->backends.count; i++) {
        struct pgwt_backend *be = &d->backends.entries[i];
        if (!be->is_alive || be->pid <= 0 || be->wp_fd < 0)
            continue;
        uint32_t key = (uint32_t)be->pid;
        struct pgwt_pid_state st;
        if (bpf_map_lookup_elem(state_fd, &key, &st) != 0)
            continue;
        if (!st.wp_live)
            continue;               /* a seed entry — no interval to close */
        if (now <= st.last_ts)
            continue;               /* zero/negative duration — nothing open */
        uint32_t we = st.last_event;
        if (PGWT_IS_MARKER(we))
            continue;               /* never a real wait */
        /* T8: close the open stretch with its MEASURED CPU. For an on-CPU
         * stretch (we==0) this is the straddling command's CPU that the
         * watchpoint never emitted (no wait boundary fired); for a wait it is
         * ≈0 (the self-check). Read schedstat now and subtract the seeded base;
         * a failed read leaves UNKNOWN so compute infers rather than records a
         * false 0. */
        uint64_t cpu_ns = PGWT_CPU_NS_UNKNOWN;
        if (d->cpu_accounting) {
            uint64_t sched_now = pgwt_read_sched_cpu_ns(be->pid);
            if (sched_now == 0)
                cpu_ns = PGWT_CPU_NS_UNKNOWN;
            else if (st.last_cpu_ns && sched_now >= st.last_cpu_ns)
                cpu_ns = sched_now - st.last_cpu_ns;
            else
                cpu_ns = 0;
        }
        struct pgwt_trace_event ev = {
            .timestamp_ns = now,
            .pid          = (uint32_t)be->pid,
            .old_event    = we,     /* the interval that was open ends here */
            .new_event    = we,
            .flags        = st.cmd_open ? PGWT_EVENT_FLAG_CMD_OPEN : 0,
            .duration_ns  = now - st.last_ts,
            .query_id     = st.last_query_id,
            .cpu_ns       = cpu_ns,
        };
        pgwt_writer_push_event(d->event_writer, &ev);
        if (d->summary_writer)
            pgwt_summary_push_event(d->summary_writer, &ev);
    }
}

/* T8 symptom #2: flush open exact intervals at daemon shutdown. Full mode has
 * no de-escalation path, so a command computing when capture ends (window
 * close / daemon exit) emitted NOTHING — the on-CPU stretch was never closed
 * (the watchpoint only fires at wait boundaries, and a pure-CPU command reaches
 * none), leaving an EMPTY trace. This closes every open wp_live interval with
 * its measured cpu_ns, exactly like de-escalation. Idempotent after
 * pgwt_deescalate (which flushed + detached, so no wp_live entries remain);
 * safe in any mode (a no-op when nothing is attached). */
void pgwt_flush_open_intervals(struct pgwt_daemon *d)
{
    flush_open_intervals(d, mono_ns());
}

/* ── Public API ───────────────────────────────────────────────────────── */

int pgwt_escalation_init(struct pgwt_daemon *d, int budget_s)
{
    struct pgwt_escalation *e = &d->escalation;
    memset(e, 0, sizeof(*e));
    e->timer_fd = -1;
    e->rolling_window_ns = 3600ULL * 1000000000ULL;   /* per rolling hour */
    /* ESC-6 budget semantics: >0 = seconds/hour cap; 0 = deny-all (escalation
     * disabled); <0 = unlimited (no cap). */
    if (budget_s < 0) {
        e->budget_unlimited = true;
        e->budget_ns = 0;
    } else {
        e->budget_unlimited = false;
        e->budget_ns = (uint64_t)budget_s * 1000000000ULL;
    }
    e->enabled = (d->mode == PGWT_MODE_TIERED);

    if (!e->enabled)
        return 0;

    e->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (e->timer_fd < 0) {
        perror("timerfd_create (escalation)");
        return -1;
    }
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = e->timer_fd };
    if (epoll_ctl(d->epoll_fd, EPOLL_CTL_ADD, e->timer_fd, &ev) != 0) {
        perror("epoll_ctl (escalation timer)");
        close(e->timer_fd);
        e->timer_fd = -1;
        return -1;
    }
    return 0;
}

void pgwt_escalation_cleanup(struct pgwt_daemon *d)
{
    struct pgwt_escalation *e = &d->escalation;
    if (e->active)
        pgwt_deescalate(d, PGWT_ESC_REASON_SHUTDOWN);
    if (e->timer_fd >= 0) {
        epoll_ctl(d->epoll_fd, EPOLL_CTL_DEL, e->timer_fd, NULL);
        close(e->timer_fd);
        e->timer_fd = -1;
    }
}

bool pgwt_escalation_is_timer_fd(const struct pgwt_daemon *d, int fd)
{
    return d->escalation.enabled && d->escalation.timer_fd >= 0
        && fd == d->escalation.timer_fd;
}

static void arm_deadline(struct pgwt_escalation *e, uint64_t deadline_ns)
{
    e->window_deadline_ns = deadline_ns;
    struct itimerspec its = {
        .it_value = {
            .tv_sec  = (time_t)(deadline_ns / 1000000000ULL),
            .tv_nsec = (long)(deadline_ns % 1000000000ULL),
        },
    };
    /* TFD_TIMER_ABSTIME: fire at the absolute monotonic deadline. */
    timerfd_settime(e->timer_fd, TFD_TIMER_ABSTIME, &its, NULL);
}

int pgwt_escalate(struct pgwt_daemon *d, int duration_s, int reason,
                  int *granted_s, const char **why)
{
    struct pgwt_escalation *e = &d->escalation;

    if (!e->enabled) {
        if (why) *why = "escalation requires --mode tiered";
        return -1;
    }
    if (duration_s <= 0) {
        if (why) *why = "duration_s must be positive";
        return -1;
    }

    uint64_t now = mono_ns();
    uint64_t want_ns = (uint64_t)duration_s * 1000000000ULL;

    /* ESC-1: budget decision on the committed-remainder-aware core. The grant
     * is CLAMPED to the remaining budget: the currently-open segment is
     * already folded into consumed, so the deadline we arm at now+grant makes
     * the window's total full-fidelity time land at exactly the budget no
     * matter how many times it is extended (the extend-every-second attack). */
    uint64_t grant_ns = want_ns;
    if (pgwt_esc_budget_decide(e, now, want_ns, &grant_ns, why) != 0) {
        e->denied_total++;
        return -1;
    }

    if (e->active) {
        /* Already escalated: extend the deadline to the budget-clamped grant
         * if it is later than the current one (never shorten a live window). */
        uint64_t new_deadline = now + grant_ns;
        if (new_deadline > e->window_deadline_ns)
            arm_deadline(e, new_deadline);
        if (granted_s)
            *granted_s = (int)((e->window_deadline_ns - now + 999999999ULL)
                               / 1000000000ULL);
        return 0;
    }

    /* Fresh escalation: attach watchpoints across the registry, open a
     * ledger segment, arm the (budget-clamped) deadline, and mark the trace. */
    int n = attach_all(d);
    e->active = true;
    e->window_start_ns = now;
    e->window_reason = reason;
    e->windows_total++;

    pgwt_esc_ledger_open(e, now);   /* ESC-5: never opens an unbilled window */

    arm_deadline(e, now + grant_ns);
    write_marker(d, PGWT_MARKER_ESCALATE_START, grant_ns, reason);

    int granted = (int)((grant_ns + 999999999ULL) / 1000000000ULL);
    if (d->verbose)
        fprintf(stderr,
                "INFO: escalated to full fidelity for %ds (reason %d, "
                "%d watchpoints attached)\n", granted, reason, n);

    if (granted_s)
        *granted_s = granted;
    return 0;
}

void pgwt_deescalate(struct pgwt_daemon *d, int reason)
{
    struct pgwt_escalation *e = &d->escalation;
    if (!e->active)
        return;

    uint64_t now = mono_ns();

    /* Drain any final transitions captured before tearing down. */
    if (d->event_rb)
        ring_buffer__consume(d->event_rb);

    /* ESC-2: flush open wait intervals as final transitions BEFORE detach_all
     * wipes them, and before the END marker closes the exact-wins range — so a
     * wait spanning the window boundary is recorded exactly once (its exact
     * portion), not dropped into an end-of-window hole. */
    flush_open_intervals(d, now);

    detach_all(d);
    e->active = false;

    /* Close the open ledger segment so budget accounting is exact. */
    pgwt_esc_ledger_close(e, now);

    /* Disarm the deadline timer. */
    struct itimerspec its = {0};
    if (e->timer_fd >= 0)
        timerfd_settime(e->timer_fd, 0, &its, NULL);

    uint64_t elapsed = now - e->window_start_ns;
    write_marker(d, PGWT_MARKER_ESCALATE_END, elapsed, reason);

    if (d->verbose)
        fprintf(stderr,
                "INFO: de-escalated to sampled (reason %d, window %.1fs)\n",
                reason, (double)elapsed / 1e9);
}

void pgwt_escalation_on_timer(struct pgwt_daemon *d)
{
    struct pgwt_escalation *e = &d->escalation;
    uint64_t expirations;
    ssize_t r = read(e->timer_fd, &expirations, sizeof(expirations));
    (void)r;

    if (e->active && mono_ns() >= e->window_deadline_ns)
        pgwt_deescalate(d, PGWT_ESC_REASON_EXPIRED);
}

void pgwt_escalation_check_budget(struct pgwt_daemon *d)
{
    struct pgwt_escalation *e = &d->escalation;
    if (!e->active)
        return;
    /* ESC-1 mid-window clamp: the budget-clamped deadline normally closes the
     * window on time, but a rolling-hour accounting change (an older segment
     * aging in/out) can push cumulative consumption to the budget mid-flight —
     * close now rather than run over. */
    if (pgwt_esc_budget_over(e, mono_ns())) {
        e->budget_closed_total++;
        pgwt_deescalate(d, PGWT_ESC_REASON_BUDGET);
    }
}
