/* compute.c — Server-side compute functions for pgwt-server
 *
 * Direct port of client/src/compute.rs. Works on raw pgwt_trace_event arrays.
 * All result structs use malloc'd arrays — caller frees with free(result->rows).
 */
#include "compute.h"
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

        /* Class name: capitalize, add * for CPU */
        if (strcasecmp(classes[c].name, "cpu") == 0)
            snprintf(rows[nr].name, sizeof(rows[nr].name), "CPU*");
        else {
            /* Capitalize first letter */
            snprintf(rows[nr].name, sizeof(rows[nr].name), "%.31s", classes[c].name);
            if (rows[nr].name[0] >= 'a' && rows[nr].name[0] <= 'z')
                rows[nr].name[0] -= 32;
        }
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

        nr++;
    }

    free(ht);

    qsort(rows, nr, sizeof(rows[0]), cmp_query_row_desc);

    out->rows       = rows;
    out->num_rows   = nr;
    out->db_time_ms = db_time_ms;
}
