/* pg_wait_tracer BPF programs — wait event tracing via ringbuf
 *
 * Programs:
 *   on_watchpoint  — fires on PGPROC->wait_event_info write, emits trace event
 *   on_bootstrap   — fires when InitProcess() writes my_wait_event_info pointer
 *   on_fork        — fires on postmaster fork, notifies daemon of new backend
 *   on_exit        — fires on backend exit, closes last interval, notifies daemon
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

/* ── Helpers ──────────────────────────────────────────────── */

/* Read the current query_id from PgBackendStatus via MyBEEntry.
 * Double-dereference: my_be_entry_addr → PgBackendStatus* → st_query_id. */
static __always_inline u64 read_query_id(void)
{
    if (!my_be_entry_addr || !st_query_id_offset) return 0;
    u64 be_entry = 0;
    bpf_probe_read_user(&be_entry, sizeof(be_entry), (void *)my_be_entry_addr);
    if (!be_entry) return 0;
    u64 qid = 0;
    bpf_probe_read_user(&qid, sizeof(qid),
                         (void *)(be_entry + st_query_id_offset));
    return qid;
}

/* Read the current wait_event_info value via double-dereference.
 * my_wait_ptr_addr → pointer → uint32 value.
 * Works for all backends: each runs in its own address space. */
static __always_inline u32 read_wait_event(void)
{
    u32 val = 0;
    u64 ptr = 0;

    bpf_probe_read_user(&ptr, sizeof(ptr), (void *)my_wait_ptr_addr);
    if (ptr)
        bpf_probe_read_user(&val, 4, (void *)ptr);
    return val;
}

/* ── Program 1: on_watchpoint ─────────────────────────────── */
/* Fires on every write to PGPROC->wait_event_info.
 * State machine: computes duration of PREVIOUS state, accumulates. */

SEC("perf_event")
int on_watchpoint(struct bpf_perf_event_data *ctx)
{
    u32 pid = bpf_get_current_pid_tgid() >> 32;
    u64 now = bpf_ktime_get_ns();
    u32 new_event = read_wait_event();

    u64 cur_query_id = read_query_id();

    struct pgwt_pid_state *st = bpf_map_lookup_elem(&state_map, &pid);
    if (st) {
        /* Compute duration of PREVIOUS state */
        u64 duration = now - st->last_ts;

        /* Emit trace event to ringbuf */
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

        /* Transition to new state */
        st->last_event = new_event;
        st->last_ts = now;
        st->last_query_id = cur_query_id;
    } else {
        /* First event for this PID — initialize, no accumulation */
        struct pgwt_pid_state new_st = {
            .last_event = new_event,
            .last_ts = now,
            .last_query_id = cur_query_id,
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
