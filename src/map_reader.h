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
    uint64_t histogram[HISTOGRAM_BUCKETS];
};

/* Per-PID accumulator */
struct pgwt_pid_accum {
    uint32_t pid;
    bool     active;          /* has data */
    int      num_events;
    uint64_t db_time_ns;      /* total non-idle time */
    uint64_t cpu_time_ns;     /* event=0 time */
    uint64_t wait_time_ns;    /* all event!=0 time */
    uint32_t current_event;   /* current wait event from state_map */
    uint64_t current_wait_ns; /* time in current state */
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

/* Read state_map for open intervals and current state (active view). */
void pgwt_read_state_map(struct pgwt_daemon *d);

/* Find per-PID accumulator. Returns NULL if not found. */
struct pgwt_pid_accum *pgwt_find_pid_accum(struct pgwt_accumulator *acc, uint32_t pid);

/* Find system-wide event stats. Returns NULL if not found. */
struct pgwt_event_stats *pgwt_find_system_event(struct pgwt_accumulator *acc,
                                                 uint32_t wait_event);

/* Get or create helpers — shared between map_reader and event_stream */
struct pgwt_pid_accum *pgwt_get_or_create_pid(struct pgwt_accumulator *acc, uint32_t pid);
struct pgwt_event_stats *pgwt_get_or_create_event(struct pgwt_pid_accum *pa, uint32_t we);
struct pgwt_event_stats *pgwt_get_or_create_system_event(struct pgwt_accumulator *acc,
                                                          uint32_t we);
struct pgwt_query_event_stats *pgwt_get_or_create_query_event(
    struct pgwt_accumulator *acc, uint64_t query_id, uint32_t we);

/* Update time model by wait event class. */
void pgwt_update_time_model(struct pgwt_time_model *tm, uint32_t event,
                             uint64_t duration_ns);

/* Log2 histogram bucket for a duration in nanoseconds. */
uint32_t pgwt_duration_to_bucket(uint64_t ns);

#endif /* PGWT_MAP_READER_H */
