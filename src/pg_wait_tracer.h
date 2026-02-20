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
};

/* Key for wait_stats map */
struct pgwt_agg_key {
    u32 pid;
    u32 wait_event;     /* full wait_event_info; 0 = CPU */
};

/* Value for wait_stats map */
struct pgwt_agg_value {
    u64 count;
    u64 total_ns;
    u64 min_ns;
    u64 max_ns;
    u32 histogram[HISTOGRAM_BUCKETS];
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

#endif /* PG_WAIT_TRACER_H */
