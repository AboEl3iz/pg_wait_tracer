/* compute.c — Server-side compute functions for pgwt-server
 *
 * Direct port of client/src/compute.rs. Works on raw pgwt_trace_event arrays.
 * All result structs use malloc'd arrays — caller frees with free(result->rows).
 */
#include "compute.h"
#include "summary_writer.h"
#include "wait_event.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Wait class mapping ───────────────────────────────────── */

int pgwt_wait_class_index(uint32_t event_id)
{
    if (event_id == 0)
        return PGWT_CLASS_CPU;

    uint8_t cls = (event_id >> 24) & 0xFF;
    switch (cls) {
    case 0x0A: return PGWT_CLASS_IO;
    case 0x03: return PGWT_CLASS_LOCK;
    case 0x01: return PGWT_CLASS_LWLOCK;
    case 0x08: return PGWT_CLASS_IPC;
    case 0x06: return PGWT_CLASS_CLIENT;
    case 0x09: return PGWT_CLASS_TIMEOUT;
    case 0x04: return PGWT_CLASS_BUFFERPIN;
    case 0x05: return PGWT_CLASS_ACTIVITY;
    case 0x07: return PGWT_CLASS_EXTENSION;
    default:   return PGWT_CLASS_UNKNOWN;
    }
}

/* ── Filter ───────────────────────────────────────────────── */

int pgwt_filter_matches(const struct pgwt_filter *f,
                        const struct pgwt_trace_event *ev)
{
    if (f->class_name[0] != '\0') {
        const char *cls = pgwt_class_name(ev->old_event);
        if (strcasecmp(cls, f->class_name) != 0)
            return 0;
    }
    if (f->event_id != 0 && ev->old_event != f->event_id)
        return 0;
    if (f->pid != 0 && ev->pid != f->pid)
        return 0;
    if (f->query_id != 0 && ev->query_id != f->query_id)
        return 0;
    return 1;
}

/* ── AAS Buckets ──────────────────────────────────────────── */

void pgwt_compute_aas(const struct pgwt_trace_event *events, int count,
                      const struct pgwt_filter *f,
                      uint64_t from_ns, uint64_t to_ns, int num_buckets,
                      struct pgwt_aas_result *out)
{
    memset(out, 0, sizeof(*out));

    uint64_t range_ns = to_ns - from_ns;
    if (range_ns == 0 || num_buckets <= 0) {
        out->bucket_ns = 1000000000ULL;
        return;
    }

    /* bucket_ns = max(1s, ceil(range / num_buckets)) */
    uint64_t bucket_ns = (range_ns + (uint64_t)num_buckets - 1) / (uint64_t)num_buckets;
    if (bucket_ns < 1000000000ULL)
        bucket_ns = 1000000000ULL;

    int actual_buckets = (int)((range_ns + bucket_ns - 1) / bucket_ns);
    struct pgwt_aas_bucket *buckets = calloc(actual_buckets, sizeof(*buckets));
    if (!buckets)
        return;

    for (int i = 0; i < actual_buckets; i++)
        buckets[i].start_ns = from_ns + (uint64_t)i * bucket_ns;

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (!pgwt_filter_matches(f, ev) || pgwt_is_idle_event(ev->old_event))
            continue;

        uint64_t ev_start = ev->timestamp_ns;
        uint64_t ev_end   = ev->timestamp_ns + ev->duration_ns;

        if (ev_end <= from_ns || ev_start >= to_ns)
            continue;

        int class_idx = pgwt_wait_class_index(ev->old_event);

        int first_b = (ev_start <= from_ns) ? 0
                     : (int)((ev_start - from_ns) / bucket_ns);
        int last_b  = (ev_end >= to_ns) ? actual_buckets - 1
                     : (int)(((ev_end - from_ns) - 1) / bucket_ns);
        if (last_b >= actual_buckets)
            last_b = actual_buckets - 1;

        for (int b = first_b; b <= last_b; b++) {
            uint64_t b_start = from_ns + (uint64_t)b * bucket_ns;
            uint64_t b_end   = b_start + bucket_ns;
            uint64_t o_start = ev_start > b_start ? ev_start : b_start;
            uint64_t o_end   = ev_end < b_end ? ev_end : b_end;
            if (o_end > o_start)
                buckets[b].class_aas[class_idx] += (double)(o_end - o_start);
        }
    }

    /* Convert accumulated ns → AAS */
    double max_aas = 0.0;
    for (int i = 0; i < actual_buckets; i++) {
        double total = 0.0;
        for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
            buckets[i].class_aas[c] /= (double)bucket_ns;
            total += buckets[i].class_aas[c];
        }
        if (total > max_aas)
            max_aas = total;
    }

    out->buckets     = buckets;
    out->num_buckets = actual_buckets;
    out->bucket_ns   = bucket_ns;
    out->max_aas     = max_aas;
}

/* ── Time Model ───────────────────────────────────────────── */

/* Internal accumulator for class/event totals */
struct class_accum {
    char     name[32];
    double   total_ns;
};

struct event_accum {
    char     class_name[32];
    uint32_t event_id;
    double   total_ns;
};

static int cmp_class_desc(const void *a, const void *b)
{
    double da = ((const struct class_accum *)a)->total_ns;
    double db = ((const struct class_accum *)b)->total_ns;
    return (db > da) - (db < da);
}

static int cmp_event_desc(const void *a, const void *b)
{
    double da = ((const struct event_accum *)a)->total_ns;
    double db = ((const struct event_accum *)b)->total_ns;
    return (db > da) - (db < da);
}

void pgwt_compute_time_model(const struct pgwt_trace_event *events, int count,
                             const struct pgwt_filter *f, double wall_ms,
                             struct pgwt_tm_result *out)
{
    memset(out, 0, sizeof(*out));
    out->wall_ms = wall_ms;

    /* Phase 1: accumulate per-class and per-(class, event) totals */
    struct class_accum classes[PGWT_NUM_CLASSES];
    memset(classes, 0, sizeof(classes));
    for (int i = 0; i < PGWT_NUM_CLASSES; i++)
        snprintf(classes[i].name, sizeof(classes[i].name), "%s",
                 pgwt_class_names[i]);

    /* Use a flat array for per-event accum (max 4096 distinct events) */
    #define MAX_DISTINCT_EVENTS 4096
    struct event_accum *ev_accum = calloc(MAX_DISTINCT_EVENTS, sizeof(*ev_accum));
    int num_ev_accum = 0;

    double db_time_ns  = 0.0;
    double idle_time_ns = 0.0;

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (!pgwt_filter_matches(f, ev))
            continue;

        double dur_ns = (double)ev->duration_ns;

        if (pgwt_is_idle_event(ev->old_event)) {
            idle_time_ns += dur_ns;
            continue;
        }

        db_time_ns += dur_ns;
        int cls_idx = pgwt_wait_class_index(ev->old_event);
        classes[cls_idx].total_ns += dur_ns;

        /* Find or insert event in flat array */
        const char *cls_name = pgwt_class_names[cls_idx];
        int found = -1;
        for (int j = 0; j < num_ev_accum; j++) {
            if (ev_accum[j].event_id == ev->old_event) {
                found = j;
                break;
            }
        }
        if (found >= 0) {
            ev_accum[found].total_ns += dur_ns;
        } else if (num_ev_accum < MAX_DISTINCT_EVENTS) {
            snprintf(ev_accum[num_ev_accum].class_name,
                     sizeof(ev_accum[num_ev_accum].class_name), "%s", cls_name);
            ev_accum[num_ev_accum].event_id = ev->old_event;
            ev_accum[num_ev_accum].total_ns = dur_ns;
            num_ev_accum++;
        }
    }

    double db_time_ms  = db_time_ns / 1e6;
    double idle_ms     = idle_time_ns / 1e6;

    /* Phase 2: build result rows */
    /* Max rows: 1 (DB Time) + NUM_CLASSES * (1 class + 3 sub-events) = ~45 */
    int max_rows = 1 + PGWT_NUM_CLASSES * 4;
    struct pgwt_tm_row *rows = calloc(max_rows, sizeof(*rows));
    int nr = 0;

    /* DB Time row */
    snprintf(rows[nr].name, sizeof(rows[nr].name), "DB Time");
    rows[nr].time_ms     = db_time_ms;
    rows[nr].pct_db_time = 100.0;
    rows[nr].aas         = wall_ms > 0 ? db_time_ms / wall_ms : 0;
    rows[nr].indent      = 0;
    nr++;

    /* Sort classes by total descending */
    qsort(classes, PGWT_NUM_CLASSES, sizeof(classes[0]), cmp_class_desc);

    /* Sort event accum for sub-event lookup */
    qsort(ev_accum, num_ev_accum, sizeof(ev_accum[0]), cmp_event_desc);

    for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
        if (classes[c].total_ns <= 0)
            continue;

        double cls_ms = classes[c].total_ns / 1e6;
        double pct = db_time_ms > 0 ? cls_ms / db_time_ms * 100.0 : 0;

        /* Class display name from lookup table */
        const char *display = classes[c].name;
        for (int k = 0; k < PGWT_NUM_CLASSES; k++) {
            if (strcasecmp(classes[c].name, pgwt_class_names[k]) == 0) {
                display = pgwt_class_display[k];
                break;
            }
        }
        if (strcasecmp(classes[c].name, "cpu") == 0)
            snprintf(rows[nr].name, sizeof(rows[nr].name), "CPU*");
        else
            snprintf(rows[nr].name, sizeof(rows[nr].name), "%.31s", display);
        rows[nr].time_ms     = cls_ms;
        rows[nr].pct_db_time = pct;
        rows[nr].aas         = wall_ms > 0 ? cls_ms / wall_ms : 0;
        rows[nr].indent      = 1;
        nr++;

        /* Top 3 sub-events for this class (skip CPU — no meaningful sub-events) */
        if (strcasecmp(classes[c].name, "cpu") == 0)
            continue;

        int sub_count = 0;
        for (int j = 0; j < num_ev_accum && sub_count < 3; j++) {
            if (strcasecmp(ev_accum[j].class_name, classes[c].name) != 0)
                continue;

            double sub_ms  = ev_accum[j].total_ns / 1e6;
            double sub_pct = db_time_ms > 0 ? sub_ms / db_time_ms * 100.0 : 0;
            if (sub_pct < 0.1)
                break;

            char buf[64];
            pgwt_event_full_name(ev_accum[j].event_id, buf, sizeof(buf));
            snprintf(rows[nr].name, sizeof(rows[nr].name), "%s", buf);
            rows[nr].time_ms     = sub_ms;
            rows[nr].pct_db_time = sub_pct;
            rows[nr].aas         = wall_ms > 0 ? sub_ms / wall_ms : 0;
            rows[nr].indent      = 2;
            nr++;
            sub_count++;
        }
    }

    free(ev_accum);

    out->rows        = rows;
    out->num_rows    = nr;
    out->db_time_ms  = db_time_ms;
    out->idle_time_ms = idle_ms;
    out->aas         = wall_ms > 0 ? db_time_ms / wall_ms : 0;
}

/* ── Top Events ───────────────────────────────────────────── */

struct top_event_accum {
    uint32_t event_id;
    uint64_t count;
    uint64_t total_ns;
    uint64_t max_ns;
};

#define EVENT_HT_SIZE 1024
#define EVENT_HT_MASK (EVENT_HT_SIZE - 1)

static int cmp_event_row_desc(const void *a, const void *b)
{
    double da = ((const struct pgwt_event_row *)a)->total_ms;
    double db = ((const struct pgwt_event_row *)b)->total_ms;
    return (db > da) - (db < da);
}

void pgwt_compute_top_events(const struct pgwt_trace_event *events, int count,
                             const struct pgwt_filter *f, double wall_ms,
                             struct pgwt_events_result *out)
{
    memset(out, 0, sizeof(*out));

    /* Hash table: open addressing, linear probe */
    struct top_event_accum *ht = calloc(EVENT_HT_SIZE, sizeof(*ht));
    int num_entries = 0;
    uint64_t db_time_ns = 0;

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (!pgwt_filter_matches(f, ev) || pgwt_is_idle_event(ev->old_event))
            continue;

        db_time_ns += ev->duration_ns;

        /* Hash by event_id */
        uint32_t h = ev->old_event & EVENT_HT_MASK;
        while (ht[h].count > 0 && ht[h].event_id != ev->old_event)
            h = (h + 1) & EVENT_HT_MASK;

        if (ht[h].count == 0) {
            ht[h].event_id = ev->old_event;
            num_entries++;
        }
        ht[h].count++;
        ht[h].total_ns += ev->duration_ns;
        if (ev->duration_ns > ht[h].max_ns)
            ht[h].max_ns = ev->duration_ns;
    }

    double db_time_ms = (double)db_time_ns / 1e6;

    /* Collect into result rows */
    struct pgwt_event_row *rows = calloc(num_entries, sizeof(*rows));
    int nr = 0;

    for (int i = 0; i < EVENT_HT_SIZE; i++) {
        if (ht[i].count == 0)
            continue;

        struct pgwt_event_row *r = &rows[nr];
        r->event_id = ht[i].event_id;
        r->count    = ht[i].count;
        r->total_ms = (double)ht[i].total_ns / 1e6;
        r->avg_us   = ht[i].count > 0
                     ? (double)ht[i].total_ns / (double)ht[i].count / 1000.0
                     : 0;
        r->max_us   = (double)ht[i].max_ns / 1000.0;
        r->pct_db   = db_time_ms > 0 ? r->total_ms / db_time_ms * 100.0 : 0;
        r->aas      = wall_ms > 0 ? r->total_ms / wall_ms : 0;

        if (ht[i].event_id == 0)
            snprintf(r->name, sizeof(r->name), "CPU*");
        else
            pgwt_event_full_name(ht[i].event_id, r->name, sizeof(r->name));
        nr++;
    }

    free(ht);

    qsort(rows, nr, sizeof(rows[0]), cmp_event_row_desc);

    out->rows        = rows;
    out->num_rows    = nr;
    out->db_time_ms  = db_time_ms;
}

/* ── Top Sessions ─────────────────────────────────────────── */

struct session_accum {
    uint32_t pid;
    uint64_t total_ns;
    uint64_t cpu_ns;
    /* Top wait tracking: simple linear array (max 256 per PID) */
    uint32_t wait_ids[MAX_EVENTS_PER_PID];
    uint64_t wait_ns[MAX_EVENTS_PER_PID];
    int      num_waits;
};

#define SESSION_HT_SIZE MAX_BACKENDS

static int cmp_session_row_desc(const void *a, const void *b)
{
    double da = ((const struct pgwt_session_row *)a)->db_time_ms;
    double db = ((const struct pgwt_session_row *)b)->db_time_ms;
    return (db > da) - (db < da);
}

void pgwt_compute_top_sessions(const struct pgwt_trace_event *events, int count,
                               const struct pgwt_filter *f, double wall_ms,
                               struct pgwt_sessions_result *out)
{
    (void)wall_ms;
    memset(out, 0, sizeof(*out));

    /* Hash table keyed by PID, open addressing */
    struct session_accum *ht = calloc(SESSION_HT_SIZE, sizeof(*ht));
    int num_entries = 0;

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (!pgwt_filter_matches(f, ev) || pgwt_is_idle_event(ev->old_event))
            continue;

        uint32_t h = ev->pid % SESSION_HT_SIZE;
        while (ht[h].pid != 0 && ht[h].pid != ev->pid)
            h = (h + 1) % SESSION_HT_SIZE;

        if (ht[h].pid == 0) {
            ht[h].pid = ev->pid;
            num_entries++;
        }
        ht[h].total_ns += ev->duration_ns;

        if (ev->old_event == 0) {
            ht[h].cpu_ns += ev->duration_ns;
        } else {
            /* Track per-wait totals */
            int found = -1;
            for (int j = 0; j < ht[h].num_waits; j++) {
                if (ht[h].wait_ids[j] == ev->old_event) {
                    found = j;
                    break;
                }
            }
            if (found >= 0) {
                ht[h].wait_ns[found] += ev->duration_ns;
            } else if (ht[h].num_waits < MAX_EVENTS_PER_PID) {
                int n = ht[h].num_waits;
                ht[h].wait_ids[n] = ev->old_event;
                ht[h].wait_ns[n]  = ev->duration_ns;
                ht[h].num_waits++;
            }
        }
    }

    /* Collect results */
    struct pgwt_session_row *rows = calloc(num_entries, sizeof(*rows));
    int nr = 0;

    for (int i = 0; i < SESSION_HT_SIZE; i++) {
        if (ht[i].pid == 0)
            continue;

        struct pgwt_session_row *r = &rows[nr];
        r->pid        = ht[i].pid;
        r->db_time_ms = (double)ht[i].total_ns / 1e6;

        double db = (double)ht[i].total_ns;
        r->cpu_pct  = db > 0 ? (double)ht[i].cpu_ns / db * 100.0 : 0;
        r->wait_pct = 100.0 - r->cpu_pct;

        /* Find top wait (exclude CPU=0) */
        uint32_t top_id = 0;
        uint64_t top_ns = 0;
        for (int j = 0; j < ht[i].num_waits; j++) {
            if (ht[i].wait_ns[j] > top_ns) {
                top_ns = ht[i].wait_ns[j];
                top_id = ht[i].wait_ids[j];
            }
        }
        r->top_wait_id = top_id;
        if (top_id == 0)
            snprintf(r->top_wait, sizeof(r->top_wait), "CPU*");
        else
            pgwt_event_full_name(top_id, r->top_wait, sizeof(r->top_wait));

        nr++;
    }

    free(ht);

    qsort(rows, nr, sizeof(rows[0]), cmp_session_row_desc);

    out->rows     = rows;
    out->num_rows = nr;
}

/* ── Top Queries ──────────────────────────────────────────── */

struct query_accum {
    uint64_t query_id;
    uint64_t count;
    uint64_t total_ns;
    uint64_t class_ns[PGWT_NUM_CLASSES]; /* per-class time breakdown */
    /* Top wait tracking */
    uint32_t wait_ids[64];
    uint64_t wait_ns[64];
    int      num_waits;
};

#define QUERY_HT_SIZE 2048
#define QUERY_HT_MASK (QUERY_HT_SIZE - 1)

static int cmp_query_row_desc(const void *a, const void *b)
{
    double da = ((const struct pgwt_query_row *)a)->total_ms;
    double db = ((const struct pgwt_query_row *)b)->total_ms;
    return (db > da) - (db < da);
}

void pgwt_compute_top_queries(const struct pgwt_trace_event *events, int count,
                              const struct pgwt_filter *f, double wall_ms,
                              struct pgwt_queries_result *out)
{
    (void)wall_ms;
    memset(out, 0, sizeof(*out));

    struct query_accum *ht = calloc(QUERY_HT_SIZE, sizeof(*ht));
    int num_entries = 0;
    uint64_t db_time_ns = 0;

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (!pgwt_filter_matches(f, ev) || pgwt_is_idle_event(ev->old_event))
            continue;
        if (ev->query_id == 0)
            continue;

        db_time_ns += ev->duration_ns;

        uint32_t h = (uint32_t)(ev->query_id ^ (ev->query_id >> 32)) & QUERY_HT_MASK;
        while (ht[h].count > 0 && ht[h].query_id != ev->query_id)
            h = (h + 1) & QUERY_HT_MASK;

        if (ht[h].count == 0) {
            ht[h].query_id = ev->query_id;
            num_entries++;
        }
        ht[h].count++;
        ht[h].total_ns += ev->duration_ns;
        ht[h].class_ns[pgwt_wait_class_index(ev->old_event)] += ev->duration_ns;

        /* Track per-wait totals */
        int found = -1;
        for (int j = 0; j < ht[h].num_waits; j++) {
            if (ht[h].wait_ids[j] == ev->old_event) {
                found = j;
                break;
            }
        }
        if (found >= 0) {
            ht[h].wait_ns[found] += ev->duration_ns;
        } else if (ht[h].num_waits < 64) {
            int n = ht[h].num_waits;
            ht[h].wait_ids[n] = ev->old_event;
            ht[h].wait_ns[n]  = ev->duration_ns;
            ht[h].num_waits++;
        }
    }

    double db_time_ms = (double)db_time_ns / 1e6;

    struct pgwt_query_row *rows = calloc(num_entries, sizeof(*rows));
    int nr = 0;

    for (int i = 0; i < QUERY_HT_SIZE; i++) {
        if (ht[i].count == 0)
            continue;

        struct pgwt_query_row *r = &rows[nr];
        r->query_id = ht[i].query_id;
        r->count    = ht[i].count;
        r->total_ms = (double)ht[i].total_ns / 1e6;
        r->avg_us   = ht[i].count > 0
                     ? (double)ht[i].total_ns / (double)ht[i].count / 1000.0
                     : 0;
        r->pct_db   = db_time_ms > 0 ? r->total_ms / db_time_ms * 100.0 : 0;

        /* Find top wait */
        uint32_t top_id = 0;
        uint64_t top_ns = 0;
        for (int j = 0; j < ht[i].num_waits; j++) {
            if (ht[i].wait_ns[j] > top_ns) {
                top_ns = ht[i].wait_ns[j];
                top_id = ht[i].wait_ids[j];
            }
        }
        r->top_wait_id = top_id;
        if (top_id == 0)
            snprintf(r->top_wait, sizeof(r->top_wait), "CPU*");
        else
            pgwt_event_full_name(top_id, r->top_wait, sizeof(r->top_wait));

        for (int c = 0; c < PGWT_NUM_CLASSES; c++)
            r->class_ms[c] = (double)ht[i].class_ns[c] / 1e6;

        nr++;
    }

    free(ht);

    qsort(rows, nr, sizeof(rows[0]), cmp_query_row_desc);

    out->rows       = rows;
    out->num_rows   = nr;
    out->db_time_ms = db_time_ms;
}

/* ══════════════════════════════════════════════════════════════
 * Summary-based compute functions
 * Each pgwt_summary_accum record represents 1 second of pre-aggregated data.
 * ══════════════════════════════════════════════════════════════ */

/* Helper: check if a summary event matches a filter */
static int summary_event_matches_filter(const struct pgwt_filter *f,
                                         uint32_t event_id)
{
    if (f->class_name[0] != '\0') {
        const char *cls = pgwt_class_name(event_id);
        if (strcasecmp(cls, f->class_name) != 0)
            return 0;
    }
    if (f->event_id != 0 && event_id != f->event_id)
        return 0;
    return 1;
}

/* ── AAS from summaries ───────────────────────────────────── */

void pgwt_compute_aas_from_summaries(
    const struct pgwt_summary_accum *records, int count,
    const struct pgwt_filter *f,
    uint64_t from_ns, uint64_t to_ns, int num_buckets,
    struct pgwt_aas_result *out)
{
    memset(out, 0, sizeof(*out));

    uint64_t range_ns = to_ns - from_ns;
    if (range_ns == 0 || num_buckets <= 0) {
        out->bucket_ns = 1000000000ULL;
        return;
    }

    uint64_t bucket_ns = (range_ns + (uint64_t)num_buckets - 1) / (uint64_t)num_buckets;
    if (bucket_ns < 1000000000ULL)
        bucket_ns = 1000000000ULL;

    int actual_buckets = (int)((range_ns + bucket_ns - 1) / bucket_ns);
    struct pgwt_aas_bucket *buckets = calloc(actual_buckets, sizeof(*buckets));
    if (!buckets) return;

    for (int i = 0; i < actual_buckets; i++)
        buckets[i].start_ns = from_ns + (uint64_t)i * bucket_ns;

    int has_class_filter = (f->class_name[0] != '\0');
    int has_event_filter = (f->event_id != 0);
    int has_pid_filter   = (f->pid != 0);
    int has_query_filter = (f->query_id != 0);

    for (int r = 0; r < count; r++) {
        const struct pgwt_summary_accum *rec = &records[r];
        uint64_t wall = rec->second_wall_ns;

        if (wall < from_ns || wall >= to_ns)
            continue;

        int bi = (int)((wall - from_ns) / bucket_ns);
        if (bi >= actual_buckets) bi = actual_buckets - 1;

        /* PID or query filter: we can't use class_ns directly because we
         * don't know which PIDs/queries contributed to each class.
         * Fall back to summing from per-event stats (still fast). */
        if (has_pid_filter || has_query_filter) {
            /* For PID filter: use session data → approximate with full events */
            /* For query filter: use query data → approximate with event totals */
            /* In summary mode these filters are approximate:
             * we can filter sessions/queries but not correlate to class exactly.
             * Skip for now — summaries give full system view. */
            continue;
        }

        if (!has_class_filter && !has_event_filter) {
            /* No filter: directly use class_ns (fast path) */
            for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
                if (c == PGWT_CLASS_ACTIVITY)
                    continue; /* exclude idle */
                buckets[bi].class_aas[c] += (double)rec->class_ns[c];
            }
        } else {
            /* Class/event filter: iterate per-event entries */
            for (int e = 0; e < SUMMARY_MAX_EVENTS; e++) {
                const struct pgwt_summary_event *se = &rec->events[e];
                if (se->event_id == 0 && se->count == 0) continue;
                if (pgwt_is_idle_event(se->event_id)) continue;
                if (!summary_event_matches_filter(f, se->event_id)) continue;

                int cls = pgwt_wait_class_index(se->event_id);
                buckets[bi].class_aas[cls] += (double)se->total_ns;
            }
        }
    }

    /* Convert ns → AAS */
    double max_aas = 0.0;
    for (int i = 0; i < actual_buckets; i++) {
        double total = 0.0;
        for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
            buckets[i].class_aas[c] /= (double)bucket_ns;
            total += buckets[i].class_aas[c];
        }
        if (total > max_aas) max_aas = total;
    }

    out->buckets     = buckets;
    out->num_buckets = actual_buckets;
    out->bucket_ns   = bucket_ns;
    out->max_aas     = max_aas;
}

/* ── Time Model from summaries ────────────────────────────── */

void pgwt_compute_time_model_from_summaries(
    const struct pgwt_summary_accum *records, int count,
    const struct pgwt_filter *f, double wall_ms,
    struct pgwt_tm_result *out)
{
    memset(out, 0, sizeof(*out));
    out->wall_ms = wall_ms;

    struct class_accum classes[PGWT_NUM_CLASSES];
    memset(classes, 0, sizeof(classes));
    for (int i = 0; i < PGWT_NUM_CLASSES; i++)
        snprintf(classes[i].name, sizeof(classes[i].name), "%s",
                 pgwt_class_names[i]);

    struct event_accum *ev_accum = calloc(MAX_DISTINCT_EVENTS, sizeof(*ev_accum));
    int num_ev_accum = 0;
    double db_time_ns = 0.0;
    double idle_time_ns = 0.0;

    for (int r = 0; r < count; r++) {
        const struct pgwt_summary_accum *rec = &records[r];

        /* If no class/event filter: use class_ns directly */
        if (f->class_name[0] == '\0' && f->event_id == 0) {
            for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
                if (c == PGWT_CLASS_ACTIVITY)
                    idle_time_ns += (double)rec->class_ns[c];
                else {
                    classes[c].total_ns += (double)rec->class_ns[c];
                    db_time_ns += (double)rec->class_ns[c];
                }
            }
        }

        /* Per-event stats: iterate event entries */
        for (int e = 0; e < SUMMARY_MAX_EVENTS; e++) {
            const struct pgwt_summary_event *se = &rec->events[e];
            if (se->event_id == 0 && se->count == 0) continue;
            if (!summary_event_matches_filter(f, se->event_id)) continue;

            if (pgwt_is_idle_event(se->event_id)) continue;

            int cls_idx = pgwt_wait_class_index(se->event_id);

            /* With filter: accumulate class totals from events */
            if (f->class_name[0] != '\0' || f->event_id != 0) {
                classes[cls_idx].total_ns += (double)se->total_ns;
                db_time_ns += (double)se->total_ns;
            }

            /* Sub-event accumulation */
            int found = -1;
            for (int j = 0; j < num_ev_accum; j++) {
                if (ev_accum[j].event_id == se->event_id) {
                    found = j; break;
                }
            }
            if (found >= 0) {
                ev_accum[found].total_ns += (double)se->total_ns;
            } else if (num_ev_accum < MAX_DISTINCT_EVENTS) {
                snprintf(ev_accum[num_ev_accum].class_name,
                         sizeof(ev_accum[num_ev_accum].class_name),
                         "%s", pgwt_class_names[cls_idx]);
                ev_accum[num_ev_accum].event_id = se->event_id;
                ev_accum[num_ev_accum].total_ns = (double)se->total_ns;
                num_ev_accum++;
            }
        }
    }

    double db_time_ms = db_time_ns / 1e6;
    double idle_ms    = idle_time_ns / 1e6;

    /* Build result rows (same format as raw compute) */
    int max_rows = 1 + PGWT_NUM_CLASSES * 4;
    struct pgwt_tm_row *rows = calloc(max_rows, sizeof(*rows));
    int nr = 0;

    snprintf(rows[nr].name, sizeof(rows[nr].name), "DB Time");
    rows[nr].time_ms     = db_time_ms;
    rows[nr].pct_db_time = 100.0;
    rows[nr].aas         = wall_ms > 0 ? db_time_ms / wall_ms : 0;
    rows[nr].indent      = 0;
    nr++;

    qsort(classes, PGWT_NUM_CLASSES, sizeof(classes[0]), cmp_class_desc);
    qsort(ev_accum, num_ev_accum, sizeof(ev_accum[0]), cmp_event_desc);

    for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
        if (classes[c].total_ns <= 0) continue;

        double cls_ms = classes[c].total_ns / 1e6;
        double pct = db_time_ms > 0 ? cls_ms / db_time_ms * 100.0 : 0;

        const char *display2 = classes[c].name;
        for (int k = 0; k < PGWT_NUM_CLASSES; k++) {
            if (strcasecmp(classes[c].name, pgwt_class_names[k]) == 0) {
                display2 = pgwt_class_display[k];
                break;
            }
        }
        if (strcasecmp(classes[c].name, "cpu") == 0)
            snprintf(rows[nr].name, sizeof(rows[nr].name), "CPU*");
        else
            snprintf(rows[nr].name, sizeof(rows[nr].name), "%.31s", display2);
        rows[nr].time_ms     = cls_ms;
        rows[nr].pct_db_time = pct;
        rows[nr].aas         = wall_ms > 0 ? cls_ms / wall_ms : 0;
        rows[nr].indent      = 1;
        nr++;

        if (strcasecmp(classes[c].name, "cpu") == 0) continue;

        int sub_count = 0;
        for (int j = 0; j < num_ev_accum && sub_count < 3; j++) {
            if (strcasecmp(ev_accum[j].class_name, classes[c].name) != 0)
                continue;
            double sub_ms  = ev_accum[j].total_ns / 1e6;
            double sub_pct = db_time_ms > 0 ? sub_ms / db_time_ms * 100.0 : 0;
            if (sub_pct < 0.1) break;

            char buf[64];
            pgwt_event_full_name(ev_accum[j].event_id, buf, sizeof(buf));
            snprintf(rows[nr].name, sizeof(rows[nr].name), "%s", buf);
            rows[nr].time_ms     = sub_ms;
            rows[nr].pct_db_time = sub_pct;
            rows[nr].aas         = wall_ms > 0 ? sub_ms / wall_ms : 0;
            rows[nr].indent      = 2;
            nr++;
            sub_count++;
        }
    }

    free(ev_accum);

    out->rows         = rows;
    out->num_rows     = nr;
    out->db_time_ms   = db_time_ms;
    out->idle_time_ms = idle_ms;
    out->aas          = wall_ms > 0 ? db_time_ms / wall_ms : 0;
}

/* ── Top Events from summaries ────────────────────────────── */

void pgwt_compute_top_events_from_summaries(
    const struct pgwt_summary_accum *records, int count,
    const struct pgwt_filter *f, double wall_ms,
    struct pgwt_events_result *out)
{
    memset(out, 0, sizeof(*out));

    /* Merge per-event stats across all summary records */
    struct top_event_accum *ht = calloc(EVENT_HT_SIZE, sizeof(*ht));
    int num_entries = 0;
    uint64_t db_time_ns = 0;

    for (int r = 0; r < count; r++) {
        const struct pgwt_summary_accum *rec = &records[r];

        for (int e = 0; e < SUMMARY_MAX_EVENTS; e++) {
            const struct pgwt_summary_event *se = &rec->events[e];
            if (se->event_id == 0 && se->count == 0) continue;
            if (pgwt_is_idle_event(se->event_id)) continue;
            if (!summary_event_matches_filter(f, se->event_id)) continue;

            db_time_ns += se->total_ns;

            uint32_t h = se->event_id & EVENT_HT_MASK;
            while (ht[h].count > 0 && ht[h].event_id != se->event_id)
                h = (h + 1) & EVENT_HT_MASK;

            if (ht[h].count == 0) {
                ht[h].event_id = se->event_id;
                num_entries++;
            }
            ht[h].count += se->count;
            ht[h].total_ns += se->total_ns;
            if (se->max_ns > ht[h].max_ns)
                ht[h].max_ns = se->max_ns;
        }
    }

    double db_time_ms = (double)db_time_ns / 1e6;

    struct pgwt_event_row *rows = calloc(num_entries, sizeof(*rows));
    int nr = 0;

    for (int i = 0; i < EVENT_HT_SIZE; i++) {
        if (ht[i].count == 0) continue;

        struct pgwt_event_row *row = &rows[nr];
        row->event_id = ht[i].event_id;
        row->count    = ht[i].count;
        row->total_ms = (double)ht[i].total_ns / 1e6;
        row->avg_us   = ht[i].count > 0
                       ? (double)ht[i].total_ns / (double)ht[i].count / 1000.0 : 0;
        row->max_us   = (double)ht[i].max_ns / 1000.0;
        row->pct_db   = db_time_ms > 0 ? row->total_ms / db_time_ms * 100.0 : 0;
        row->aas      = wall_ms > 0 ? row->total_ms / wall_ms : 0;

        if (ht[i].event_id == 0)
            snprintf(row->name, sizeof(row->name), "CPU*");
        else
            pgwt_event_full_name(ht[i].event_id, row->name, sizeof(row->name));
        nr++;
    }

    free(ht);
    qsort(rows, nr, sizeof(rows[0]), cmp_event_row_desc);

    out->rows       = rows;
    out->num_rows   = nr;
    out->db_time_ms = db_time_ms;
}

/* ── Top Sessions from summaries ──────────────────────────── */

void pgwt_compute_top_sessions_from_summaries(
    const struct pgwt_summary_accum *records, int count,
    const struct pgwt_filter *f, double wall_ms,
    struct pgwt_sessions_result *out)
{
    (void)f; (void)wall_ms;
    memset(out, 0, sizeof(*out));

    /* Merge per-PID stats.  Use simple open-addressing HT. */
    struct {
        uint32_t pid;
        uint64_t db_time_ns;
        uint64_t cpu_ns;
        uint32_t top_wait_id;
        uint64_t top_wait_ns;
    } *ht = calloc(SESSION_HT_SIZE, sizeof(*ht));
    int num_entries = 0;

    for (int r = 0; r < count; r++) {
        const struct pgwt_summary_accum *rec = &records[r];

        for (int s = 0; s < SUMMARY_MAX_SESSIONS; s++) {
            const struct pgwt_summary_session *ss = &rec->sessions[s];
            if (ss->pid == 0 && ss->db_time_ns == 0) continue;
            if (f->pid != 0 && ss->pid != f->pid) continue;

            uint32_t h = ss->pid % SESSION_HT_SIZE;
            while (ht[h].pid != 0 && ht[h].pid != ss->pid)
                h = (h + 1) % SESSION_HT_SIZE;

            if (ht[h].pid == 0) {
                ht[h].pid = ss->pid;
                num_entries++;
            }
            ht[h].db_time_ns += ss->db_time_ns;
            ht[h].cpu_ns += ss->cpu_ns;
            if (ss->top_wait_ns > ht[h].top_wait_ns) {
                ht[h].top_wait_id = ss->top_wait_id;
                ht[h].top_wait_ns = ss->top_wait_ns;
            }
        }
    }

    struct pgwt_session_row *rows = calloc(num_entries, sizeof(*rows));
    int nr = 0;

    for (int i = 0; i < SESSION_HT_SIZE; i++) {
        if (ht[i].pid == 0) continue;

        struct pgwt_session_row *row = &rows[nr];
        row->pid        = ht[i].pid;
        row->db_time_ms = (double)ht[i].db_time_ns / 1e6;
        double db = (double)ht[i].db_time_ns;
        row->cpu_pct  = db > 0 ? (double)ht[i].cpu_ns / db * 100.0 : 0;
        row->wait_pct = 100.0 - row->cpu_pct;
        row->top_wait_id = ht[i].top_wait_id;
        if (ht[i].top_wait_id == 0)
            snprintf(row->top_wait, sizeof(row->top_wait), "CPU*");
        else
            pgwt_event_full_name(ht[i].top_wait_id, row->top_wait,
                                  sizeof(row->top_wait));
        nr++;
    }

    free(ht);
    qsort(rows, nr, sizeof(rows[0]), cmp_session_row_desc);

    out->rows     = rows;
    out->num_rows = nr;
}

/* ── Top Queries from summaries ───────────────────────────── */

void pgwt_compute_top_queries_from_summaries(
    const struct pgwt_summary_accum *records, int count,
    const struct pgwt_filter *f, double wall_ms,
    struct pgwt_queries_result *out)
{
    (void)wall_ms;
    memset(out, 0, sizeof(*out));

    struct {
        uint64_t query_id;
        uint64_t count;
        uint64_t total_ns;
        uint32_t top_wait_id;
        uint64_t top_wait_ns;
    } *ht = calloc(QUERY_HT_SIZE, sizeof(*ht));
    int num_entries = 0;
    uint64_t db_time_ns = 0;

    for (int r = 0; r < count; r++) {
        const struct pgwt_summary_accum *rec = &records[r];

        for (int q = 0; q < SUMMARY_MAX_QUERIES; q++) {
            const struct pgwt_summary_query *sq = &rec->queries[q];
            if (sq->query_id == 0 && sq->count == 0) continue;
            if (f->query_id != 0 && sq->query_id != f->query_id) continue;

            db_time_ns += sq->total_ns;

            uint32_t h = (uint32_t)(sq->query_id ^ (sq->query_id >> 32)) & QUERY_HT_MASK;
            while (ht[h].count > 0 && ht[h].query_id != sq->query_id)
                h = (h + 1) & QUERY_HT_MASK;

            if (ht[h].count == 0) {
                ht[h].query_id = sq->query_id;
                num_entries++;
            }
            ht[h].count += sq->count;
            ht[h].total_ns += sq->total_ns;
            if (sq->top_wait_ns > ht[h].top_wait_ns) {
                ht[h].top_wait_id = sq->top_wait_id;
                ht[h].top_wait_ns = sq->top_wait_ns;
            }
        }
    }

    double db_time_ms = (double)db_time_ns / 1e6;

    struct pgwt_query_row *rows = calloc(num_entries, sizeof(*rows));
    int nr = 0;

    for (int i = 0; i < QUERY_HT_SIZE; i++) {
        if (ht[i].count == 0) continue;

        struct pgwt_query_row *row = &rows[nr];
        row->query_id = ht[i].query_id;
        row->count    = ht[i].count;
        row->total_ms = (double)ht[i].total_ns / 1e6;
        row->avg_us   = ht[i].count > 0
                       ? (double)ht[i].total_ns / (double)ht[i].count / 1000.0 : 0;
        row->pct_db   = db_time_ms > 0 ? row->total_ms / db_time_ms * 100.0 : 0;
        row->top_wait_id = ht[i].top_wait_id;
        if (ht[i].top_wait_id == 0)
            snprintf(row->top_wait, sizeof(row->top_wait), "CPU*");
        else
            pgwt_event_full_name(ht[i].top_wait_id, row->top_wait,
                                  sizeof(row->top_wait));
        nr++;
    }

    free(ht);
    qsort(rows, nr, sizeof(rows[0]), cmp_query_row_desc);

    out->rows       = rows;
    out->num_rows   = nr;
    out->db_time_ms = db_time_ms;
}
