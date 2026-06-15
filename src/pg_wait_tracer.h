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

/* ── Version ──────────────────────────────────────────────── */
#define PGWT_VERSION         "0.8.0"

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
    u32 _pad;           /* alignment */
    u64 last_ts;        /* ktime_ns of last transition */
    u64 last_query_id;  /* query_id set by on_report_query_id uprobe */
    u64 be_entry_ptr;   /* cached PgBackendStatus* (avoids 1 probe_read per event) */
    u64 wait_event_addr; /* direct PGPROC->wait_event_info address (saves 1 probe_read) */
};

/* ── Lifecycle Events (ring buffer) ───────────────────────── */

enum pgwt_lifecycle_type {
    PGWT_LIFECYCLE_FORK = 1,
    PGWT_LIFECYCLE_INIT = 2,
    PGWT_LIFECYCLE_EXIT = 3,
    PGWT_LIFECYCLE_QUERY_TEXT = 4,
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
    u32 flags;          /* reader-side annotation (PGWT_EVENT_FLAG_*); 0 on the wire */
    u64 duration_ns;    /* time spent in old_event */
    u64 query_id;       /* active query during old_event */
};

/* flags bits — set by the trace reader, never on the wire/ringbuf.
 * SAMPLE marks a record decoded from a SAMPLES block: it is a point
 * observation (new_event = sampled event; old_event = 0; duration_ns = 0),
 * not a transition interval. A3's compute must treat it as worth one
 * sample_period_ns, not as an interval duration. */
#define PGWT_EVENT_FLAG_SAMPLE  0x1U

#define PGWT_EVENT_EXIT  0xFFFFFFFFU  /* sentinel new_event for process exit */

/* Query lifecycle markers — emitted as trace events with these sentinels.
 * old_event = marker type, new_event = marker type, duration_ns = 0.
 * Inserted into the per-PID stream between wait events. */
#define PGWT_MARKER_EXEC_START   0xFFFFFFF0U  /* query execution start (PortalRun entry) */
#define PGWT_MARKER_EXEC_END     0xFFFFFFF1U  /* query execution end (PortalRun return) */
#define PGWT_MARKER_PLAN_START   0xFFFFFFF2U  /* query planning start (pg_plan_query entry) */
#define PGWT_MARKER_PLAN_END     0xFFFFFFF3U  /* query planning end (pg_plan_query return) */

/* Escalation window markers (A4). Written by the daemon (pid = 0, a value no
 * PG backend uses) when a tiered-mode full-fidelity window opens/closes, so a
 * reader/UI can show WHY exact data exists for a time range. Convention
 * (matches the query markers so duration-consuming views are unaffected):
 *   old_event = new_event = marker type
 *   duration_ns           = 0  (markers never carry a wait duration)
 *   query_id              = PGWT_ESC_PACK(window_seconds, reason): the granted
 *                           window length (START) or elapsed seconds (END) in
 *                           the high bits, the reason code in the low byte.
 * Forward-compatible: a reader that doesn't know these treats them as plain
 * markers (skipped from wait accumulation by PGWT_IS_MARKER). */
#define PGWT_MARKER_ESCALATE_START 0xFFFFFFF4U  /* full-fidelity window opened */
#define PGWT_MARKER_ESCALATE_END   0xFFFFFFF5U  /* full-fidelity window closed */

#define PGWT_IS_MARKER(e) ((e) >= 0xFFFFFFF0U && (e) <= 0xFFFFFFF5U)

/* Pack/unpack the escalation marker payload carried in query_id. */
#define PGWT_ESC_PACK(secs, reason) \
    (((uint64_t)(secs) << 8) | ((uint64_t)(reason) & 0xFFU))
#define PGWT_ESC_UNPACK_SECS(qid)   ((uint64_t)(qid) >> 8)
#define PGWT_ESC_UNPACK_REASON(qid) ((unsigned)((qid) & 0xFFU))

/* Reason a tiered escalation window opened/closed, recorded in the marker's
 * query_id slot. Manual is the only A4 producer; anomaly triggers (A5) and
 * budget/expiry close reasons are reserved here so the trace schema is stable
 * across phases. */
enum pgwt_escalation_reason {
    PGWT_ESC_REASON_MANUAL   = 0,  /* operator requested via control socket */
    PGWT_ESC_REASON_ANOMALY  = 1,  /* anomaly trigger (A5) */
    PGWT_ESC_REASON_EXPIRED  = 2,  /* window reached its deadline (close) */
    PGWT_ESC_REASON_REQUEST  = 3,  /* explicit deescalate request (close) */
    PGWT_ESC_REASON_SHUTDOWN = 4,  /* daemon stopping (close) */
};

/* ── Query Text Event (lifecycle ringbuf) ─────────────────── */
#define PGWT_QUERY_TEXT_LEN 256

struct pgwt_query_text_event {
    u32 type;       /* PGWT_LIFECYCLE_QUERY_TEXT */
    u32 pid;
    u64 query_id;
    char text[PGWT_QUERY_TEXT_LEN];
};

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
/* Compose a wait_event_info from class byte + event id. */
#define WEI(cls, id) (((uint32_t)(cls) << 24) | (uint32_t)(id))

/* Client:ClientRead — idle between queries (like Oracle's SQL*Net message from client) */
#define PG_WAIT_CLIENT_READ  ((PG_WAIT_CLIENT << 24) | 0x000000)

/* ── BPF Accumulator (lightweight mode — no ringbuf) ───── */
#define ACCUM_MAP_MAX_ENTRIES 1024

struct pgwt_accum_val {
    u64 total_ns;   /* total time in this wait event */
    u64 count;      /* number of transitions into this event */
};

#endif /* PG_WAIT_TRACER_H */
