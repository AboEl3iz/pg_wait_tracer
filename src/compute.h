/* compute.h — Server-side compute functions for pgwt-server */
#ifndef PGWT_COMPUTE_H
#define PGWT_COMPUTE_H

#include "pg_wait_tracer.h"
#include <stdint.h>

/* ── Wait classes (same order as Rust WaitClass enum) ──────── */

#define PGWT_NUM_CLASSES 11

#define PGWT_CLASS_CPU       0
#define PGWT_CLASS_IO        1
#define PGWT_CLASS_LOCK      2
#define PGWT_CLASS_LWLOCK    3
#define PGWT_CLASS_IPC       4
#define PGWT_CLASS_CLIENT    5
#define PGWT_CLASS_TIMEOUT   6
#define PGWT_CLASS_BUFFERPIN 7
#define PGWT_CLASS_ACTIVITY  8
#define PGWT_CLASS_EXTENSION 9
#define PGWT_CLASS_UNKNOWN   10

static const char *pgwt_class_names[PGWT_NUM_CLASSES] = {
    "cpu", "io", "lock", "lwlock", "ipc", "client",
    "timeout", "bufferpin", "activity", "extension", "unknown"
};

/* Display names matching PostgreSQL conventions */
static const char *pgwt_class_display[PGWT_NUM_CLASSES] = {
    "CPU", "IO", "Lock", "LWLock", "IPC", "Client",
    "Timeout", "BufferPin", "Activity", "Extension", "Unknown"
};

/* Map wait_event_info → class index (0–10) */
int pgwt_wait_class_index(uint32_t event_id);

/* ── Filters ──────────────────────────────────────────────── */

struct pgwt_filter {
    char     class_name[32];   /* "" = no filter */
    uint32_t event_id;         /* 0 = no filter */
    uint32_t pid;              /* 0 = no filter */
    uint64_t query_id;         /* 0 = no filter */
};

int pgwt_filter_matches(const struct pgwt_filter *f,
                        const struct pgwt_trace_event *ev);

/* ── AAS Buckets ──────────────────────────────────────────── */

struct pgwt_aas_bucket {
    uint64_t start_ns;
    double   class_aas[PGWT_NUM_CLASSES];
};

#define AAS_MAX_EVENT_SERIES 16

struct pgwt_aas_event_series {
    uint32_t event_id;
    char     name[64];     /* "IO:DataFileRead" */
    double   total_aas;    /* sum across all buckets, for sorting */
};

struct pgwt_aas_result {
    struct pgwt_aas_bucket *buckets;   /* malloc'd, caller frees */
    int      num_buckets;
    uint64_t bucket_ns;
    double   max_aas;

    /* Per-event breakdown (populated when detail_events=1) */
    int      num_event_series;   /* 0 = class mode (default) */
    struct pgwt_aas_event_series event_series[AAS_MAX_EVENT_SERIES];
    double  *event_aas;          /* malloc'd [num_buckets * num_event_series] */
};

void pgwt_compute_aas(const struct pgwt_trace_event *events, int count,
                      const struct pgwt_filter *f,
                      uint64_t from_ns, uint64_t to_ns, int num_buckets,
                      int detail_events, int max_series,
                      struct pgwt_aas_result *out);

/* ── Time Model ───────────────────────────────────────────── */

struct pgwt_tm_row {
    char   name[64];
    double time_ms;
    double pct_db_time;
    double aas;
    int    indent;       /* 0=top, 1=class, 2=sub-event */
};

struct pgwt_tm_result {
    struct pgwt_tm_row *rows;   /* malloc'd, caller frees */
    int    num_rows;
    double db_time_ms;
    double idle_time_ms;
    double aas;
    double wall_ms;
};

void pgwt_compute_time_model(const struct pgwt_trace_event *events, int count,
                             const struct pgwt_filter *f, double wall_ms,
                             struct pgwt_tm_result *out);

/* ── Top Events ───────────────────────────────────────────── */

struct pgwt_event_row {
    uint32_t event_id;
    char     name[64];
    uint64_t count;
    double   total_ms;
    double   avg_us;
    double   max_us;
    double   pct_db;
    double   aas;
};

struct pgwt_events_result {
    struct pgwt_event_row *rows;   /* malloc'd, caller frees */
    int    num_rows;
    double db_time_ms;
};

void pgwt_compute_top_events(const struct pgwt_trace_event *events, int count,
                             const struct pgwt_filter *f, double wall_ms,
                             struct pgwt_events_result *out);

/* ── Top Sessions ─────────────────────────────────────────── */

struct pgwt_session_row {
    uint32_t pid;
    double   db_time_ms;
    double   cpu_pct;
    double   wait_pct;
    char     top_wait[64];
    uint32_t top_wait_id;
};

struct pgwt_sessions_result {
    struct pgwt_session_row *rows;   /* malloc'd, caller frees */
    int num_rows;
};

void pgwt_compute_top_sessions(const struct pgwt_trace_event *events, int count,
                               const struct pgwt_filter *f, double wall_ms,
                               struct pgwt_sessions_result *out);

/* ── Top Queries ──────────────────────────────────────────── */

struct pgwt_query_row {
    uint64_t query_id;
    uint64_t count;
    double   total_ms;
    double   avg_us;
    double   pct_db;
    char     top_wait[64];
    uint32_t top_wait_id;
    double   class_ms[PGWT_NUM_CLASSES];  /* per-class time breakdown */
    /* Per-event breakdown (populated from raw events path) */
    int      num_events;
    uint32_t event_ids[16];
    double   event_ms[16];
};

struct pgwt_queries_result {
    struct pgwt_query_row *rows;   /* malloc'd, caller frees */
    int    num_rows;
    double db_time_ms;
};

void pgwt_compute_top_queries(const struct pgwt_trace_event *events, int count,
                              const struct pgwt_filter *f, double wall_ms,
                              struct pgwt_queries_result *out);

/* ── Heatmap (latency distribution over time) ─────────────── */

struct pgwt_heatmap_result {
    uint64_t *grid;        /* malloc'd [num_buckets * HISTOGRAM_BUCKETS] */
    int      num_buckets;  /* time buckets */
    uint64_t bucket_ns;    /* time bucket width */
    uint64_t *times;       /* malloc'd [num_buckets] start timestamps */
    uint64_t max_count;    /* max cell value (for color scaling) */
    uint64_t total_events; /* total events in heatmap */
};

void pgwt_compute_heatmap(const struct pgwt_trace_event *events, int count,
                          const struct pgwt_filter *f,
                          uint64_t from_ns, uint64_t to_ns, int num_buckets,
                          struct pgwt_heatmap_result *out);

/* ── Summary-based compute (streaming visitor, 1-second records) ── */

void pgwt_compute_aas_from_summaries(
    const char *trace_dir, uint64_t from_ns, uint64_t to_ns,
    const struct pgwt_filter *f, int num_buckets,
    struct pgwt_aas_result *out);

void pgwt_compute_time_model_from_summaries(
    const char *trace_dir, uint64_t from_ns, uint64_t to_ns,
    const struct pgwt_filter *f, double wall_ms,
    struct pgwt_tm_result *out);

void pgwt_compute_top_events_from_summaries(
    const char *trace_dir, uint64_t from_ns, uint64_t to_ns,
    const struct pgwt_filter *f, double wall_ms,
    struct pgwt_events_result *out);

void pgwt_compute_top_sessions_from_summaries(
    const char *trace_dir, uint64_t from_ns, uint64_t to_ns,
    const struct pgwt_filter *f, double wall_ms,
    struct pgwt_sessions_result *out);

void pgwt_compute_top_queries_from_summaries(
    const char *trace_dir, uint64_t from_ns, uint64_t to_ns,
    const struct pgwt_filter *f, double wall_ms,
    struct pgwt_queries_result *out);

void pgwt_compute_heatmap_from_summaries(
    const char *trace_dir, uint64_t from_ns, uint64_t to_ns,
    const struct pgwt_filter *f, int num_buckets,
    struct pgwt_heatmap_result *out);

#endif /* PGWT_COMPUTE_H */
