/* pg_wait_tracer BPF programs — wait event tracing via ringbuf
 *
 * Programs:
 *   on_watchpoint  — fires on PGPROC->wait_event_info write, emits trace event
 *   on_bootstrap   — fires when InitProcess() writes my_wait_event_info pointer
 *   on_fork        — fires on postmaster fork, notifies daemon of new backend
 *   on_exit        — fires on backend exit, closes last interval, notifies daemon
 *
 * Modes (set via lightweight_mode before load):
 *   0 = full trace: emit every event to event_ringbuf (for --trace / recording)
 *   1 = lightweight: accumulate durations in accum_map (for --view, lower overhead)
 */

#ifndef __BPF__
#define __BPF__
#endif
#include "pg_wait_tracer.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

/* ── Constants from userspace (set before load via .rodata) ── */
volatile const u32 target_postmaster_pid = 0;
volatile const u64 my_wait_ptr_addr = 0;
volatile const u64 my_be_entry_addr = 0;  /* address of MyBEEntry global */
volatile const u32 st_query_id_offset = 0; /* offsetof(PgBackendStatus, st_query_id) */
volatile const u32 lightweight_mode = 0;   /* 0=full trace, 1=lightweight (no ringbuf) */

/* ── Maps ─────────────────────────────────────────────────── */

/* Per-PID state: last event + timestamp for duration computation.
 * Must be regular HASH (not PERCPU) because a backend can migrate
 * between CPUs — we need the same last_ts regardless of CPU. */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 4096);
    __type(key, u32);
    __type(value, struct pgwt_pid_state);
} state_map SEC(".maps");

/* Ring buffer for lifecycle events — pollable via epoll */
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 64 * 1024);
} lifecycle_rb SEC(".maps");

/* Ring buffer for raw trace events — every wait event transition */
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 64 * 1024 * 1024);  /* 64MB */
} event_ringbuf SEC(".maps");

/* Accumulator map for lightweight mode: per-event duration totals.
 * Key = wait_event_info (u32), Value = {total_ns, count}.
 * Daemon reads and zeroes this periodically. */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, ACCUM_MAP_MAX_ENTRIES);
    __type(key, u32);
    __type(value, struct pgwt_accum_val);
} accum_map SEC(".maps");

/* ── Helpers ──────────────────────────────────────────────── */

/* Read query_id using cached be_entry pointer (1 probe_read instead of 2).
 * Falls back to double-deref if cache is empty. */
static __always_inline u64 read_query_id_cached(u64 cached_be_entry)
{
    if (!st_query_id_offset) return 0;
    u64 be_entry = cached_be_entry;
    if (!be_entry) {
        if (!my_be_entry_addr) return 0;
        bpf_probe_read_user(&be_entry, sizeof(be_entry),
                             (void *)my_be_entry_addr);
        if (!be_entry) return 0;
    }
    u64 qid = 0;
    bpf_probe_read_user(&qid, sizeof(qid),
                         (void *)(be_entry + st_query_id_offset));
    return qid;
}

/* Resolve and return PgBackendStatus* for caching in state_map. */
static __always_inline u64 resolve_be_entry(void)
{
    if (!my_be_entry_addr) return 0;
    u64 be_entry = 0;
    bpf_probe_read_user(&be_entry, sizeof(be_entry),
                         (void *)my_be_entry_addr);
    return be_entry;
}

/* Read wait_event_info directly from PGPROC address (1 probe_read).
 * Used when we have the cached address from state_map. */
static __always_inline u32 read_wait_event_direct(u64 addr)
{
    u32 val = 0;
    if (addr)
        bpf_probe_read_user(&val, 4, (void *)addr);
    return val;
}

/* Read the current wait_event_info value via double-dereference.
 * my_wait_ptr_addr → pointer → uint32 value.
 * Used only on first event for a PID (before we cache the address). */
static __always_inline u32 read_wait_event(u64 *out_addr)
{
    u32 val = 0;
    u64 ptr = 0;

    bpf_probe_read_user(&ptr, sizeof(ptr), (void *)my_wait_ptr_addr);
    if (out_addr)
        *out_addr = ptr;
    if (ptr)
        bpf_probe_read_user(&val, 4, (void *)ptr);
    return val;
}

/* Accumulate duration for a wait event into accum_map. */
static __always_inline void accum_add(u32 event, u64 duration)
{
    struct pgwt_accum_val *av = bpf_map_lookup_elem(&accum_map, &event);
    if (av) {
        __sync_fetch_and_add(&av->total_ns, duration);
        __sync_fetch_and_add(&av->count, 1);
    } else {
        struct pgwt_accum_val new_av = { .total_ns = duration, .count = 1 };
        bpf_map_update_elem(&accum_map, &event, &new_av, BPF_NOEXIST);
    }
}

/* ── Program 1: on_watchpoint ─────────────────────────────── */
/* Fires on every write to PGPROC->wait_event_info.
 * State machine: computes duration of PREVIOUS state, accumulates. */

SEC("perf_event")
int on_watchpoint(struct bpf_perf_event_data *ctx)
{
    u32 pid = bpf_get_current_pid_tgid() >> 32;

    struct pgwt_pid_state *st = bpf_map_lookup_elem(&state_map, &pid);
    if (st) {
        /* Fast path: read wait_event directly (1 probe_read vs 2) */
        u32 new_event = read_wait_event_direct(st->wait_event_addr);

        /* Skip redundant writes — watchpoint fires even if value unchanged */
        if (new_event == st->last_event)
            return 0;

        u64 now = bpf_ktime_get_ns();
        u64 duration = now - st->last_ts;

        if (lightweight_mode) {
            /* Lightweight: accumulate in BPF map, no ringbuf */
            accum_add(st->last_event, duration);
        } else {
            /* Full trace: emit per-event to ringbuf */
            u64 cur_query_id = read_query_id_cached(st->be_entry_ptr);

            struct pgwt_trace_event *evt =
                bpf_ringbuf_reserve(&event_ringbuf, sizeof(*evt), 0);
            if (evt) {
                evt->timestamp_ns = now;
                evt->pid = pid;
                evt->old_event = st->last_event;
                evt->new_event = new_event;
                evt->duration_ns = duration;
                evt->query_id = st->last_query_id;
                bpf_ringbuf_submit(evt, 0);
            }

            st->last_query_id = cur_query_id;
        }

        /* Transition to new state */
        st->last_event = new_event;
        st->last_ts = now;
    } else {
        /* First event for this PID — initialize via double-deref,
         * cache the resolved address for future fast-path reads */
        u64 wait_addr = 0;
        u32 new_event = read_wait_event(&wait_addr);
        struct pgwt_pid_state new_st = {
            .last_event = new_event,
            .last_ts = bpf_ktime_get_ns(),
            .last_query_id = 0,
            .be_entry_ptr = resolve_be_entry(),
            .wait_event_addr = wait_addr,
        };
        bpf_map_update_elem(&state_map, &pid, &new_st, BPF_ANY);
    }
    return 0;
}

/* ── Program 2: on_bootstrap ──────────────────────────────── */
/* Fires when InitProcess() writes the my_wait_event_info pointer.
 * Sends {pid, PGPROC->wait_event_info addr} to daemon via ring buffer. */

SEC("perf_event")
int on_bootstrap(struct bpf_perf_event_data *ctx)
{
    u32 pid = bpf_get_current_pid_tgid() >> 32;

    u64 ptr = 0;
    bpf_probe_read_user(&ptr, sizeof(ptr), (void *)my_wait_ptr_addr);

    if (ptr != 0) {
        struct pgwt_lifecycle_event *ev;
        ev = bpf_ringbuf_reserve(&lifecycle_rb, sizeof(*ev), 0);
        if (ev) {
            ev->type = PGWT_LIFECYCLE_INIT;
            ev->pid = pid;
            ev->addr = ptr;
            ev->timestamp = bpf_ktime_get_ns();
            bpf_ringbuf_submit(ev, 0);
        }
    }
    return 0;
}

/* ── Program 3: on_fork ───────────────────────────────────── */
/* Detects new PG backends forked by postmaster. */

SEC("tp/sched/sched_process_fork")
int on_fork(struct trace_event_raw_sched_process_fork *ctx)
{
    u32 parent = ctx->parent_pid;
    u32 child  = ctx->child_pid;

    if (parent != target_postmaster_pid)
        return 0;

    struct pgwt_lifecycle_event *ev;
    ev = bpf_ringbuf_reserve(&lifecycle_rb, sizeof(*ev), 0);
    if (ev) {
        ev->type = PGWT_LIFECYCLE_FORK;
        ev->pid = child;
        ev->addr = 0;
        ev->timestamp = bpf_ktime_get_ns();
        bpf_ringbuf_submit(ev, 0);
    }
    return 0;
}

/* ── Program 4: on_exit ───────────────────────────────────── */
/* Detects exiting backends. Closes last open interval first. */

SEC("tp/sched/sched_process_exit")
int on_exit(struct trace_event_raw_sched_process_template *ctx)
{
    u32 pid = ctx->pid;

    struct pgwt_pid_state *st = bpf_map_lookup_elem(&state_map, &pid);
    if (!st)
        return 0;

    /* Close the last open interval */
    u64 now = bpf_ktime_get_ns();
    u64 duration = now - st->last_ts;

    if (lightweight_mode) {
        accum_add(st->last_event, duration);
    } else {
        /* Emit final trace event (exit sentinel) */
        struct pgwt_trace_event *evt =
            bpf_ringbuf_reserve(&event_ringbuf, sizeof(*evt), 0);
        if (evt) {
            evt->timestamp_ns = now;
            evt->pid = pid;
            evt->old_event = st->last_event;
            evt->new_event = PGWT_EVENT_EXIT;
            evt->duration_ns = duration;
            evt->query_id = st->last_query_id;
            bpf_ringbuf_submit(evt, 0);
        }
    }

    /* Notify daemon */
    struct pgwt_lifecycle_event *ev;
    ev = bpf_ringbuf_reserve(&lifecycle_rb, sizeof(*ev), 0);
    if (ev) {
        ev->type = PGWT_LIFECYCLE_EXIT;
        ev->pid = pid;
        ev->addr = 0;
        ev->timestamp = now;
        bpf_ringbuf_submit(ev, 0);
    }
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
