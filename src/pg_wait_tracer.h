/* pg_wait_tracer.h — Shared types between BPF and userspace */
#ifndef PG_WAIT_TRACER_H
#define PG_WAIT_TRACER_H

#ifdef __BPF__
#include "vmlinux.h"
#else
#include <stdint.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

/* ── Histogram ────────────────────────────────────────────── */
#define HISTOGRAM_BUCKETS    16
/* Bucket boundaries in microseconds:
 * 0: <1, 1: 1-2, 2: 2-4, 3: 4-8, 4: 8-16, 5: 16-32,
 * 6: 32-64, 7: 64-128, 8: 128-256, 9: 256-512, 10: 512-1024,
 * 11: 1ms-2ms, 12: 2-4ms, 13: 4-8ms, 14: 8-16ms, 15: >=16ms */

/* ── Limits ───────────────────────────────────────────────── */
#define MAX_BACKENDS         1024
#define MAX_EVENTS_PER_PID   256
#define MAX_CPUS             128

/* ── BPF Map Structs ──────────────────────────────────────── */

/* Per-PID state in state_map */
struct pgwt_pid_state {
    u32 last_event;     /* previous wait_event_info value (0 = on CPU) */
    u64 last_ts;        /* ktime_ns of last transition */
    u64 last_query_id;  /* query_id active during last_event */
    u64 be_entry_ptr;   /* cached PgBackendStatus* (avoids 1 probe_read per event) */
    u64 wait_event_addr; /* direct PGPROC->wait_event_info address (saves 1 probe_read) */
};

/* ── Lifecycle Events (ring buffer) ───────────────────────── */

enum pgwt_lifecycle_type {
    PGWT_LIFECYCLE_FORK = 1,
    PGWT_LIFECYCLE_INIT = 2,
    PGWT_LIFECYCLE_EXIT = 3,
};

struct pgwt_lifecycle_event {
    u32 type;       /* enum pgwt_lifecycle_type */
    u32 pid;
    u64 addr;       /* for INIT: PGPROC->wait_event_info address */
    u64 timestamp;
};

/* ── Raw Trace Event (ringbuf, 36 bytes) ─────────────────── */

struct pgwt_trace_event {
    u64 timestamp_ns;   /* absolute ktime_get_ns() */
    u32 pid;            /* backend PID */
    u32 old_event;      /* previous wait_event_info */
    u32 new_event;      /* new wait_event_info (0xFFFFFFFF = exit) */
    u32 _pad;           /* alignment */
    u64 duration_ns;    /* time spent in old_event */
    u64 query_id;       /* active query during old_event */
};

#define PGWT_EVENT_EXIT  0xFFFFFFFFU  /* sentinel new_event for process exit */

/* Query lifecycle markers — emitted as trace events with these sentinels.
 * old_event = marker type, new_event = marker type, duration_ns = 0.
 * Inserted into the per-PID stream between wait events. */
#define PGWT_MARKER_EXEC_START   0xFFFFFFF0U  /* query execution start (PortalRun entry) */
#define PGWT_MARKER_EXEC_END     0xFFFFFFF1U  /* query execution end (PortalRun return) */
#define PGWT_MARKER_PLAN_START   0xFFFFFFF2U  /* query planning start (pg_plan_query entry) */
#define PGWT_MARKER_PLAN_END     0xFFFFFFF3U  /* query planning end (pg_plan_query return) */

#define PGWT_IS_MARKER(e) ((e) >= 0xFFFFFFF0U && (e) <= 0xFFFFFFF3U)

/* ── Wait Event Class IDs ─────────────────────────────────── */
#define PG_WAIT_LWLOCK      0x01
#define PG_WAIT_LOCK        0x03
#define PG_WAIT_BUFFERPIN   0x04
#define PG_WAIT_ACTIVITY    0x05
#define PG_WAIT_CLIENT      0x06
#define PG_WAIT_EXTENSION   0x07
#define PG_WAIT_IPC         0x08
#define PG_WAIT_TIMEOUT     0x09
#define PG_WAIT_IO          0x0A

#define WE_CLASS(x)  (((x) >> 24) & 0xFF)
#define WE_EVENT(x)  ((x) & 0x00FFFFFF)

/* Client:ClientRead — idle between queries (like Oracle's SQL*Net message from client) */
#define PG_WAIT_CLIENT_READ  ((PG_WAIT_CLIENT << 24) | 0x000000)

/* ── BPF Accumulator (lightweight mode — no ringbuf) ───── */
#define ACCUM_MAP_MAX_ENTRIES 1024

struct pgwt_accum_val {
    u64 total_ns;   /* total time in this wait event */
    u64 count;      /* number of transitions into this event */
};

#endif /* PG_WAIT_TRACER_H */
