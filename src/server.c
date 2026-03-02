/* server.c — pgwt-server: trace file replay server for web client.
 *
 * Reads JSON lines from stdin, dispatches commands, writes JSON to stdout.
 * Designed to run over SSH: ssh user@host pgwt-server /path/to/traces
 */
#include "pg_wait_tracer.h"
#include "event_reader.h"
#include "summary_reader.h"
#include "compute.h"
#include "wait_event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

/* ── Query text map ───────────────────────────────────────── */

#define QT_MAP_SIZE 8192

struct qt_entry {
    uint64_t query_id;
    char     text[256];   /* truncated SQL text */
};

/* ── Server state ─────────────────────────────────────────── */

struct pgwt_server {
    char trace_dir[512];
    int  num_cpus;
    struct pgwt_trace_file_entry files[256];
    int  num_files;
    uint64_t earliest_wall_ns;
    uint64_t latest_wall_ns;
    int  total_events;

    /* Query text map: query_id → SQL text */
    struct qt_entry qt_map[QT_MAP_SIZE];
    int  qt_count;
};

/* ── JSON request parsing (hand-written, no library) ──────── */

struct pgwt_request {
    int64_t  id;
    char     cmd[32];
    uint64_t from_ns;
    uint64_t to_ns;
    int      num_buckets;
    struct pgwt_filter filter;
};

/* Skip whitespace */
static const char *skip_ws(const char *p)
{
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

/* Extract a JSON string value after "key": */
static int json_string(const char *json, const char *key, char *out, int outsz)
{
    out[0] = '\0';
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    p = skip_ws(p);
    if (*p != ':') return 0;
    p = skip_ws(p + 1);
    if (*p != '"') return 0;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < outsz - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return 1;
}

/* Extract a JSON integer value after "key": */
static int json_int64(const char *json, const char *key, int64_t *out)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    p = skip_ws(p);
    if (*p != ':') return 0;
    p = skip_ws(p + 1);
    char *end;
    *out = strtoll(p, &end, 10);
    return end != p;
}

static int json_uint64(const char *json, const char *key, uint64_t *out)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    p = skip_ws(p);
    if (*p != ':') return 0;
    p = skip_ws(p + 1);
    char *end;
    *out = strtoull(p, &end, 10);
    return end != p;
}

static int json_int(const char *json, const char *key, int *out)
{
    int64_t v;
    if (!json_int64(json, key, &v)) return 0;
    *out = (int)v;
    return 1;
}

/* Parse filters sub-object */
static void parse_filters(const char *json, struct pgwt_filter *f)
{
    memset(f, 0, sizeof(*f));

    /* Find "filters" sub-object */
    const char *p = strstr(json, "\"filters\"");
    if (!p) return;
    p += 9;
    p = skip_ws(p);
    if (*p != ':') return;
    p = skip_ws(p + 1);
    if (*p != '{') return;

    /* Extract from the sub-object (find matching }) */
    const char *end = strchr(p, '}');
    if (!end) return;

    /* Copy sub-object to temporary buffer for parsing */
    int len = (int)(end - p + 1);
    char buf[512];
    if (len >= (int)sizeof(buf)) return;
    memcpy(buf, p, len);
    buf[len] = '\0';

    json_string(buf, "class", f->class_name, sizeof(f->class_name));

    int64_t v;
    if (json_int64(buf, "event_id", &v))
        f->event_id = (uint32_t)v;
    if (json_int64(buf, "pid", &v))
        f->pid = (uint32_t)v;

    uint64_t uv;
    if (json_uint64(buf, "query_id", &uv))
        f->query_id = uv;
}

static void parse_request(const char *line, struct pgwt_request *req)
{
    memset(req, 0, sizeof(*req));

    int64_t id;
    if (json_int64(line, "id", &id))
        req->id = id;

    json_string(line, "cmd", req->cmd, sizeof(req->cmd));
    json_uint64(line, "from", &req->from_ns);
    json_uint64(line, "to", &req->to_ns);
    json_int(line, "buckets", &req->num_buckets);
    parse_filters(line, &req->filter);
}

/* ── Query text loading ───────────────────────────────────── */

static uint64_t qt_hash64(uint64_t x)
{
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return x;
}

static void qt_map_insert(struct pgwt_server *srv, uint64_t query_id,
                           const char *text)
{
    uint32_t idx = (uint32_t)(qt_hash64(query_id) & (QT_MAP_SIZE - 1));
    for (int i = 0; i < QT_MAP_SIZE; i++) {
        struct qt_entry *e = &srv->qt_map[idx];
        if (e->query_id == 0) {
            e->query_id = query_id;
            snprintf(e->text, sizeof(e->text), "%s", text);
            srv->qt_count++;
            return;
        }
        if (e->query_id == query_id)
            return;  /* already present */
        idx = (idx + 1) & (QT_MAP_SIZE - 1);
    }
}

static const char *qt_map_lookup(const struct pgwt_server *srv, uint64_t query_id)
{
    if (query_id == 0)
        return NULL;
    uint32_t idx = (uint32_t)(qt_hash64(query_id) & (QT_MAP_SIZE - 1));
    for (int i = 0; i < QT_MAP_SIZE; i++) {
        const struct qt_entry *e = &srv->qt_map[idx];
        if (e->query_id == query_id)
            return e->text;
        if (e->query_id == 0)
            return NULL;
        idx = (idx + 1) & (QT_MAP_SIZE - 1);
    }
    return NULL;
}

/* Load query_texts.jsonl from trace dir into the hash map.
 * File format: {"q":<query_id>,"t":"<text>","ts":<wall_ns>} per line.
 * Simple hand-rolled parsing (no JSON library). */
static void server_load_query_texts(struct pgwt_server *srv)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/query_texts.jsonl", srv->trace_dir);

    FILE *fp = fopen(path, "r");
    if (!fp)
        return;

    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        /* Parse "q":<number> */
        const char *qp = strstr(line, "\"q\":");
        if (!qp) continue;
        qp += 4;
        char *end;
        uint64_t qid = strtoull(qp, &end, 10);
        if (end == qp || qid == 0) continue;

        /* Parse "t":"<text>" */
        const char *tp = strstr(line, "\"t\":\"");
        if (!tp) continue;
        tp += 5;

        /* Extract text, handling JSON escapes minimally */
        char text[256];
        int ti = 0;
        while (*tp && *tp != '"' && ti < (int)sizeof(text) - 1) {
            if (*tp == '\\' && tp[1]) {
                tp++;
                switch (*tp) {
                case 'n':  text[ti++] = ' '; break;  /* newlines → space */
                case 'r':  break;
                case 't':  text[ti++] = ' '; break;
                case '"':  text[ti++] = '"'; break;
                case '\\': text[ti++] = '\\'; break;
                default:
                    if (*tp == 'u' && tp[1] && tp[2] && tp[3] && tp[4]) {
                        text[ti++] = ' ';  /* \uXXXX → space */
                        tp += 4;
                    }
                    break;
                }
            } else {
                text[ti++] = *tp;
            }
            tp++;
        }
        text[ti] = '\0';

        qt_map_insert(srv, qid, text);
    }

    fclose(fp);
    if (srv->qt_count > 0)
        fprintf(stderr, "pgwt-server: loaded %d query texts\n", srv->qt_count);
}

/* ── Event loading ────────────────────────────────────────── */

/*
 * Load all events in [from_wall_ns, to_wall_ns] from trace files.
 * If from/to are 0, use full range. Timestamps in returned events
 * are converted to wall-clock nanoseconds.
 * Returns malloc'd array. Caller must free(). Sets *out_count.
 */
static struct pgwt_trace_event *
server_load_events(struct pgwt_server *srv,
                   uint64_t from_wall_ns, uint64_t to_wall_ns,
                   int *out_count)
{
    *out_count = 0;

    if (from_wall_ns == 0)
        from_wall_ns = srv->earliest_wall_ns;
    if (to_wall_ns == 0)
        to_wall_ns = srv->latest_wall_ns;

    /* Start with 64K events, grow as needed */
    int cap = 65536;
    struct pgwt_trace_event *events = malloc(cap * sizeof(*events));
    if (!events) return NULL;
    int total = 0;

    struct pgwt_trace_event block_buf[PGWT_BLOCK_EVENTS];

    for (int fi = 0; fi < srv->num_files; fi++) {
        struct pgwt_event_reader reader;
        if (pgwt_reader_open(&reader, srv->files[fi].path) != 0)
            continue;

        /* Convert wall-clock range to monotonic for this file */
        uint64_t from_mono = pgwt_reader_wall_to_mono(&reader, from_wall_ns);
        uint64_t to_mono   = pgwt_reader_wall_to_mono(&reader, to_wall_ns);

        int first_block = pgwt_reader_find_block(&reader, from_mono);

        for (int b = first_block; b < reader.num_blocks; b++) {
            /* Quick check: if block starts after our end time, stop */
            if (reader.block_index[b].timestamp_ns > to_mono)
                break;

            int n = pgwt_reader_decode_block(&reader, b, block_buf,
                                              PGWT_BLOCK_EVENTS);
            if (n <= 0)
                continue;

            for (int i = 0; i < n; i++) {
                uint64_t ts_mono = block_buf[i].timestamp_ns;
                if (ts_mono < from_mono)
                    continue;
                if (ts_mono > to_mono)
                    break; /* events are sorted */

                /* Grow buffer if needed */
                if (total >= cap) {
                    cap *= 2;
                    struct pgwt_trace_event *tmp = realloc(events,
                        cap * sizeof(*events));
                    if (!tmp) {
                        pgwt_reader_close(&reader);
                        *out_count = total;
                        return events;
                    }
                    events = tmp;
                }

                events[total] = block_buf[i];
                /* Convert timestamp to wall-clock */
                events[total].timestamp_ns =
                    pgwt_reader_mono_to_wall(&reader, ts_mono);
                total++;
            }
        }

        pgwt_reader_close(&reader);
    }

    *out_count = total;
    return events;
}

/* ── Server init ──────────────────────────────────────────── */

static int server_init(struct pgwt_server *srv, const char *trace_dir)
{
    memset(srv, 0, sizeof(*srv));
    snprintf(srv->trace_dir, sizeof(srv->trace_dir), "%s", trace_dir);

    srv->num_cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (srv->num_cpus <= 0)
        srv->num_cpus = 1;

    pgwt_init_event_names(18); /* Default to PG18 tables */

    srv->num_files = pgwt_scan_trace_files(trace_dir, srv->files, 256);
    if (srv->num_files <= 0) {
        fprintf(stderr, "pgwt-server: no trace files found in %s\n", trace_dir);
        return -1;
    }

    /* Determine time range by opening first and last files */
    srv->earliest_wall_ns = 0;
    srv->latest_wall_ns   = 0;
    srv->total_events     = 0;

    for (int i = 0; i < srv->num_files; i++) {
        struct pgwt_event_reader reader;
        if (pgwt_reader_open(&reader, srv->files[i].path) != 0)
            continue;

        if (reader.num_blocks > 0) {
            uint64_t first_mono = reader.block_index[0].timestamp_ns;
            uint64_t last_mono  = reader.block_index[reader.num_blocks - 1].timestamp_ns;

            uint64_t first_wall = pgwt_reader_mono_to_wall(&reader, first_mono);
            uint64_t last_wall  = pgwt_reader_mono_to_wall(&reader, last_mono);

            if (srv->earliest_wall_ns == 0 || first_wall < srv->earliest_wall_ns)
                srv->earliest_wall_ns = first_wall;
            if (last_wall > srv->latest_wall_ns)
                srv->latest_wall_ns = last_wall;

            /* Approximate event count from block count */
            srv->total_events += reader.num_blocks * PGWT_BLOCK_EVENTS;
        }

        pgwt_reader_close(&reader);
    }

    fprintf(stderr, "pgwt-server: %d trace files, %d CPUs, ~%d events\n",
            srv->num_files, srv->num_cpus, srv->total_events);

    server_load_query_texts(srv);

    return 0;
}

/* ── Summary threshold ────────────────────────────────────── */

#define SUMMARY_THRESHOLD_NS  (120ULL * 1000000000ULL)  /* 120 seconds */

/* Check if summaries should be used for this request's time range.
 * Returns 1 if summaries are preferred, 0 if raw events should be used. */
static int should_use_summaries(struct pgwt_server *srv, struct pgwt_request *req)
{
    uint64_t from = req->from_ns ? req->from_ns : srv->earliest_wall_ns;
    uint64_t to   = req->to_ns   ? req->to_ns   : srv->latest_wall_ns;
    if (to <= from) return 0;
    return (to - from) >= SUMMARY_THRESHOLD_NS;
}

/* ── Command handlers ─────────────────────────────────────── */

static void handle_info(struct pgwt_server *srv, struct pgwt_request *req)
{
    /* Re-scan for new files */
    srv->num_files = pgwt_scan_trace_files(srv->trace_dir, srv->files, 256);

    /* Re-load query texts (picks up newly captured queries) */
    memset(srv->qt_map, 0, sizeof(srv->qt_map));
    srv->qt_count = 0;
    server_load_query_texts(srv);

    /* Re-compute time range */
    srv->earliest_wall_ns = 0;
    srv->latest_wall_ns   = 0;
    srv->total_events     = 0;

    for (int i = 0; i < srv->num_files; i++) {
        struct pgwt_event_reader reader;
        if (pgwt_reader_open(&reader, srv->files[i].path) != 0)
            continue;
        if (reader.num_blocks > 0) {
            uint64_t first_wall = pgwt_reader_mono_to_wall(&reader,
                reader.block_index[0].timestamp_ns);
            uint64_t last_wall = pgwt_reader_mono_to_wall(&reader,
                reader.block_index[reader.num_blocks - 1].timestamp_ns);
            if (srv->earliest_wall_ns == 0 || first_wall < srv->earliest_wall_ns)
                srv->earliest_wall_ns = first_wall;
            if (last_wall > srv->latest_wall_ns)
                srv->latest_wall_ns = last_wall;
            srv->total_events += reader.num_blocks * PGWT_BLOCK_EVENTS;
        }
        pgwt_reader_close(&reader);
    }

    printf("{\"id\":%lld,\"from_ns\":%llu,\"to_ns\":%llu,"
           "\"num_events\":%d,\"num_cpus\":%d}\n",
           (long long)req->id,
           (unsigned long long)srv->earliest_wall_ns,
           (unsigned long long)srv->latest_wall_ns,
           srv->total_events, srv->num_cpus);
}

static void handle_aas(struct pgwt_server *srv, struct pgwt_request *req)
{
    int num_buckets = req->num_buckets > 0 ? req->num_buckets : 120;

    uint64_t from = req->from_ns ? req->from_ns : srv->earliest_wall_ns;
    uint64_t to   = req->to_ns   ? req->to_ns   : srv->latest_wall_ns;

    struct pgwt_aas_result aas;

    if (should_use_summaries(srv, req)) {
        struct pgwt_summary_accum *records;
        int rcount = pgwt_load_summaries(srv->trace_dir, from, to, &records);
        if (rcount > 0) {
            pgwt_compute_aas_from_summaries(records, rcount, &req->filter,
                                             from, to, num_buckets, &aas);
            free(records);
        } else {
            memset(&aas, 0, sizeof(aas));
            aas.bucket_ns = 1000000000ULL;
            free(records);
        }
    } else {
        int count;
        struct pgwt_trace_event *events =
            server_load_events(srv, req->from_ns, req->to_ns, &count);
        pgwt_compute_aas(events, count, &req->filter, from, to, num_buckets, &aas);
        free(events);
    }

    /* Emit JSON */
    printf("{\"id\":%lld,\"buckets\":[", (long long)req->id);
    for (int i = 0; i < aas.num_buckets; i++) {
        if (i > 0) putchar(',');
        printf("{\"t\":%llu", (unsigned long long)aas.buckets[i].start_ns);
        for (int c = 0; c < PGWT_NUM_CLASSES; c++)
            printf(",\"%s\":%.4f", pgwt_class_names[c], aas.buckets[i].class_aas[c]);
        putchar('}');
    }
    printf("],\"max_aas\":%.4f,\"bucket_ns\":%llu}\n",
           aas.max_aas, (unsigned long long)aas.bucket_ns);

    free(aas.buckets);
}

static void handle_time_model(struct pgwt_server *srv, struct pgwt_request *req)
{
    uint64_t from = req->from_ns ? req->from_ns : srv->earliest_wall_ns;
    uint64_t to   = req->to_ns   ? req->to_ns   : srv->latest_wall_ns;
    double wall_ms = (double)(to - from) / 1e6;

    struct pgwt_tm_result tm;

    if (should_use_summaries(srv, req)) {
        struct pgwt_summary_accum *records;
        int rcount = pgwt_load_summaries(srv->trace_dir, from, to, &records);
        pgwt_compute_time_model_from_summaries(
            records, rcount > 0 ? rcount : 0, &req->filter, wall_ms, &tm);
        free(records);
    } else {
        int count;
        struct pgwt_trace_event *events =
            server_load_events(srv, req->from_ns, req->to_ns, &count);
        pgwt_compute_time_model(events, count, &req->filter, wall_ms, &tm);
        free(events);
    }

    printf("{\"id\":%lld,\"rows\":[", (long long)req->id);
    for (int i = 0; i < tm.num_rows; i++) {
        if (i > 0) putchar(',');
        printf("{\"name\":\"%s\",\"ms\":%.2f,\"pct\":%.2f,"
               "\"aas\":%.4f,\"indent\":%d}",
               tm.rows[i].name, tm.rows[i].time_ms,
               tm.rows[i].pct_db_time, tm.rows[i].aas,
               tm.rows[i].indent);
    }
    printf("],\"db_time_ms\":%.2f,\"idle_time_ms\":%.2f,"
           "\"aas\":%.4f,\"wall_ms\":%.2f}\n",
           tm.db_time_ms, tm.idle_time_ms, tm.aas, tm.wall_ms);

    free(tm.rows);
}

static void handle_top_events(struct pgwt_server *srv, struct pgwt_request *req)
{
    uint64_t from = req->from_ns ? req->from_ns : srv->earliest_wall_ns;
    uint64_t to   = req->to_ns   ? req->to_ns   : srv->latest_wall_ns;
    double wall_ms = (double)(to - from) / 1e6;

    struct pgwt_events_result res;

    if (should_use_summaries(srv, req)) {
        struct pgwt_summary_accum *records;
        int rcount = pgwt_load_summaries(srv->trace_dir, from, to, &records);
        pgwt_compute_top_events_from_summaries(
            records, rcount > 0 ? rcount : 0, &req->filter, wall_ms, &res);
        free(records);
    } else {
        int count;
        struct pgwt_trace_event *events =
            server_load_events(srv, req->from_ns, req->to_ns, &count);
        pgwt_compute_top_events(events, count, &req->filter, wall_ms, &res);
        free(events);
    }

    printf("{\"id\":%lld,\"rows\":[", (long long)req->id);
    for (int i = 0; i < res.num_rows; i++) {
        if (i > 0) putchar(',');
        printf("{\"event_id\":%u,\"name\":\"%s\","
               "\"class\":\"%s\",\"count\":%llu,"
               "\"total_ms\":%.2f,\"avg_us\":%.2f,\"max_us\":%.2f,"
               "\"pct\":%.2f,\"aas\":%.4f}",
               res.rows[i].event_id, res.rows[i].name,
               pgwt_class_name(res.rows[i].event_id),
               (unsigned long long)res.rows[i].count,
               res.rows[i].total_ms, res.rows[i].avg_us,
               res.rows[i].max_us, res.rows[i].pct_db,
               res.rows[i].aas);
    }
    printf("],\"db_time_ms\":%.2f}\n", res.db_time_ms);

    free(res.rows);
}

static void handle_top_sessions(struct pgwt_server *srv, struct pgwt_request *req)
{
    uint64_t from = req->from_ns ? req->from_ns : srv->earliest_wall_ns;
    uint64_t to   = req->to_ns   ? req->to_ns   : srv->latest_wall_ns;
    double wall_ms = (double)(to - from) / 1e6;

    struct pgwt_sessions_result res;

    if (should_use_summaries(srv, req)) {
        struct pgwt_summary_accum *records;
        int rcount = pgwt_load_summaries(srv->trace_dir, from, to, &records);
        pgwt_compute_top_sessions_from_summaries(
            records, rcount > 0 ? rcount : 0, &req->filter, wall_ms, &res);
        free(records);
    } else {
        int count;
        struct pgwt_trace_event *events =
            server_load_events(srv, req->from_ns, req->to_ns, &count);
        pgwt_compute_top_sessions(events, count, &req->filter, wall_ms, &res);
        free(events);
    }

    printf("{\"id\":%lld,\"rows\":[", (long long)req->id);
    for (int i = 0; i < res.num_rows; i++) {
        if (i > 0) putchar(',');
        printf("{\"pid\":%u,\"db_time_ms\":%.2f,"
               "\"cpu_pct\":%.1f,\"wait_pct\":%.1f,"
               "\"top_wait\":\"%s\",\"top_wait_id\":%u}",
               res.rows[i].pid, res.rows[i].db_time_ms,
               res.rows[i].cpu_pct, res.rows[i].wait_pct,
               res.rows[i].top_wait, res.rows[i].top_wait_id);
    }
    printf("]}\n");

    free(res.rows);
}

static void handle_top_queries(struct pgwt_server *srv, struct pgwt_request *req)
{
    uint64_t from = req->from_ns ? req->from_ns : srv->earliest_wall_ns;
    uint64_t to   = req->to_ns   ? req->to_ns   : srv->latest_wall_ns;
    double wall_ms = (double)(to - from) / 1e6;

    struct pgwt_queries_result res;

    if (should_use_summaries(srv, req)) {
        struct pgwt_summary_accum *records;
        int rcount = pgwt_load_summaries(srv->trace_dir, from, to, &records);
        pgwt_compute_top_queries_from_summaries(
            records, rcount > 0 ? rcount : 0, &req->filter, wall_ms, &res);
        free(records);
    } else {
        int count;
        struct pgwt_trace_event *events =
            server_load_events(srv, req->from_ns, req->to_ns, &count);
        pgwt_compute_top_queries(events, count, &req->filter, wall_ms, &res);
        free(events);
    }

    printf("{\"id\":%lld,\"rows\":[", (long long)req->id);
    for (int i = 0; i < res.num_rows; i++) {
        if (i > 0) putchar(',');
        printf("{\"query_id\":%llu,\"count\":%llu,"
               "\"total_ms\":%.2f,\"avg_us\":%.2f,"
               "\"pct\":%.2f,\"top_wait\":\"%s\",\"top_wait_id\":%u",
               (unsigned long long)res.rows[i].query_id,
               (unsigned long long)res.rows[i].count,
               res.rows[i].total_ms, res.rows[i].avg_us,
               res.rows[i].pct_db, res.rows[i].top_wait,
               res.rows[i].top_wait_id);

        /* Add query text if available */
        const char *qt = qt_map_lookup(srv, res.rows[i].query_id);
        if (qt) {
            printf(",\"text\":\"");
            for (const char *c = qt; *c; c++) {
                switch (*c) {
                case '"':  fputs("\\\"", stdout); break;
                case '\\': fputs("\\\\", stdout); break;
                case '\n': fputs("\\n", stdout);  break;
                case '\r': fputs("\\r", stdout);  break;
                case '\t': fputs("\\t", stdout);  break;
                default:
                    if ((unsigned char)*c < 0x20)
                        printf("\\u%04x", (unsigned char)*c);
                    else
                        putchar(*c);
                }
            }
            putchar('"');
        }

        /* Per-class time breakdown */
        printf(",\"classes\":[");
        for (int c = 0; c < 11; c++) {
            if (c > 0) putchar(',');
            printf("%.2f", res.rows[i].class_ms[c]);
        }
        putchar(']');

        putchar('}');
    }
    printf("],\"db_time_ms\":%.2f}\n", res.db_time_ms);

    free(res.rows);
}

/* ── Dispatch ─────────────────────────────────────────────── */

static void dispatch(struct pgwt_server *srv, struct pgwt_request *req)
{
    if (strcmp(req->cmd, "info") == 0)
        handle_info(srv, req);
    else if (strcmp(req->cmd, "aas") == 0)
        handle_aas(srv, req);
    else if (strcmp(req->cmd, "time_model") == 0)
        handle_time_model(srv, req);
    else if (strcmp(req->cmd, "top_events") == 0)
        handle_top_events(srv, req);
    else if (strcmp(req->cmd, "top_sessions") == 0)
        handle_top_sessions(srv, req);
    else if (strcmp(req->cmd, "top_queries") == 0)
        handle_top_queries(srv, req);
    else
        printf("{\"id\":%lld,\"error\":\"unknown command: %s\"}\n",
               (long long)req->id, req->cmd);
}

/* ── Main ─────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: pgwt-server <trace-dir>\n");
        return 1;
    }

    /* Line-buffered stdout is critical for SSH pipe */
    setvbuf(stdout, NULL, _IOLBF, 0);

    struct pgwt_server srv;
    if (server_init(&srv, argv[1]) != 0)
        return 1;

    char line[8192];
    while (fgets(line, sizeof(line), stdin)) {
        struct pgwt_request req;
        parse_request(line, &req);

        if (req.cmd[0] == '\0') {
            printf("{\"id\":%lld,\"error\":\"missing cmd\"}\n",
                   (long long)req.id);
            continue;
        }

        dispatch(&srv, &req);
    }

    return 0;
}
