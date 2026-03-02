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

struct pgwt_aas_result {
    struct pgwt_aas_bucket *buckets;   /* malloc'd, caller frees */
    int      num_buckets;
    uint64_t bucket_ns;
    double   max_aas;
};

void pgwt_compute_aas(const struct pgwt_trace_event *events, int count,
                      const struct pgwt_filter *f,
                      uint64_t from_ns, uint64_t to_ns, int num_buckets,
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
};

struct pgwt_queries_result {
    struct pgwt_query_row *rows;   /* malloc'd, caller frees */
    int    num_rows;
    double db_time_ms;
};

void pgwt_compute_top_queries(const struct pgwt_trace_event *events, int count,
                              const struct pgwt_filter *f, double wall_ms,
                              struct pgwt_queries_result *out);

/* ── Summary-based compute (pre-aggregated 1-second records) ── */

struct pgwt_summary_accum;   /* forward decl, defined in summary_writer.h */

void pgwt_compute_aas_from_summaries(
    const struct pgwt_summary_accum *records, int count,
    const struct pgwt_filter *f,
    uint64_t from_ns, uint64_t to_ns, int num_buckets,
    struct pgwt_aas_result *out);

void pgwt_compute_time_model_from_summaries(
    const struct pgwt_summary_accum *records, int count,
    const struct pgwt_filter *f, double wall_ms,
    struct pgwt_tm_result *out);

void pgwt_compute_top_events_from_summaries(
    const struct pgwt_summary_accum *records, int count,
    const struct pgwt_filter *f, double wall_ms,
    struct pgwt_events_result *out);

void pgwt_compute_top_sessions_from_summaries(
    const struct pgwt_summary_accum *records, int count,
    const struct pgwt_filter *f, double wall_ms,
    struct pgwt_sessions_result *out);

void pgwt_compute_top_queries_from_summaries(
    const struct pgwt_summary_accum *records, int count,
    const struct pgwt_filter *f, double wall_ms,
    struct pgwt_queries_result *out);

#endif /* PGWT_COMPUTE_H */
