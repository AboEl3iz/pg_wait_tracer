/* compute.c — Server-side compute functions for pgwt-server
 *
 * Direct port of client/src/compute.rs. Works on raw pgwt_trace_event arrays.
 * All result structs use malloc'd arrays — caller frees with free(result->rows).
 */
#include "compute.h"
#include "summary_writer.h"
#include "summary_reader.h"
#include "wait_event.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* pgwt_duration_to_bucket: log2 latency bucket for heatmap.
 * In daemon build this comes from map_reader.c; inline it here for server. */
#ifdef PGWT_SERVER
static uint32_t compute_duration_to_bucket(uint64_t ns)
{
    if (ns < 1000) return 0;
    uint64_t us = ns / 1000;
    uint32_t b = 0;
    while (us > 1 && b < HISTOGRAM_BUCKETS - 1) {
        us >>= 1;
        b++;
    }
    return b;
}
#else
#include "map_reader.h"
#define compute_duration_to_bucket pgwt_duration_to_bucket
#endif

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

/* Hash table entry for per-event total accumulation (pass 1) */
struct aas_event_accum {
    uint32_t event_id;
    uint64_t total_ns;
};

#define AAS_EVT_HT_SIZE 512
#define AAS_EVT_HT_MASK (AAS_EVT_HT_SIZE - 1)

static int cmp_aas_event_series(const void *a, const void *b)
{
    double da = ((const struct pgwt_aas_event_series *)a)->total_aas;
    double db = ((const struct pgwt_aas_event_series *)b)->total_aas;
    return (db > da) - (db < da);
}

void pgwt_compute_aas(const struct pgwt_trace_event *events, int count,
                      const struct pgwt_filter *f,
                      uint64_t from_ns, uint64_t to_ns, int num_buckets,
                      int detail_events, int max_series,
                      struct pgwt_aas_result *out)
{
    memset(out, 0, sizeof(*out));

    uint64_t range_ns = to_ns - from_ns;
    if (range_ns == 0 || num_buckets <= 0) {
        out->bucket_ns = 1000000000ULL;
        return;
    }

    /* Raw events have ns precision — no minimum bucket width */
    uint64_t bucket_ns = (range_ns + (uint64_t)num_buckets - 1) / (uint64_t)num_buckets;
    if (bucket_ns == 0)
        bucket_ns = 1;

    int actual_buckets = (int)((range_ns + bucket_ns - 1) / bucket_ns);
    struct pgwt_aas_bucket *buckets = calloc(actual_buckets, sizeof(*buckets));
    if (!buckets)
        return;

    for (int i = 0; i < actual_buckets; i++)
        buckets[i].start_ns = from_ns + (uint64_t)i * bucket_ns;

    /* Pass 1: accumulate class AAS + per-event totals (if detail requested) */
    struct aas_event_accum *evt_ht = NULL;
    if (detail_events) {
        evt_ht = calloc(AAS_EVT_HT_SIZE, sizeof(*evt_ht));
        if (!evt_ht) { free(buckets); return; }
    }

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

        /* Per-event total for sorting (pass 1) */
        if (evt_ht) {
            uint32_t h = ev->old_event & AAS_EVT_HT_MASK;
            while (evt_ht[h].total_ns > 0 && evt_ht[h].event_id != ev->old_event)
                h = (h + 1) & AAS_EVT_HT_MASK;
            evt_ht[h].event_id = ev->old_event;
            evt_ht[h].total_ns += ev->duration_ns;
        }
    }

    /* Convert accumulated ns → AAS (class buckets) */
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

    /* Per-event breakdown: sort top N, then pass 2 for per-bucket data */
    if (evt_ht) {
        if (max_series <= 0) max_series = AAS_MAX_EVENT_SERIES;
        if (max_series > AAS_MAX_EVENT_SERIES) max_series = AAS_MAX_EVENT_SERIES;

        /* Collect all events from hash table into event_series for sorting */
        struct pgwt_aas_event_series all[AAS_EVT_HT_SIZE];
        int n_all = 0;
        for (int i = 0; i < AAS_EVT_HT_SIZE; i++) {
            if (evt_ht[i].total_ns == 0) continue;
            all[n_all].event_id = evt_ht[i].event_id;
            all[n_all].total_aas = (double)evt_ht[i].total_ns / (double)range_ns;
            pgwt_event_full_name(evt_ht[i].event_id, all[n_all].name,
                                 sizeof(all[n_all].name));
            n_all++;
        }
        free(evt_ht);

        qsort(all, n_all, sizeof(all[0]), cmp_aas_event_series);

        int ns = n_all < max_series ? n_all : max_series;
        out->num_event_series = ns;
        for (int i = 0; i < ns; i++)
            out->event_series[i] = all[i];

        if (ns > 0) {
            /* Build lookup: event_id → series index (linear, ns is small) */
            out->event_aas = calloc((size_t)actual_buckets * ns, sizeof(double));
            if (!out->event_aas) { out->num_event_series = 0; return; }

            /* Pass 2: accumulate per-event per-bucket */
            for (int i = 0; i < count; i++) {
                const struct pgwt_trace_event *ev = &events[i];
                if (!pgwt_filter_matches(f, ev) || pgwt_is_idle_event(ev->old_event))
                    continue;

                /* Find series index for this event */
                int si = -1;
                for (int s = 0; s < ns; s++) {
                    if (out->event_series[s].event_id == ev->old_event) {
                        si = s; break;
                    }
                }
                if (si < 0) continue;  /* not in top N */

                uint64_t ev_start = ev->timestamp_ns;
                uint64_t ev_end   = ev->timestamp_ns + ev->duration_ns;
                if (ev_end <= from_ns || ev_start >= to_ns)
                    continue;

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
                        out->event_aas[b * ns + si] += (double)(o_end - o_start);
                }
            }

            /* Convert ns → AAS and compute max */
            double evt_max = 0.0;
            for (int b = 0; b < actual_buckets; b++) {
                double total = 0.0;
                for (int s = 0; s < ns; s++) {
                    out->event_aas[b * ns + s] /= (double)bucket_ns;
                    total += out->event_aas[b * ns + s];
                }
                if (total > evt_max) evt_max = total;
            }
            out->max_aas = evt_max;  /* override with event-level max */
        }
    }
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

        /* Per-event breakdown: sort by time desc, pick top 16 */
        r->num_events = 0;
        if (ht[i].num_waits > 0) {
            /* Simple selection sort for top 16 */
            uint8_t used[64] = {0};
            for (int k = 0; k < 16 && k < ht[i].num_waits; k++) {
                int best = -1;
                uint64_t best_ns = 0;
                for (int j = 0; j < ht[i].num_waits; j++) {
                    if (!used[j] && ht[i].wait_ns[j] > best_ns) {
                        best_ns = ht[i].wait_ns[j];
                        best = j;
                    }
                }
                if (best < 0) break;
                used[best] = 1;
                r->event_ids[k] = ht[i].wait_ids[best];
                r->event_ms[k]  = (double)ht[i].wait_ns[best] / 1e6;
                r->num_events++;
            }
        }

        nr++;
    }

    free(ht);

    qsort(rows, nr, sizeof(rows[0]), cmp_query_row_desc);

    out->rows       = rows;
    out->num_rows   = nr;
    out->db_time_ms = db_time_ms;
}

/* ── Heatmap (latency distribution over time) ─────────────── */

void pgwt_compute_heatmap(const struct pgwt_trace_event *events, int count,
                          const struct pgwt_filter *f,
                          uint64_t from_ns, uint64_t to_ns, int num_buckets,
                          struct pgwt_heatmap_result *out)
{
    memset(out, 0, sizeof(*out));

    uint64_t range_ns = to_ns - from_ns;
    if (range_ns == 0 || num_buckets <= 0)
        return;

    /* Raw events have ns precision — no minimum bucket width */
    uint64_t bucket_ns = (range_ns + (uint64_t)num_buckets - 1) / (uint64_t)num_buckets;
    if (bucket_ns == 0)
        bucket_ns = 1;

    int actual_buckets = (int)((range_ns + bucket_ns - 1) / bucket_ns);
    int grid_size = actual_buckets * HISTOGRAM_BUCKETS;

    uint64_t *grid = calloc(grid_size, sizeof(uint64_t));
    uint64_t *times = malloc(actual_buckets * sizeof(uint64_t));
    if (!grid || !times) {
        free(grid);
        free(times);
        return;
    }

    for (int i = 0; i < actual_buckets; i++)
        times[i] = from_ns + (uint64_t)i * bucket_ns;

    uint64_t total = 0;

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (!pgwt_filter_matches(f, ev) || pgwt_is_idle_event(ev->old_event))
            continue;

        uint64_t ev_ts = ev->timestamp_ns;
        if (ev_ts < from_ns || ev_ts >= to_ns)
            continue;

        int time_b = (int)((ev_ts - from_ns) / bucket_ns);
        if (time_b >= actual_buckets)
            time_b = actual_buckets - 1;

        int lat_b = (int)compute_duration_to_bucket(ev->duration_ns);

        grid[time_b * HISTOGRAM_BUCKETS + lat_b]++;
        total++;
    }

    /* Find max cell for color scaling */
    uint64_t max_count = 0;
    for (int i = 0; i < grid_size; i++) {
        if (grid[i] > max_count)
            max_count = grid[i];
    }

    out->grid         = grid;
    out->num_buckets  = actual_buckets;
    out->bucket_ns    = bucket_ns;
    out->times        = times;
    out->max_count    = max_count;
    out->total_events = total;
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

struct aas_summary_ctx {
    struct pgwt_aas_bucket *buckets;
    int      actual_buckets;
    uint64_t from_ns;
    uint64_t bucket_ns;
    const struct pgwt_filter *f;
    int has_class_filter;
    int has_event_filter;
    int has_pid_filter;
    int has_query_filter;
};

static int aas_summary_visitor(const struct pgwt_summary_accum *rec, void *arg)
{
    struct aas_summary_ctx *ctx = arg;
    uint64_t wall = rec->second_wall_ns;

    int bi = (int)((wall - ctx->from_ns) / ctx->bucket_ns);
    if (bi >= ctx->actual_buckets) bi = ctx->actual_buckets - 1;
    if (bi < 0) bi = 0;

    /* PID filter: summaries don't have per-PID class breakdown */
    if (ctx->has_pid_filter)
        return 0;

    /* Query filter: use per-query class_ns (v2 data) */
    if (ctx->has_query_filter) {
        for (int q = 0; q < SUMMARY_MAX_QUERIES; q++) {
            const struct pgwt_summary_query *sq = &rec->queries[q];
            if (sq->query_id == 0 && sq->count == 0) continue;
            if (sq->query_id != ctx->f->query_id) continue;
            for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
                if (c == PGWT_CLASS_ACTIVITY) continue;
                ctx->buckets[bi].class_aas[c] += (double)sq->class_ns[c];
            }
            break;
        }
        return 0;
    }

    if (!ctx->has_class_filter && !ctx->has_event_filter) {
        for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
            if (c == PGWT_CLASS_ACTIVITY) continue;
            ctx->buckets[bi].class_aas[c] += (double)rec->class_ns[c];
        }
    } else {
        for (int e = 0; e < SUMMARY_MAX_EVENTS; e++) {
            const struct pgwt_summary_event *se = &rec->events[e];
            if (se->event_id == 0 && se->count == 0) continue;
            if (pgwt_is_idle_event(se->event_id)) continue;
            if (!summary_event_matches_filter(ctx->f, se->event_id)) continue;
            int cls = pgwt_wait_class_index(se->event_id);
            ctx->buckets[bi].class_aas[cls] += (double)se->total_ns;
        }
    }
    return 0;
}

void pgwt_compute_aas_from_summaries(
    const char *trace_dir, uint64_t from_ns, uint64_t to_ns,
    const struct pgwt_filter *f, int num_buckets,
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

    struct aas_summary_ctx ctx = {
        .buckets = buckets,
        .actual_buckets = actual_buckets,
        .from_ns = from_ns,
        .bucket_ns = bucket_ns,
        .f = f,
        .has_class_filter = (f->class_name[0] != '\0'),
        .has_event_filter = (f->event_id != 0),
        .has_pid_filter   = (f->pid != 0),
        .has_query_filter = (f->query_id != 0),
    };

    pgwt_visit_summaries(trace_dir, from_ns, to_ns, aas_summary_visitor, &ctx);

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

struct tm_summary_ctx {
    struct class_accum classes[PGWT_NUM_CLASSES];
    struct event_accum *ev_accum;
    int    num_ev_accum;
    double db_time_ns;
    double idle_time_ns;
    const struct pgwt_filter *f;
};

static int tm_summary_visitor(const struct pgwt_summary_accum *rec, void *arg)
{
    struct tm_summary_ctx *ctx = arg;
    const struct pgwt_filter *f = ctx->f;

    /* Query filter: use per-query class_ns + top_events */
    if (f->query_id != 0) {
        for (int q = 0; q < SUMMARY_MAX_QUERIES; q++) {
            const struct pgwt_summary_query *sq = &rec->queries[q];
            if (sq->query_id == 0 && sq->count == 0) continue;
            if (sq->query_id != f->query_id) continue;

            for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
                if (c == PGWT_CLASS_ACTIVITY)
                    ctx->idle_time_ns += (double)sq->class_ns[c];
                else {
                    ctx->classes[c].total_ns += (double)sq->class_ns[c];
                    ctx->db_time_ns += (double)sq->class_ns[c];
                }
            }
            for (int j = 0; j < sq->num_top_events; j++) {
                uint32_t eid = sq->top_events[j].event_id;
                if (pgwt_is_idle_event(eid)) continue;
                double ns = (double)sq->top_events[j].total_ns;
                int cls_idx = pgwt_wait_class_index(eid);
                int found = -1;
                for (int k = 0; k < ctx->num_ev_accum; k++) {
                    if (ctx->ev_accum[k].event_id == eid) { found = k; break; }
                }
                if (found >= 0) {
                    ctx->ev_accum[found].total_ns += ns;
                } else if (ctx->num_ev_accum < MAX_DISTINCT_EVENTS) {
                    snprintf(ctx->ev_accum[ctx->num_ev_accum].class_name,
                             sizeof(ctx->ev_accum[ctx->num_ev_accum].class_name),
                             "%s", pgwt_class_names[cls_idx]);
                    ctx->ev_accum[ctx->num_ev_accum].event_id = eid;
                    ctx->ev_accum[ctx->num_ev_accum].total_ns = ns;
                    ctx->num_ev_accum++;
                }
            }
            break;
        }
        return 0;
    }

    /* If no class/event filter: use class_ns directly */
    if (f->class_name[0] == '\0' && f->event_id == 0) {
        for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
            if (c == PGWT_CLASS_ACTIVITY)
                ctx->idle_time_ns += (double)rec->class_ns[c];
            else {
                ctx->classes[c].total_ns += (double)rec->class_ns[c];
                ctx->db_time_ns += (double)rec->class_ns[c];
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
            ctx->classes[cls_idx].total_ns += (double)se->total_ns;
            ctx->db_time_ns += (double)se->total_ns;
        }

        /* Sub-event accumulation */
        int found = -1;
        for (int j = 0; j < ctx->num_ev_accum; j++) {
            if (ctx->ev_accum[j].event_id == se->event_id) {
                found = j; break;
            }
        }
        if (found >= 0) {
            ctx->ev_accum[found].total_ns += (double)se->total_ns;
        } else if (ctx->num_ev_accum < MAX_DISTINCT_EVENTS) {
            snprintf(ctx->ev_accum[ctx->num_ev_accum].class_name,
                     sizeof(ctx->ev_accum[ctx->num_ev_accum].class_name),
                     "%s", pgwt_class_names[cls_idx]);
            ctx->ev_accum[ctx->num_ev_accum].event_id = se->event_id;
            ctx->ev_accum[ctx->num_ev_accum].total_ns = (double)se->total_ns;
            ctx->num_ev_accum++;
        }
    }
    return 0;
}

void pgwt_compute_time_model_from_summaries(
    const char *trace_dir, uint64_t from_ns, uint64_t to_ns,
    const struct pgwt_filter *f, double wall_ms,
    struct pgwt_tm_result *out)
{
    memset(out, 0, sizeof(*out));
    out->wall_ms = wall_ms;

    struct tm_summary_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.f = f;
    for (int i = 0; i < PGWT_NUM_CLASSES; i++)
        snprintf(ctx.classes[i].name, sizeof(ctx.classes[i].name), "%s",
                 pgwt_class_names[i]);
    ctx.ev_accum = calloc(MAX_DISTINCT_EVENTS, sizeof(*ctx.ev_accum));

    pgwt_visit_summaries(trace_dir, from_ns, to_ns, tm_summary_visitor, &ctx);

    double db_time_ms = ctx.db_time_ns / 1e6;
    double idle_ms    = ctx.idle_time_ns / 1e6;

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

    qsort(ctx.classes, PGWT_NUM_CLASSES, sizeof(ctx.classes[0]), cmp_class_desc);
    qsort(ctx.ev_accum, ctx.num_ev_accum, sizeof(ctx.ev_accum[0]), cmp_event_desc);

    for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
        if (ctx.classes[c].total_ns <= 0) continue;

        double cls_ms = ctx.classes[c].total_ns / 1e6;
        double pct = db_time_ms > 0 ? cls_ms / db_time_ms * 100.0 : 0;

        const char *display2 = ctx.classes[c].name;
        for (int k = 0; k < PGWT_NUM_CLASSES; k++) {
            if (strcasecmp(ctx.classes[c].name, pgwt_class_names[k]) == 0) {
                display2 = pgwt_class_display[k];
                break;
            }
        }
        if (strcasecmp(ctx.classes[c].name, "cpu") == 0)
            snprintf(rows[nr].name, sizeof(rows[nr].name), "CPU*");
        else
            snprintf(rows[nr].name, sizeof(rows[nr].name), "%.31s", display2);
        rows[nr].time_ms     = cls_ms;
        rows[nr].pct_db_time = pct;
        rows[nr].aas         = wall_ms > 0 ? cls_ms / wall_ms : 0;
        rows[nr].indent      = 1;
        nr++;

        if (strcasecmp(ctx.classes[c].name, "cpu") == 0) continue;

        int sub_count = 0;
        for (int j = 0; j < ctx.num_ev_accum && sub_count < 3; j++) {
            if (strcasecmp(ctx.ev_accum[j].class_name, ctx.classes[c].name) != 0)
                continue;
            double sub_ms  = ctx.ev_accum[j].total_ns / 1e6;
            double sub_pct = db_time_ms > 0 ? sub_ms / db_time_ms * 100.0 : 0;
            if (sub_pct < 0.1) break;

            char buf[64];
            pgwt_event_full_name(ctx.ev_accum[j].event_id, buf, sizeof(buf));
            snprintf(rows[nr].name, sizeof(rows[nr].name), "%s", buf);
            rows[nr].time_ms     = sub_ms;
            rows[nr].pct_db_time = sub_pct;
            rows[nr].aas         = wall_ms > 0 ? sub_ms / wall_ms : 0;
            rows[nr].indent      = 2;
            nr++;
            sub_count++;
        }
    }

    free(ctx.ev_accum);

    out->rows         = rows;
    out->num_rows     = nr;
    out->db_time_ms   = db_time_ms;
    out->idle_time_ms = idle_ms;
    out->aas          = wall_ms > 0 ? db_time_ms / wall_ms : 0;
}

/* ── Top Events from summaries ────────────────────────────── */

struct te_summary_ctx {
    struct top_event_accum *ht;
    int      num_entries;
    uint64_t db_time_ns;
    const struct pgwt_filter *f;
};

static int te_summary_visitor(const struct pgwt_summary_accum *rec, void *arg)
{
    struct te_summary_ctx *ctx = arg;

    /* Query filter: use per-query top_events */
    if (ctx->f->query_id != 0) {
        for (int q = 0; q < SUMMARY_MAX_QUERIES; q++) {
            const struct pgwt_summary_query *sq = &rec->queries[q];
            if (sq->query_id == 0 && sq->count == 0) continue;
            if (sq->query_id != ctx->f->query_id) continue;

            for (int j = 0; j < sq->num_top_events; j++) {
                uint32_t eid = sq->top_events[j].event_id;
                if (pgwt_is_idle_event(eid)) continue;
                if (!summary_event_matches_filter(ctx->f, eid)) continue;

                ctx->db_time_ns += sq->top_events[j].total_ns;

                uint32_t h = eid & EVENT_HT_MASK;
                while (ctx->ht[h].count > 0 && ctx->ht[h].event_id != eid)
                    h = (h + 1) & EVENT_HT_MASK;

                if (ctx->ht[h].count == 0) {
                    ctx->ht[h].event_id = eid;
                    ctx->num_entries++;
                }
                ctx->ht[h].count += sq->top_events[j].count;
                ctx->ht[h].total_ns += sq->top_events[j].total_ns;
            }
            break;
        }
        return 0;
    }

    for (int e = 0; e < SUMMARY_MAX_EVENTS; e++) {
        const struct pgwt_summary_event *se = &rec->events[e];
        if (se->event_id == 0 && se->count == 0) continue;
        if (pgwt_is_idle_event(se->event_id)) continue;
        if (!summary_event_matches_filter(ctx->f, se->event_id)) continue;

        ctx->db_time_ns += se->total_ns;

        uint32_t h = se->event_id & EVENT_HT_MASK;
        while (ctx->ht[h].count > 0 && ctx->ht[h].event_id != se->event_id)
            h = (h + 1) & EVENT_HT_MASK;

        if (ctx->ht[h].count == 0) {
            ctx->ht[h].event_id = se->event_id;
            ctx->num_entries++;
        }
        ctx->ht[h].count += se->count;
        ctx->ht[h].total_ns += se->total_ns;
        if (se->max_ns > ctx->ht[h].max_ns)
            ctx->ht[h].max_ns = se->max_ns;
    }
    return 0;
}

void pgwt_compute_top_events_from_summaries(
    const char *trace_dir, uint64_t from_ns, uint64_t to_ns,
    const struct pgwt_filter *f, double wall_ms,
    struct pgwt_events_result *out)
{
    memset(out, 0, sizeof(*out));

    struct te_summary_ctx ctx = {
        .ht = calloc(EVENT_HT_SIZE, sizeof(*ctx.ht)),
        .num_entries = 0,
        .db_time_ns = 0,
        .f = f,
    };
    if (!ctx.ht) return;

    pgwt_visit_summaries(trace_dir, from_ns, to_ns, te_summary_visitor, &ctx);

    double db_time_ms = (double)ctx.db_time_ns / 1e6;

    struct pgwt_event_row *rows = calloc(ctx.num_entries, sizeof(*rows));
    int nr = 0;

    for (int i = 0; i < EVENT_HT_SIZE; i++) {
        if (ctx.ht[i].count == 0) continue;

        struct pgwt_event_row *row = &rows[nr];
        row->event_id = ctx.ht[i].event_id;
        row->count    = ctx.ht[i].count;
        row->total_ms = (double)ctx.ht[i].total_ns / 1e6;
        row->avg_us   = ctx.ht[i].count > 0
                       ? (double)ctx.ht[i].total_ns / (double)ctx.ht[i].count / 1000.0 : 0;
        row->max_us   = (double)ctx.ht[i].max_ns / 1000.0;
        row->pct_db   = db_time_ms > 0 ? row->total_ms / db_time_ms * 100.0 : 0;
        row->aas      = wall_ms > 0 ? row->total_ms / wall_ms : 0;

        if (ctx.ht[i].event_id == 0)
            snprintf(row->name, sizeof(row->name), "CPU*");
        else
            pgwt_event_full_name(ctx.ht[i].event_id, row->name, sizeof(row->name));
        nr++;
    }

    free(ctx.ht);
    qsort(rows, nr, sizeof(rows[0]), cmp_event_row_desc);

    out->rows       = rows;
    out->num_rows   = nr;
    out->db_time_ms = db_time_ms;
}

/* ── Top Sessions from summaries ──────────────────────────── */

struct ts_summary_ht_entry {
    uint32_t pid;
    uint64_t db_time_ns;
    uint64_t cpu_ns;
    uint32_t top_wait_id;
    uint64_t top_wait_ns;
};

struct ts_summary_ctx {
    struct ts_summary_ht_entry *ht;
    int num_entries;
    const struct pgwt_filter *f;
};

static int ts_summary_visitor(const struct pgwt_summary_accum *rec, void *arg)
{
    struct ts_summary_ctx *ctx = arg;

    for (int s = 0; s < SUMMARY_MAX_SESSIONS; s++) {
        const struct pgwt_summary_session *ss = &rec->sessions[s];
        if (ss->pid == 0 && ss->db_time_ns == 0) continue;
        if (ctx->f->pid != 0 && ss->pid != ctx->f->pid) continue;

        uint32_t h = ss->pid % SESSION_HT_SIZE;
        while (ctx->ht[h].pid != 0 && ctx->ht[h].pid != ss->pid)
            h = (h + 1) % SESSION_HT_SIZE;

        if (ctx->ht[h].pid == 0) {
            ctx->ht[h].pid = ss->pid;
            ctx->num_entries++;
        }
        ctx->ht[h].db_time_ns += ss->db_time_ns;
        ctx->ht[h].cpu_ns += ss->cpu_ns;
        if (ss->top_wait_ns > ctx->ht[h].top_wait_ns) {
            ctx->ht[h].top_wait_id = ss->top_wait_id;
            ctx->ht[h].top_wait_ns = ss->top_wait_ns;
        }
    }
    return 0;
}

void pgwt_compute_top_sessions_from_summaries(
    const char *trace_dir, uint64_t from_ns, uint64_t to_ns,
    const struct pgwt_filter *f, double wall_ms,
    struct pgwt_sessions_result *out)
{
    (void)wall_ms;
    memset(out, 0, sizeof(*out));

    struct ts_summary_ctx ctx = {
        .ht = calloc(SESSION_HT_SIZE, sizeof(*ctx.ht)),
        .num_entries = 0,
        .f = f,
    };
    if (!ctx.ht) return;

    pgwt_visit_summaries(trace_dir, from_ns, to_ns, ts_summary_visitor, &ctx);

    struct pgwt_session_row *rows = calloc(ctx.num_entries, sizeof(*rows));
    int nr = 0;

    for (int i = 0; i < SESSION_HT_SIZE; i++) {
        if (ctx.ht[i].pid == 0) continue;

        struct pgwt_session_row *row = &rows[nr];
        row->pid        = ctx.ht[i].pid;
        row->db_time_ms = (double)ctx.ht[i].db_time_ns / 1e6;
        double db = (double)ctx.ht[i].db_time_ns;
        row->cpu_pct  = db > 0 ? (double)ctx.ht[i].cpu_ns / db * 100.0 : 0;
        row->wait_pct = 100.0 - row->cpu_pct;
        row->top_wait_id = ctx.ht[i].top_wait_id;
        if (ctx.ht[i].top_wait_id == 0)
            snprintf(row->top_wait, sizeof(row->top_wait), "CPU*");
        else
            pgwt_event_full_name(ctx.ht[i].top_wait_id, row->top_wait,
                                  sizeof(row->top_wait));
        nr++;
    }

    free(ctx.ht);
    qsort(rows, nr, sizeof(rows[0]), cmp_session_row_desc);

    out->rows     = rows;
    out->num_rows = nr;
}

/* ── Top Queries from summaries ───────────────────────────── */

struct tq_summary_ht_entry {
    uint64_t query_id;
    uint64_t count;
    uint64_t total_ns;
    uint32_t top_wait_id;
    uint64_t top_wait_ns;
    uint64_t class_ns[PGWT_NUM_CLASSES];
};

struct tq_summary_ctx {
    struct tq_summary_ht_entry *ht;
    int      num_entries;
    uint64_t db_time_ns;
    const struct pgwt_filter *f;
};

static int tq_summary_visitor(const struct pgwt_summary_accum *rec, void *arg)
{
    struct tq_summary_ctx *ctx = arg;

    /* Resolve class filter index (-1 = no filter) */
    int filter_cls = -1;
    if (ctx->f->class_name[0] != '\0') {
        for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
            if (strcasecmp(ctx->f->class_name, pgwt_class_names[c]) == 0) {
                filter_cls = c;
                break;
            }
        }
        if (filter_cls < 0) return 0;
    }

    for (int q = 0; q < SUMMARY_MAX_QUERIES; q++) {
        const struct pgwt_summary_query *sq = &rec->queries[q];
        if (sq->query_id == 0 && sq->count == 0) continue;
        if (ctx->f->query_id != 0 && sq->query_id != ctx->f->query_id) continue;

        /* Determine effective time and top wait, respecting class+event filters */
        uint64_t effective_ns = sq->total_ns;
        uint32_t rec_top_wait_id = sq->top_wait_id;
        uint64_t rec_top_wait_ns = sq->top_wait_ns;

        if (filter_cls >= 0 || ctx->f->event_id != 0) {
            /* Use top_events[] to compute class/event-filtered totals */
            effective_ns = 0;
            rec_top_wait_id = 0;
            rec_top_wait_ns = 0;

            for (int j = 0; j < sq->num_top_events; j++) {
                uint32_t eid = sq->top_events[j].event_id;
                /* Class filter */
                if (filter_cls >= 0 && pgwt_wait_class_index(eid) != filter_cls)
                    continue;
                /* Event filter */
                if (ctx->f->event_id != 0 && eid != ctx->f->event_id)
                    continue;

                effective_ns += sq->top_events[j].total_ns;
                if (sq->top_events[j].total_ns > rec_top_wait_ns) {
                    rec_top_wait_ns = sq->top_events[j].total_ns;
                    rec_top_wait_id = eid;
                }
            }

            /* If top_events didn't cover this class, fall back to class_ns */
            if (effective_ns == 0 && filter_cls >= 0 && ctx->f->event_id == 0) {
                int has_class_ns = 0;
                for (int c = 0; c < PGWT_NUM_CLASSES; c++)
                    if (sq->class_ns[c] > 0) { has_class_ns = 1; break; }
                if (has_class_ns)
                    effective_ns = sq->class_ns[filter_cls];
                else if (pgwt_wait_class_index(sq->top_wait_id) == filter_cls)
                    effective_ns = sq->total_ns;
                rec_top_wait_id = sq->top_wait_id;
                rec_top_wait_ns = effective_ns;
            }

            if (effective_ns == 0) continue;
        }

        ctx->db_time_ns += effective_ns;

        uint32_t h = (uint32_t)(sq->query_id ^ (sq->query_id >> 32)) & QUERY_HT_MASK;
        while (ctx->ht[h].count > 0 && ctx->ht[h].query_id != sq->query_id)
            h = (h + 1) & QUERY_HT_MASK;

        if (ctx->ht[h].count == 0) {
            ctx->ht[h].query_id = sq->query_id;
            ctx->num_entries++;
        }
        ctx->ht[h].count += sq->count;
        ctx->ht[h].total_ns += effective_ns;
        /* Per-class breakdown */
        int has_class_ns = 0;
        for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
            ctx->ht[h].class_ns[c] += sq->class_ns[c];
            if (sq->class_ns[c] > 0) has_class_ns = 1;
        }
        if (!has_class_ns) {
            int cls = pgwt_wait_class_index(sq->top_wait_id);
            ctx->ht[h].class_ns[cls] += sq->total_ns;
        }
        if (rec_top_wait_ns > ctx->ht[h].top_wait_ns) {
            ctx->ht[h].top_wait_id = rec_top_wait_id;
            ctx->ht[h].top_wait_ns = rec_top_wait_ns;
        }
    }
    return 0;
}

void pgwt_compute_top_queries_from_summaries(
    const char *trace_dir, uint64_t from_ns, uint64_t to_ns,
    const struct pgwt_filter *f, double wall_ms,
    struct pgwt_queries_result *out)
{
    (void)wall_ms;
    memset(out, 0, sizeof(*out));

    struct tq_summary_ctx ctx = {
        .ht = calloc(QUERY_HT_SIZE, sizeof(*ctx.ht)),
        .num_entries = 0,
        .db_time_ns = 0,
        .f = f,
    };
    if (!ctx.ht) return;

    pgwt_visit_summaries(trace_dir, from_ns, to_ns, tq_summary_visitor, &ctx);

    double db_time_ms = (double)ctx.db_time_ns / 1e6;

    struct pgwt_query_row *rows = calloc(ctx.num_entries, sizeof(*rows));
    int nr = 0;

    for (int i = 0; i < QUERY_HT_SIZE; i++) {
        if (ctx.ht[i].count == 0) continue;

        struct pgwt_query_row *row = &rows[nr];
        row->query_id = ctx.ht[i].query_id;
        row->count    = ctx.ht[i].count;
        row->total_ms = (double)ctx.ht[i].total_ns / 1e6;
        row->avg_us   = ctx.ht[i].count > 0
                       ? (double)ctx.ht[i].total_ns / (double)ctx.ht[i].count / 1000.0 : 0;
        row->pct_db   = db_time_ms > 0 ? row->total_ms / db_time_ms * 100.0 : 0;
        row->top_wait_id = ctx.ht[i].top_wait_id;
        if (ctx.ht[i].top_wait_id == 0)
            snprintf(row->top_wait, sizeof(row->top_wait), "CPU*");
        else
            pgwt_event_full_name(ctx.ht[i].top_wait_id, row->top_wait,
                                  sizeof(row->top_wait));
        for (int c = 0; c < PGWT_NUM_CLASSES; c++)
            row->class_ms[c] = (double)ctx.ht[i].class_ns[c] / 1e6;
        nr++;
    }

    free(ctx.ht);
    qsort(rows, nr, sizeof(rows[0]), cmp_query_row_desc);

    out->rows       = rows;
    out->num_rows   = nr;
    out->db_time_ms = db_time_ms;
}

/* ── Heatmap from summaries ──────────────────────────────── */

struct hm_summary_ctx {
    uint64_t *grid;        /* [num_buckets * HISTOGRAM_BUCKETS] */
    uint64_t *times;       /* [num_buckets] */
    int      num_buckets;
    uint64_t from_ns;
    uint64_t bucket_ns;
    uint64_t max_count;
    uint64_t total_events;
    const struct pgwt_filter *f;
};

static int hm_summary_visitor(const struct pgwt_summary_accum *rec, void *arg)
{
    struct hm_summary_ctx *ctx = arg;
    uint64_t wall = rec->second_wall_ns;

    int bi = (int)((wall - ctx->from_ns) / ctx->bucket_ns);
    if (bi >= ctx->num_buckets) bi = ctx->num_buckets - 1;
    if (bi < 0) bi = 0;

    for (int e = 0; e < SUMMARY_MAX_EVENTS; e++) {
        const struct pgwt_summary_event *se = &rec->events[e];
        if (se->event_id == 0 && se->count == 0) continue;
        if (pgwt_is_idle_event(se->event_id)) continue;
        if (!summary_event_matches_filter(ctx->f, se->event_id)) continue;

        for (int b = 0; b < HISTOGRAM_BUCKETS; b++) {
            uint64_t v = se->histogram[b];
            if (v == 0) continue;

            uint64_t idx = (uint64_t)bi * HISTOGRAM_BUCKETS + b;
            ctx->grid[idx] += v;
            ctx->total_events += v;

            if (ctx->grid[idx] > ctx->max_count)
                ctx->max_count = ctx->grid[idx];
        }
    }
    return 0;
}

void pgwt_compute_heatmap_from_summaries(
    const char *trace_dir, uint64_t from_ns, uint64_t to_ns,
    const struct pgwt_filter *f, int num_buckets,
    struct pgwt_heatmap_result *out)
{
    memset(out, 0, sizeof(*out));

    uint64_t range_ns = to_ns - from_ns;
    if (range_ns == 0 || num_buckets <= 0) return;

    uint64_t bucket_ns = (range_ns + (uint64_t)num_buckets - 1) / (uint64_t)num_buckets;
    if (bucket_ns < 1000000000ULL)
        bucket_ns = 1000000000ULL;

    int actual_buckets = (int)((range_ns + bucket_ns - 1) / bucket_ns);
    uint64_t *grid = calloc((size_t)actual_buckets * HISTOGRAM_BUCKETS, sizeof(uint64_t));
    uint64_t *times = calloc(actual_buckets, sizeof(uint64_t));
    if (!grid || !times) { free(grid); free(times); return; }

    for (int i = 0; i < actual_buckets; i++)
        times[i] = from_ns + (uint64_t)i * bucket_ns;

    struct hm_summary_ctx ctx = {
        .grid = grid,
        .times = times,
        .num_buckets = actual_buckets,
        .from_ns = from_ns,
        .bucket_ns = bucket_ns,
        .max_count = 0,
        .total_events = 0,
        .f = f,
    };

    pgwt_visit_summaries(trace_dir, from_ns, to_ns, hm_summary_visitor, &ctx);

    out->grid         = grid;
    out->num_buckets  = actual_buckets;
    out->bucket_ns    = bucket_ns;
    out->times        = times;
    out->max_count    = ctx.max_count;
    out->total_events = ctx.total_events;
}
