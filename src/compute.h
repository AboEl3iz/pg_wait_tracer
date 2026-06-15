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

/* ── Fidelity (trace format v2 — D3) ───────────────────────── */

/* The fidelity of the data covering a queried time window.
 *
 *   EXACT   — every covered range has TRANSITIONS (full-fidelity) blocks.
 *   SAMPLED — every covered range has only SAMPLES blocks (point
 *             observations); estimators use ASH math (each sample worth
 *             one sample_period_ns).
 *   MIXED   — the window spans both. Per the exact-wins merge rule,
 *             transition data is authoritative inside transition-covered
 *             ranges; samples only contribute outside them.
 *   NONE    — no data at all in the window.
 *
 * The integer ordering is meaningful for merging window fidelities
 * (see pgwt_fidelity_merge): NONE < EXACT < SAMPLED < MIXED. */
enum pgwt_fidelity {
    PGWT_FIDELITY_NONE    = 0,
    PGWT_FIDELITY_EXACT   = 1,
    PGWT_FIDELITY_SAMPLED = 2,
    PGWT_FIDELITY_MIXED   = 3,
};

/* The fidelity a view requires to produce a meaningful result.
 *   SAMPLED-capable views (time_model, system/session/query events, AAS,
 *     active/sessions) work from samples via ASH math.
 *   EXACT-required views (histogram/heatmap, transitions, fingerprints,
 *     lock chains, interference, variants, concurrency) need real
 *     intervals/order and report "unavailable" over a sampled-only window. */
enum pgwt_required_fidelity {
    PGWT_REQ_SAMPLED = 0,   /* samples are enough */
    PGWT_REQ_EXACT   = 1,   /* needs full-fidelity transitions */
};

/* Derive the overall fidelity of a loaded window from what block types
 * actually contributed records after the exact-wins merge. */
static inline enum pgwt_fidelity
pgwt_fidelity_of(int has_transitions, int has_samples)
{
    if (has_transitions && has_samples) return PGWT_FIDELITY_MIXED;
    if (has_transitions)                return PGWT_FIDELITY_EXACT;
    if (has_samples)                    return PGWT_FIDELITY_SAMPLED;
    return PGWT_FIDELITY_NONE;
}

/* Lowercase token used in JSON responses ("exact"|"sampled"|"mixed"|"none"). */
static inline const char *pgwt_fidelity_str(enum pgwt_fidelity fid)
{
    switch (fid) {
    case PGWT_FIDELITY_EXACT:   return "exact";
    case PGWT_FIDELITY_SAMPLED: return "sampled";
    case PGWT_FIDELITY_MIXED:   return "mixed";
    default:                    return "none";
    }
}

/* True when a view requiring `req` fidelity cannot be produced from a
 * window of the given `fid`. EXACT-required views are unavailable over a
 * window that contributed no transition data (SAMPLED or NONE). */
static inline int
pgwt_fidelity_unavailable(enum pgwt_required_fidelity req, enum pgwt_fidelity fid)
{
    if (req != PGWT_REQ_EXACT)
        return 0;
    return (fid != PGWT_FIDELITY_EXACT && fid != PGWT_FIDELITY_MIXED);
}

/* Canonical message for an EXACT-required view over a sampled-only window. */
#define PGWT_UNAVAILABLE_MSG "requires full-fidelity data"

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
    double   p50_us;
    double   p95_us;
    double   p99_us;
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

/* ── Transitions ──────────────────────────────────────────── */

struct pgwt_transition_row {
    uint32_t from_event;
    uint32_t to_event;
    char     from_name[64];
    char     to_name[64];
    uint64_t count;
    double   total_ns;       /* total time spent in from_event before this transition */
};

struct pgwt_transitions_result {
    struct pgwt_transition_row *rows;  /* malloc'd, caller frees */
    int    num_rows;
    uint64_t total_transitions;
};

/* Compute top state transitions (old_event → new_event).
 * Returns pairs sorted by count descending, limited to max_rows. */
void pgwt_compute_transitions(const struct pgwt_trace_event *events, int count,
                               const struct pgwt_filter *f, int max_rows,
                               struct pgwt_transitions_result *out);

/* ── Fingerprint ─────────────────────────────────────────── */

struct pgwt_fingerprint_row {
    uint64_t query_id;
    double   class_pct[PGWT_NUM_CLASSES];  /* % of time per wait class */
    uint64_t total_transitions;
    uint32_t top_from;       /* most frequent transition source */
    uint32_t top_to;         /* most frequent transition target */
    char     signature[128]; /* compact text fingerprint */
};

struct pgwt_fingerprint_result {
    struct pgwt_fingerprint_row *rows;  /* malloc'd, caller frees */
    int    num_rows;
};

/* Compute per-query wait fingerprints: class distribution + top transition. */
void pgwt_compute_fingerprints(const struct pgwt_trace_event *events, int count,
                                const struct pgwt_filter *f,
                                struct pgwt_fingerprint_result *out);

/* ── Concurrency / Burst Detection ────────────────────────── */

struct pgwt_burst {
    uint64_t timestamp_ns;    /* when the burst started */
    uint32_t event_id;        /* which wait event */
    char     event_name[64];
    int      num_sessions;    /* how many sessions entered simultaneously */
    uint32_t pids[64];        /* affected PIDs (up to 64) */
    int      num_pids;
};

struct pgwt_concurrency_result {
    /* Per-bucket peak concurrency (same layout as AAS buckets) */
    int     *peak_sessions;   /* malloc'd [num_buckets] — max simultaneous waiters */
    uint32_t *peak_event;     /* malloc'd [num_buckets] — event with max concurrency */
    int      num_buckets;
    uint64_t bucket_ns;

    /* Detected bursts (sorted by num_sessions descending) */
    struct pgwt_burst *bursts;  /* malloc'd, caller frees */
    int    num_bursts;
};

/* Detect concurrency peaks and burst events.
 * burst_window_ns: time window for burst detection (e.g. 10ms = 10000000).
 * burst_threshold: minimum sessions to count as burst (e.g. 4). */
void pgwt_compute_concurrency(const struct pgwt_trace_event *events, int count,
                               const struct pgwt_filter *f,
                               uint64_t from_ns, uint64_t to_ns,
                               int num_buckets,
                               uint64_t burst_window_ns, int burst_threshold,
                               struct pgwt_concurrency_result *out);

/* ── Lock Chains ──────────────────────────────────────────── */

struct pgwt_lock_chain_link {
    uint32_t waiter_pid;
    uint32_t blocker_pid;     /* PID that was on CPU while waiter waited on Lock */
    uint32_t lock_event;      /* Lock:transactionid, Lock:tuple, etc. */
    char     lock_name[64];
    uint64_t wait_ns;         /* how long the waiter waited */
    uint64_t timestamp_ns;    /* when the wait started */
};

struct pgwt_lock_chains_result {
    struct pgwt_lock_chain_link *links;  /* malloc'd, caller frees */
    int    num_links;
};

/* Detect lock chains: find sessions waiting on Lock events and identify
 * which other session was likely the blocker (on CPU during the same interval).
 * Returns waiter→blocker links sorted by wait_ns descending. */
void pgwt_compute_lock_chains(const struct pgwt_trace_event *events, int count,
                               const struct pgwt_filter *f, int max_links,
                               struct pgwt_lock_chains_result *out);

/* ── Interference Scoring ────────────────────────────────── */

struct pgwt_interference_row {
    uint32_t pid_a;
    uint32_t pid_b;
    double   score;        /* 0-1: how much A and B contend on same events */
    uint32_t top_event;    /* most contended event */
    char     top_event_name[64];
    uint64_t overlap_ns;   /* total time both were waiting on same event */
};

struct pgwt_interference_result {
    struct pgwt_interference_row *rows;  /* malloc'd, caller frees */
    int    num_rows;
};

/* Score cross-session interference: find PID pairs that frequently wait
 * on the same event at the same time. High-scoring pairs are "noisy neighbors". */
void pgwt_compute_interference(const struct pgwt_trace_event *events, int count,
                                const struct pgwt_filter *f, int max_rows,
                                struct pgwt_interference_result *out);

/* ── Variants (per-query execution flow patterns) ─────────── */

/* A step in a compressed flow pattern */
struct pgwt_variant_step {
    uint32_t event_id;
    char     name[64];
    int      is_loop;          /* 1 if this step starts a loop */
    int      loop_len;         /* number of events in the loop body */
};

#define PGWT_MAX_VARIANT_STEPS 32

struct pgwt_variant {
    struct pgwt_variant_step steps[PGWT_MAX_VARIANT_STEPS];
    int      num_steps;
    uint64_t hash;             /* hash of compressed pattern */
    int      exec_count;       /* number of executions matching this pattern */
    int      num_query_ids;    /* distinct query_ids */
    uint64_t total_ns;         /* total wall time across all executions */
    uint64_t avg_ns;           /* average execution time */
    uint64_t p95_ns;           /* p95 execution time */
    double   avg_loop_n;       /* average loop iteration count */
    uint64_t top_query_id;     /* most frequent query_id */
    /* Per-step timing (avg duration per step across executions) */
    uint64_t step_avg_ns[PGWT_MAX_VARIANT_STEPS];
};

struct pgwt_variants_result {
    struct pgwt_variant *variants;  /* malloc'd, caller frees */
    int    num_variants;
    int    total_executions;        /* total EXEC_START/END pairs found */
};

/* Phase selectors for variant extraction */
enum pgwt_variant_phase {
    PGWT_PHASE_EXEC = 0,   /* EXEC_START → EXEC_END */
    PGWT_PHASE_PLAN = 1,   /* PLAN_START → PLAN_END */
};

/* Extract per-query flow variants from trace events.
 * phase selects which markers to use as case boundaries.
 * Loop-compresses patterns, groups identical ones, ranks by total time. */
void pgwt_compute_variants(const struct pgwt_trace_event *events, int count,
                            const struct pgwt_filter *f, int max_variants,
                            enum pgwt_variant_phase phase,
                            struct pgwt_variants_result *out);

#endif /* PGWT_COMPUTE_H */
