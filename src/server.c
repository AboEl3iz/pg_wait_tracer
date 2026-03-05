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
#include "cmdline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

/* ── Utility ──────────────────────────────────────────────── */

/* Count lines in a file. Returns 0 if file doesn't exist. */
static int count_lines(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    int n = 0;
    char buf[4096];
    while (fgets(buf, sizeof(buf), fp))
        n++;
    fclose(fp);
    return n;
}

/* Next power-of-2 >= n, minimum 256 */
static int next_pow2(int n)
{
    if (n < 256) return 256;
    int p = 256;
    while (p < n) p <<= 1;
    return p;
}

/* ── Query text map ───────────────────────────────────────── */

struct qt_entry {
    uint64_t query_id;
    char    *text;        /* heap-allocated SQL text */
};

/* ── Backend metadata map ─────────────────────────────────── */

struct bm_entry {
    uint32_t pid;
    char     type[32];
    char     user[64];
    char     db[64];
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

    /* Query text map: query_id → SQL text (dynamic, power-of-2 sized) */
    struct qt_entry *qt_map;
    int  qt_capacity;
    int  qt_count;

    /* Backend metadata map: pid → type/user/db (dynamic, power-of-2 sized) */
    struct bm_entry *bm_map;
    int  bm_capacity;
    int  bm_count;
};

/* ── JSON request parsing (hand-written, no library) ──────── */

struct pgwt_request {
    int64_t  id;
    char     cmd[32];
    uint64_t from_ns;
    uint64_t to_ns;
    int      num_buckets;
    char     detail[16];       /* "events" for per-event AAS breakdown */
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

    /* query_id sent as string to avoid JS precision loss on uint64 */
    char qid_str[32] = {0};
    if (json_string(buf, "query_id", qid_str, sizeof(qid_str)) && qid_str[0])
        f->query_id = strtoull(qid_str, NULL, 10);
    else {
        uint64_t uv;
        if (json_uint64(buf, "query_id", &uv))
            f->query_id = uv;
    }
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
    json_string(line, "detail", req->detail, sizeof(req->detail));
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
    if (!srv->qt_map) return;
    int mask = srv->qt_capacity - 1;
    uint32_t idx = (uint32_t)(qt_hash64(query_id) & mask);
    for (int i = 0; i < srv->qt_capacity; i++) {
        struct qt_entry *e = &srv->qt_map[idx];
        if (e->query_id == 0) {
            e->query_id = query_id;
            e->text = strdup(text);
            srv->qt_count++;
            return;
        }
        if (e->query_id == query_id)
            return;  /* already present */
        idx = (idx + 1) & mask;
    }
}

/* Free all heap-allocated text strings and reset the map */
static void qt_map_clear(struct pgwt_server *srv)
{
    if (srv->qt_map) {
        for (int i = 0; i < srv->qt_capacity; i++)
            free(srv->qt_map[i].text);
        free(srv->qt_map);
        srv->qt_map = NULL;
    }
    srv->qt_capacity = 0;
    srv->qt_count = 0;
}

static const char *qt_map_lookup(const struct pgwt_server *srv, uint64_t query_id)
{
    if (query_id == 0 || !srv->qt_map)
        return NULL;
    int mask = srv->qt_capacity - 1;
    uint32_t idx = (uint32_t)(qt_hash64(query_id) & mask);
    for (int i = 0; i < srv->qt_capacity; i++) {
        const struct qt_entry *e = &srv->qt_map[idx];
        if (e->query_id == query_id)
            return e->text;
        if (e->query_id == 0)
            return NULL;
        idx = (idx + 1) & mask;
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

    /* Size the hash map to fit the file */
    int lines = count_lines(path);
    srv->qt_capacity = next_pow2(lines * 2);
    srv->qt_map = calloc(srv->qt_capacity, sizeof(struct qt_entry));
    if (!srv->qt_map) { srv->qt_capacity = 0; return; }

    FILE *fp = fopen(path, "r");
    if (!fp)
        return;

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;

    while ((line_len = getline(&line, &line_cap, fp)) > 0) {
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

        /* Extract text into dynamic buffer, handling JSON escapes */
        int text_cap = (line_len > 256) ? (int)line_len : 256;
        char *text = malloc(text_cap);
        if (!text) continue;
        int ti = 0;
        while (*tp && *tp != '"') {
            if (ti >= text_cap - 1) {
                text_cap *= 2;
                char *tmp = realloc(text, text_cap);
                if (!tmp) break;
                text = tmp;
            }
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
        free(text);
    }

    free(line);
    fclose(fp);
    if (srv->qt_count > 0)
        fprintf(stderr, "pgwt-server: loaded %d query texts\n", srv->qt_count);
}

/* ── Backend metadata map ─────────────────────────────────── */

static void bm_map_insert(struct pgwt_server *srv, uint32_t pid,
                            const char *type, const char *user, const char *db)
{
    if (!srv->bm_map) return;
    int mask = srv->bm_capacity - 1;
    uint32_t idx = pid & mask;
    for (int i = 0; i < srv->bm_capacity; i++) {
        struct bm_entry *e = &srv->bm_map[idx];
        if (e->pid == 0) {
            e->pid = pid;
            snprintf(e->type, sizeof(e->type), "%s", type);
            snprintf(e->user, sizeof(e->user), "%s", user ? user : "");
            snprintf(e->db, sizeof(e->db), "%s", db ? db : "");
            srv->bm_count++;
            return;
        }
        if (e->pid == pid) {
            /* Update with latest info */
            snprintf(e->type, sizeof(e->type), "%s", type);
            if (user && user[0]) snprintf(e->user, sizeof(e->user), "%s", user);
            if (db && db[0]) snprintf(e->db, sizeof(e->db), "%s", db);
            return;
        }
        idx = (idx + 1) & mask;
    }
}

static const struct bm_entry *bm_map_lookup(const struct pgwt_server *srv,
                                             uint32_t pid)
{
    if (pid == 0 || !srv->bm_map) return NULL;
    int mask = srv->bm_capacity - 1;
    uint32_t idx = pid & mask;
    for (int i = 0; i < srv->bm_capacity; i++) {
        const struct bm_entry *e = &srv->bm_map[idx];
        if (e->pid == pid) return e;
        if (e->pid == 0) return NULL;
        idx = (idx + 1) & mask;
    }
    return NULL;
}

/* Load backends.jsonl from trace dir.
 * Format: {"pid":<int>,"type":"<str>","user":"<str>","db":"<str>"} */
static void server_load_backends(struct pgwt_server *srv)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/backends.jsonl", srv->trace_dir);

    /* Size the hash map to fit the file */
    int lines = count_lines(path);
    srv->bm_capacity = next_pow2(lines * 2);
    srv->bm_map = calloc(srv->bm_capacity, sizeof(struct bm_entry));
    if (!srv->bm_map) { srv->bm_capacity = 0; return; }

    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        /* Parse "pid":<number> */
        const char *pp = strstr(line, "\"pid\":");
        if (!pp) continue;
        uint32_t pid = (uint32_t)strtoul(pp + 6, NULL, 10);
        if (pid == 0) continue;

        /* Parse "type":"<str>" */
        char type[32] = "unknown";
        const char *tp = strstr(line, "\"type\":\"");
        if (tp) {
            tp += 8;
            int i = 0;
            while (*tp && *tp != '"' && i < (int)sizeof(type) - 1)
                type[i++] = *tp++;
            type[i] = '\0';
        }

        /* Parse "user":"<str>" */
        char user[64] = "";
        const char *up = strstr(line, "\"user\":\"");
        if (up) {
            up += 8;
            int i = 0;
            while (*up && *up != '"' && i < (int)sizeof(user) - 1)
                user[i++] = *up++;
            user[i] = '\0';
        }

        /* Parse "db":"<str>" */
        char db[64] = "";
        const char *dp = strstr(line, "\"db\":\"");
        if (dp) {
            dp += 6;
            int i = 0;
            while (*dp && *dp != '"' && i < (int)sizeof(db) - 1)
                db[i++] = *dp++;
            db[i] = '\0';
        }

        bm_map_insert(srv, pid, type, user, db);
    }

    fclose(fp);
    if (srv->bm_count > 0)
        fprintf(stderr, "pgwt-server: loaded %d backend metadata entries\n",
                srv->bm_count);
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
    server_load_backends(srv);

    return 0;
}

/* ── Summary threshold ────────────────────────────────────── */

/* Use summary path for ranges >= 120s (instant response, ~300KB peak memory).
 * Raw events used for ranges < 120s (exact per-event resolution).
 * Force raw events when pid filter is active — summaries don't have
 * per-PID event/class breakdown. query_id is handled via v2 per-query data. */
static int should_use_summaries(struct pgwt_server *srv, struct pgwt_request *req)
{
    if (req->filter.pid != 0)
        return 0;
    uint64_t from = req->from_ns ? req->from_ns : srv->earliest_wall_ns;
    uint64_t to   = req->to_ns   ? req->to_ns   : srv->latest_wall_ns;
    if (to <= from) return 0;
    uint64_t range_ns = to - from;
    return (range_ns >= 120ULL * 1000000000ULL);
}

/* ── Command handlers ─────────────────────────────────────── */

static void handle_info(struct pgwt_server *srv, struct pgwt_request *req)
{
    /* Re-scan for new files */
    srv->num_files = pgwt_scan_trace_files(srv->trace_dir, srv->files, 256);

    /* Re-load query texts and backend metadata */
    qt_map_clear(srv);
    server_load_query_texts(srv);
    free(srv->bm_map);
    srv->bm_map = NULL;
    srv->bm_capacity = 0;
    srv->bm_count = 0;
    server_load_backends(srv);

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

    int detail_events = (strcmp(req->detail, "events") == 0);

    struct pgwt_aas_result aas;

    /* Event-detail mode always uses raw events (no summary path yet) */
    if (!detail_events && should_use_summaries(srv, req)) {
        pgwt_compute_aas_from_summaries(srv->trace_dir, from, to,
                                         &req->filter, num_buckets, &aas);
    } else {
        int count;
        struct pgwt_trace_event *events =
            server_load_events(srv, req->from_ns, req->to_ns, &count);
        pgwt_compute_aas(events, count, &req->filter, from, to, num_buckets,
                         detail_events, AAS_MAX_EVENT_SERIES, &aas);
        free(events);
    }

    /* Emit JSON */
    if (aas.num_event_series > 0) {
        /* Event-breakdown mode */
        int ns = aas.num_event_series;
        printf("{\"id\":%lld,\"breakdown\":\"events\",\"series\":[",
               (long long)req->id);
        for (int s = 0; s < ns; s++) {
            if (s > 0) putchar(',');
            printf("{\"event_id\":%u,\"name\":\"%s\"}",
                   aas.event_series[s].event_id, aas.event_series[s].name);
        }
        printf("],\"buckets\":[");
        for (int i = 0; i < aas.num_buckets; i++) {
            if (i > 0) putchar(',');
            printf("{\"t\":%llu,\"aas\":[",
                   (unsigned long long)aas.buckets[i].start_ns);
            for (int s = 0; s < ns; s++) {
                if (s > 0) putchar(',');
                printf("%.4f", aas.event_aas[i * ns + s]);
            }
            printf("]}");
        }
        printf("],\"max_aas\":%.4f,\"bucket_ns\":%llu}\n",
               aas.max_aas, (unsigned long long)aas.bucket_ns);
        free(aas.event_aas);
    } else {
        /* Class-breakdown mode (default) */
        printf("{\"id\":%lld,\"buckets\":[", (long long)req->id);
        for (int i = 0; i < aas.num_buckets; i++) {
            if (i > 0) putchar(',');
            printf("{\"t\":%llu", (unsigned long long)aas.buckets[i].start_ns);
            for (int c = 0; c < PGWT_NUM_CLASSES; c++)
                printf(",\"%s\":%.4f", pgwt_class_names[c],
                       aas.buckets[i].class_aas[c]);
            putchar('}');
        }
        printf("],\"max_aas\":%.4f,\"bucket_ns\":%llu}\n",
               aas.max_aas, (unsigned long long)aas.bucket_ns);
    }

    free(aas.buckets);
}

static void handle_time_model(struct pgwt_server *srv, struct pgwt_request *req)
{
    uint64_t from = req->from_ns ? req->from_ns : srv->earliest_wall_ns;
    uint64_t to   = req->to_ns   ? req->to_ns   : srv->latest_wall_ns;
    double wall_ms = (double)(to - from) / 1e6;

    struct pgwt_tm_result tm;

    if (should_use_summaries(srv, req)) {
        pgwt_compute_time_model_from_summaries(srv->trace_dir, from, to,
                                                &req->filter, wall_ms, &tm);
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
        pgwt_compute_top_events_from_summaries(srv->trace_dir, from, to,
                                                &req->filter, wall_ms, &res);
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
               "\"total_ms\":%.2f,\"avg_us\":%.2f,"
               "\"p50_us\":%.2f,\"p95_us\":%.2f,\"p99_us\":%.2f,"
               "\"max_us\":%.2f,"
               "\"pct\":%.2f,\"aas\":%.4f}",
               res.rows[i].event_id, res.rows[i].name,
               pgwt_class_name(res.rows[i].event_id),
               (unsigned long long)res.rows[i].count,
               res.rows[i].total_ms, res.rows[i].avg_us,
               res.rows[i].p50_us, res.rows[i].p95_us, res.rows[i].p99_us,
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

    /* Sessions + query_id: summaries lack per-session query data, use raw events */
    if (should_use_summaries(srv, req) && req->filter.query_id == 0) {
        pgwt_compute_top_sessions_from_summaries(srv->trace_dir, from, to,
                                                  &req->filter, wall_ms, &res);
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
               "\"top_wait\":\"%s\",\"top_wait_id\":%u",
               res.rows[i].pid, res.rows[i].db_time_ms,
               res.rows[i].cpu_pct, res.rows[i].wait_pct,
               res.rows[i].top_wait, res.rows[i].top_wait_id);
        const struct bm_entry *bm = bm_map_lookup(srv, res.rows[i].pid);
        if (bm) {
            printf(",\"type\":\"%s\"", bm->type);
            if (bm->user[0]) printf(",\"user\":\"%s\"", bm->user);
            if (bm->db[0]) printf(",\"db\":\"%s\"", bm->db);
        }
        putchar('}');
    }
    printf("]}\n");

    free(res.rows);
}

static void handle_top_queries(struct pgwt_server *srv, struct pgwt_request *req)
{
    uint64_t from = req->from_ns ? req->from_ns : srv->earliest_wall_ns;
    uint64_t to   = req->to_ns   ? req->to_ns   : srv->latest_wall_ns;
    double wall_ms = (double)(to - from) / 1e6;

    /* Force raw events when class/event filter active (need per-event breakdown) */
    int has_event_filter = (req->filter.class_name[0] != '\0' ||
                            req->filter.event_id != 0);

    struct pgwt_queries_result res;

    if (!has_event_filter && should_use_summaries(srv, req)) {
        pgwt_compute_top_queries_from_summaries(srv->trace_dir, from, to,
                                                 &req->filter, wall_ms, &res);
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
        printf("{\"query_id\":\"%llu\",\"count\":%llu,"
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

        /* Per-event breakdown (when available from raw events path) */
        if (res.rows[i].num_events > 0) {
            char ename[64];
            printf(",\"events\":[");
            for (int e = 0; e < res.rows[i].num_events; e++) {
                if (e > 0) putchar(',');
                uint32_t eid = res.rows[i].event_ids[e];
                if (eid == 0)
                    snprintf(ename, sizeof(ename), "CPU");
                else
                    pgwt_event_full_name(eid, ename, sizeof(ename));
                printf("{\"id\":%u,\"name\":\"%s\",\"ms\":%.2f}",
                       eid, ename, res.rows[i].event_ms[e]);
            }
            putchar(']');
        }

        putchar('}');
    }
    printf("],\"db_time_ms\":%.2f}\n", res.db_time_ms);

    free(res.rows);
}

static void handle_heatmap(struct pgwt_server *srv, struct pgwt_request *req)
{
    uint64_t from = req->from_ns ? req->from_ns : srv->earliest_wall_ns;
    uint64_t to   = req->to_ns   ? req->to_ns   : srv->latest_wall_ns;
    int num_buckets = req->num_buckets > 0 ? req->num_buckets : 200;

    struct pgwt_heatmap_result res;

    /* Heatmap + query_id: summaries lack per-query histograms, use raw events */
    if (should_use_summaries(srv, req) && req->filter.query_id == 0) {
        pgwt_compute_heatmap_from_summaries(srv->trace_dir, from, to,
                                             &req->filter, num_buckets, &res);
    } else {
        int count;
        struct pgwt_trace_event *events =
            server_load_events(srv, req->from_ns, req->to_ns, &count);
        pgwt_compute_heatmap(events, count, &req->filter,
                             from, to, num_buckets, &res);
        free(events);
    }

    /* Latency bucket labels */
    static const char *labels[HISTOGRAM_BUCKETS] = {
        "<1\\u00b5s", "1-2\\u00b5s", "2-4\\u00b5s", "4-8\\u00b5s",
        "8-16\\u00b5s", "16-32\\u00b5s", "32-64\\u00b5s", "64-128\\u00b5s",
        "128-256\\u00b5s", "256-512\\u00b5s", "0.5-1ms", "1-2ms",
        "2-4ms", "4-8ms", "8-16ms", "\\u226516ms"
    };

    printf("{\"id\":%lld,\"times\":[", (long long)req->id);
    for (int i = 0; i < res.num_buckets; i++) {
        if (i > 0) putchar(',');
        printf("%llu", (unsigned long long)res.times[i]);
    }

    printf("],\"labels\":[");
    for (int i = 0; i < HISTOGRAM_BUCKETS; i++) {
        if (i > 0) putchar(',');
        printf("\"%s\"", labels[i]);
    }

    /* Sparse cells: only emit non-zero [time_idx, latency_idx, count] */
    printf("],\"cells\":[");
    int first = 1;
    for (int t = 0; t < res.num_buckets; t++) {
        for (int l = 0; l < HISTOGRAM_BUCKETS; l++) {
            uint64_t v = res.grid[t * HISTOGRAM_BUCKETS + l];
            if (v == 0) continue;
            if (!first) putchar(',');
            printf("[%d,%d,%llu]", t, l, (unsigned long long)v);
            first = 0;
        }
    }

    printf("],\"max_count\":%llu,\"total_events\":%llu,"
           "\"bucket_ns\":%llu}\n",
           (unsigned long long)res.max_count,
           (unsigned long long)res.total_events,
           (unsigned long long)res.bucket_ns);

    free(res.grid);
    free(res.times);
}

/* ── Session Timeline ─────────────────────────────────────── */

#define TIMELINE_MAX_EVENTS 5000
#define TIMELINE_MAX_PIDS   256

static void handle_session_timeline(struct pgwt_server *srv, struct pgwt_request *req)
{
    int count;
    struct pgwt_trace_event *events =
        server_load_events(srv, req->from_ns, req->to_ns, &count);

    /* First pass: count matching events and collect unique PIDs */
    uint32_t pids[TIMELINE_MAX_PIDS];
    int num_pids = 0;
    int total_matching = 0;

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (!pgwt_filter_matches(&req->filter, ev))
            continue;
        if (pgwt_is_idle_event(ev->old_event))
            continue;
        total_matching++;

        int found = 0;
        for (int p = 0; p < num_pids; p++) {
            if (pids[p] == ev->pid) { found = 1; break; }
        }
        if (!found && num_pids < TIMELINE_MAX_PIDS)
            pids[num_pids++] = ev->pid;
    }

    /* Emit PIDs array */
    printf("{\"id\":%lld,\"pids\":[", (long long)req->id);
    for (int p = 0; p < num_pids; p++) {
        if (p > 0) putchar(',');
        printf("%u", pids[p]);
    }
    printf("],\"events\":[");

    /* Second pass: emit events (up to cap) */
    int nr = 0;
    int first = 1;
    for (int i = 0; i < count && nr < TIMELINE_MAX_EVENTS; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (!pgwt_filter_matches(&req->filter, ev))
            continue;
        if (pgwt_is_idle_event(ev->old_event))
            continue;

        if (!first) putchar(',');
        first = 0;

        char ename[64];
        if (ev->old_event == 0)
            snprintf(ename, sizeof(ename), "CPU");
        else
            pgwt_event_full_name(ev->old_event, ename, sizeof(ename));

        printf("{\"s\":%llu,\"d\":%llu,\"e\":%u,\"n\":\"%s\","
               "\"c\":%d,\"p\":%u,\"q\":\"%llu\"}",
               (unsigned long long)ev->timestamp_ns,
               (unsigned long long)ev->duration_ns,
               ev->old_event, ename,
               pgwt_wait_class_index(ev->old_event),
               ev->pid,
               (unsigned long long)ev->query_id);
        nr++;
    }

    printf("],\"truncated\":%s,\"total_count\":%d}\n",
           nr < total_matching ? "true" : "false",
           total_matching);

    free(events);
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
    else if (strcmp(req->cmd, "heatmap") == 0)
        handle_heatmap(srv, req);
    else if (strcmp(req->cmd, "session_timeline") == 0)
        handle_session_timeline(srv, req);
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
