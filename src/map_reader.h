/* map_reader.h — BPF map reading, per-CPU summing, accumulation */
#ifndef PGWT_MAP_READER_H
#define PGWT_MAP_READER_H

#include "pg_wait_tracer.h"

#include <stdint.h>
#include <stdbool.h>

/* Per-event stats (userspace accumulator) */
struct pgwt_event_stats {
    uint32_t wait_event;
    uint64_t count;
    uint64_t total_ns;
    uint64_t min_ns;
    uint64_t max_ns;
    uint32_t histogram[HISTOGRAM_BUCKETS];
};

/* Per-PID accumulator */
struct pgwt_pid_accum {
    uint32_t pid;
    bool     active;          /* has data */
    int      num_events;
    uint64_t db_time_ns;      /* total non-idle time */
    uint64_t cpu_time_ns;     /* event=0 time */
    uint64_t wait_time_ns;    /* all event!=0 time */
    struct pgwt_event_stats events[MAX_EVENTS_PER_PID];
};

/* Time model (system-wide) */
struct pgwt_time_model {
    uint64_t db_time_ns;
    uint64_t cpu_time_ns;
    uint64_t io_time_ns;
    uint64_t lwlock_time_ns;
    uint64_t lock_time_ns;
    uint64_t bufferpin_time_ns;
    uint64_t client_time_ns;
    uint64_t ipc_time_ns;
    uint64_t timeout_time_ns;
    uint64_t extension_time_ns;
    uint64_t activity_time_ns;
};

/* Per-(query_id, event) stats */
struct pgwt_query_event_stats {
    uint64_t query_id;
    uint32_t wait_event;
    uint64_t count;
    uint64_t total_ns;
    uint64_t min_ns;
    uint64_t max_ns;
};
#define MAX_QUERY_EVENTS  4096

/* Full accumulator state */
struct pgwt_accumulator {
    /* Current snapshot */
    struct pgwt_pid_accum pids[MAX_BACKENDS];
    int num_pids;
    struct pgwt_time_model tm;

    /* Previous snapshot (for delta) */
    struct pgwt_time_model prev_tm;

    /* System-wide event aggregation */
    struct pgwt_event_stats system_events[4096];
    int num_system_events;

    /* Query-level event aggregation */
    struct pgwt_query_event_stats query_events[MAX_QUERY_EVENTS];
    int num_query_events;
};

/* Forward */
struct pgwt_daemon;

/* Initialize accumulator. */
void pgwt_accum_init(struct pgwt_accumulator *acc);

/* Read all BPF map entries and update accumulator. */
int pgwt_read_maps(struct pgwt_daemon *d);

/* Find per-PID accumulator. Returns NULL if not found. */
struct pgwt_pid_accum *pgwt_find_pid_accum(struct pgwt_accumulator *acc, uint32_t pid);

/* Find system-wide event stats. Returns NULL if not found. */
struct pgwt_event_stats *pgwt_find_system_event(struct pgwt_accumulator *acc,
                                                 uint32_t wait_event);

#endif /* PGWT_MAP_READER_H */
