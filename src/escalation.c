/* escalation.c — Tiered-mode escalation engine (Track A, D5)
 *
 * See escalation.h for the design. The engine flips the FULL tier's
 * watchpoints on (escalate) and off (deescalate) under a bounded, budgeted
 * window. The sampler is never stopped — it runs through the window so a
 * crash mid-escalation can't open a gap; A3's exact-wins merge removes the
 * overlap at read time.
 */
#include "escalation.h"
#include "daemon.h"
#include "backend.h"
#include "event_writer.h"
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

/* Sum full-fidelity ns consumed within [now - rolling_window, now], clamping
 * each segment to that window. The currently-open segment (end_ns == 0) is
 * counted up to now. Stale segments are pruned (compacted) as a side effect. */
static uint64_t budget_consumed_ns(struct pgwt_escalation *e, uint64_t now)
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
        /* keep this (still partly in-window) segment */
        e->ledger[w++] = e->ledger[i];
    }
    e->ledger_count = w;
    return consumed;
}

double pgwt_escalation_budget_remaining_s(const struct pgwt_daemon *d)
{
    const struct pgwt_escalation *e = &d->escalation;
    if (!e->enabled || e->budget_ns == 0)
        return 0;
    /* const-correct: budget_consumed_ns prunes, so cast away const. */
    uint64_t consumed = budget_consumed_ns((struct pgwt_escalation *)e,
                                           mono_ns());
    if (consumed >= e->budget_ns)
        return 0;
    return (double)(e->budget_ns - consumed) / 1e9;
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
            if (bpf_map_lookup_elem(state_fd, &key, &st) == 0)
                qid = st.last_query_id;   /* preserve the join key */
            struct pgwt_pid_state seed = {
                .last_event = 0,
                .last_ts = now,
                .last_query_id = qid,
                .wait_event_addr = be->wp_addr,
            };
            bpf_map_update_elem(state_fd, &key, &seed, BPF_ANY);
        }
    }
}

/* ── Public API ───────────────────────────────────────────────────────── */

int pgwt_escalation_init(struct pgwt_daemon *d, int budget_s)
{
    struct pgwt_escalation *e = &d->escalation;
    memset(e, 0, sizeof(*e));
    e->timer_fd = -1;
    e->rolling_window_ns = 3600ULL * 1000000000ULL;   /* per rolling hour */
    e->budget_ns = (budget_s > 0 ? (uint64_t)budget_s : 0) * 1000000000ULL;
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

    /* Budget check: would the requested window push consumed past budget?
     * The currently-open segment is already counted in budget_consumed_ns,
     * so for an extension we only charge the additional time past the current
     * deadline. */
    if (e->budget_ns > 0) {
        uint64_t consumed = budget_consumed_ns(e, now);
        uint64_t additional;
        if (e->active && e->window_deadline_ns > now) {
            uint64_t new_deadline = now + want_ns;
            additional = (new_deadline > e->window_deadline_ns)
                       ? (new_deadline - e->window_deadline_ns) : 0;
        } else {
            additional = want_ns;
        }
        if (consumed + additional > e->budget_ns) {
            e->denied_total++;
            if (why) *why = "escalation budget exhausted for this hour";
            return -1;
        }
    }

    if (e->active) {
        /* Already escalated: extend the deadline if the new one is later. */
        uint64_t new_deadline = now + want_ns;
        if (new_deadline > e->window_deadline_ns)
            arm_deadline(e, new_deadline);
        if (granted_s)
            *granted_s = (int)((e->window_deadline_ns - now + 999999999ULL)
                               / 1000000000ULL);
        return 0;
    }

    /* Fresh escalation: attach watchpoints across the registry, open a
     * ledger segment, arm the deadline, and mark the trace. */
    int n = attach_all(d);
    e->active = true;
    e->window_start_ns = now;
    e->window_reason = reason;
    e->windows_total++;

    if (e->ledger_count < PGWT_ESC_LEDGER_MAX) {
        e->ledger[e->ledger_count].start_ns = now;
        e->ledger[e->ledger_count].end_ns = 0;
        e->ledger_count++;
    }

    arm_deadline(e, now + want_ns);
    write_marker(d, PGWT_MARKER_ESCALATE_START, want_ns, reason);

    if (d->verbose)
        fprintf(stderr,
                "INFO: escalated to full fidelity for %ds (reason %d, "
                "%d watchpoints attached)\n", duration_s, reason, n);

    if (granted_s)
        *granted_s = duration_s;
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

    detach_all(d);
    e->active = false;

    /* Close the open ledger segment so budget accounting is exact. */
    for (int i = e->ledger_count - 1; i >= 0; i--) {
        if (e->ledger[i].end_ns == 0) {
            e->ledger[i].end_ns = now;
            break;
        }
    }

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
