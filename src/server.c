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
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>

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

/* Next power-of-2 >= n, minimum 256, clamped to 1<<30 */
static int next_pow2(int n)
{
    if (n < 256) return 256;
    if (n > (1 << 30)) return 1 << 30;
    int p = 256;
    while (p < n) p <<= 1;
    return p;
}

/* ── cJSON helpers ────────────────────────────────────────── */

/* Add uint64 as raw JSON number (avoids double precision loss for ns timestamps) */
static void cjson_add_uint64(cJSON *obj, const char *name, uint64_t val)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)val);
    cJSON_AddRawToObject(obj, name, buf);
}

/* Add int64 as raw JSON number (preserves sign for query_id etc.) */
static void cjson_add_int64(cJSON *obj, const char *name, int64_t val)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)val);
    cJSON_AddRawToObject(obj, name, buf);
}

/* Create a raw uint64 item for arrays */
static cJSON *cjson_create_uint64(uint64_t val)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)val);
    return cJSON_CreateRaw(buf);
}

/* Print cJSON to stdout as a single line and clean up */
static void emit_json(cJSON *root)
{
    char *str = cJSON_PrintUnformatted(root);
    if (str) {
        puts(str);
        cJSON_free(str);
    }
    cJSON_Delete(root);
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

/* Per-file event cache — for immutable .trace.lz4 files only */
struct file_cache_entry {
    char     path[512];
    struct pgwt_trace_event *events;
    int      count;
    int      immutable;
    uint64_t first_wall_ns;  /* earliest event timestamp (for range skip) */
    uint64_t last_wall_ns;   /* latest event timestamp */
};

struct pgwt_server {
    char trace_dir[512];
    int  num_cpus;
    struct pgwt_trace_file_entry files[256];
    int  num_files;
    uint64_t earliest_wall_ns;
    uint64_t latest_wall_ns;
    int64_t total_events;

    /* Per-file event cache */
    struct file_cache_entry cache[256];
    int  cache_count;

    /* Query text map: query_id → SQL text (dynamic, power-of-2 sized) */
    struct qt_entry *qt_map;
    int  qt_capacity;
    int  qt_count;

    /* Backend metadata map: pid → type/user/db (dynamic, power-of-2 sized) */
    struct bm_entry *bm_map;
    int  bm_capacity;
    int  bm_count;
};

/* ── JSON request parsing (cJSON) ─────────────────────────── */

struct pgwt_request {
    int64_t  id;
    char     cmd[32];
    uint64_t from_ns;
    uint64_t to_ns;
    int      num_buckets;
    char     detail[16];       /* "events" for per-event AAS breakdown */
    struct pgwt_filter filter;
};

static void parse_filters(cJSON *root, struct pgwt_filter *f)
{
    memset(f, 0, sizeof(*f));

    cJSON *filters = cJSON_GetObjectItem(root, "filters");
    if (!filters) return;

    cJSON *cls = cJSON_GetObjectItem(filters, "class");
    if (cJSON_IsString(cls) && cls->valuestring)
        snprintf(f->class_name, sizeof(f->class_name), "%s", cls->valuestring);

    cJSON *eid = cJSON_GetObjectItem(filters, "event_id");
    if (cJSON_IsNumber(eid))
        f->event_id = (uint32_t)eid->valuedouble;

    cJSON *pid = cJSON_GetObjectItem(filters, "pid");
    if (cJSON_IsNumber(pid))
        f->pid = (uint32_t)pid->valuedouble;

    /* query_id sent as string to avoid JS precision loss on uint64 */
    cJSON *qid = cJSON_GetObjectItem(filters, "query_id");
    if (cJSON_IsString(qid) && qid->valuestring && qid->valuestring[0])
        f->query_id = (uint64_t)strtoll(qid->valuestring, NULL, 10);
    else if (cJSON_IsNumber(qid))
        f->query_id = (uint64_t)(int64_t)qid->valuedouble;
}

static void parse_request(const char *line, struct pgwt_request *req)
{
    memset(req, 0, sizeof(*req));

    cJSON *root = cJSON_Parse(line);
    if (!root) return;

    cJSON *id = cJSON_GetObjectItem(root, "id");
    if (cJSON_IsNumber(id))
        req->id = (int64_t)id->valuedouble;

    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (cJSON_IsString(cmd) && cmd->valuestring)
        snprintf(req->cmd, sizeof(req->cmd), "%s", cmd->valuestring);

    cJSON *from = cJSON_GetObjectItem(root, "from");
    if (cJSON_IsNumber(from))
        req->from_ns = (uint64_t)from->valuedouble;

    cJSON *to = cJSON_GetObjectItem(root, "to");
    if (cJSON_IsNumber(to))
        req->to_ns = (uint64_t)to->valuedouble;

    cJSON *buckets = cJSON_GetObjectItem(root, "buckets");
    if (cJSON_IsNumber(buckets))
        req->num_buckets = (int)buckets->valuedouble;

    cJSON *detail = cJSON_GetObjectItem(root, "detail");
    if (cJSON_IsString(detail) && detail->valuestring)
        snprintf(req->detail, sizeof(req->detail), "%s", detail->valuestring);

    parse_filters(root, &req->filter);

    cJSON_Delete(root);
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
 * File format: {"q":<query_id>,"t":"<text>","ts":<wall_ns>} per line. */
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
        cJSON *root = cJSON_Parse(line);
        if (!root) continue;

        cJSON *q = cJSON_GetObjectItem(root, "q");
        cJSON *t = cJSON_GetObjectItem(root, "t");

        uint64_t qid = 0;
        if (cJSON_IsString(q) && q->valuestring)
            qid = (uint64_t)strtoll(q->valuestring, NULL, 10);
        else if (cJSON_IsNumber(q))
            qid = (uint64_t)(int64_t)q->valuedouble;  /* legacy numeric format */
        if (qid == 0 || !cJSON_IsString(t) || !t->valuestring) {
            cJSON_Delete(root);
            continue;
        }

        qt_map_insert(srv, qid, t->valuestring);
        cJSON_Delete(root);
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
        cJSON *root = cJSON_Parse(line);
        if (!root) continue;

        cJSON *pid_item = cJSON_GetObjectItem(root, "pid");
        if (!cJSON_IsNumber(pid_item)) { cJSON_Delete(root); continue; }
        uint32_t pid = (uint32_t)pid_item->valuedouble;
        if (pid == 0) { cJSON_Delete(root); continue; }

        cJSON *type_item = cJSON_GetObjectItem(root, "type");
        cJSON *user_item = cJSON_GetObjectItem(root, "user");
        cJSON *db_item   = cJSON_GetObjectItem(root, "db");

        const char *type = cJSON_IsString(type_item) ? type_item->valuestring : "unknown";
        const char *user = cJSON_IsString(user_item) ? user_item->valuestring : "";
        const char *db   = cJSON_IsString(db_item)   ? db_item->valuestring   : "";

        bm_map_insert(srv, pid, type, user, db);
        cJSON_Delete(root);
    }

    fclose(fp);
    if (srv->bm_count > 0)
        fprintf(stderr, "pgwt-server: loaded %d backend metadata entries\n",
                srv->bm_count);
}

/* ── Event caching + on-demand loading ────────────────────── */

/* Check if a file is the current (mutable) trace */
static int is_current_trace(const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return strcmp(base, "current.trace") == 0;
}

/* Get cached events for an immutable file. Reads once, caches forever.
 * Returns NULL for current.trace (handled separately). */
static struct file_cache_entry *
get_cached_immutable(struct pgwt_server *srv, const char *path)
{
    /* Look for existing entry */
    for (int i = 0; i < srv->cache_count; i++) {
        if (strcmp(srv->cache[i].path, path) == 0)
            return &srv->cache[i];
    }

    /* New entry — read entire file once */
    if (srv->cache_count >= 256)
        return NULL;

    struct file_cache_entry *ce = &srv->cache[srv->cache_count++];
    memset(ce, 0, sizeof(*ce));
    snprintf(ce->path, sizeof(ce->path), "%s", path);
    ce->immutable = 1;
    ce->count = 0;

    struct pgwt_event_reader reader;
    if (pgwt_reader_open(&reader, path) != 0)
        return ce;

    int cap = 16384;
    ce->events = malloc(cap * sizeof(*ce->events));
    struct pgwt_trace_event block_buf[PGWT_BLOCK_EVENTS];

    /* Store time range for this file */
    if (reader.num_blocks > 0) {
        ce->first_wall_ns = pgwt_reader_mono_to_wall(&reader,
            reader.block_index[0].timestamp_ns);
        ce->last_wall_ns = pgwt_reader_mono_to_wall(&reader,
            reader.block_index[reader.num_blocks - 1].timestamp_ns);
    }

    for (int b = 0; b < reader.num_blocks; b++) {
        int n = pgwt_reader_decode_block(&reader, b, block_buf, PGWT_BLOCK_EVENTS);
        for (int i = 0; i < n; i++) {
            if (ce->count >= cap) {
                cap *= 2;
                struct pgwt_trace_event *tmp = realloc(ce->events, cap * sizeof(*tmp));
                if (!tmp) break;
                ce->events = tmp;
            }
            ce->events[ce->count] = block_buf[i];
            ce->events[ce->count].timestamp_ns =
                pgwt_reader_mono_to_wall(&reader, block_buf[i].timestamp_ns);
            ce->count++;
        }
    }
    pgwt_reader_close(&reader);
    return ce;
}

/* Read events from current.trace for a time range — on demand, no caching.
 * Opens file, seeks to the right blocks via block index, decodes only
 * the blocks that overlap [from_wall_ns, to_wall_ns].
 * Appends to events array at *total, growing if needed. */
static void load_current_trace_range(const char *path,
                                      uint64_t from_wall_ns, uint64_t to_wall_ns,
                                      struct pgwt_trace_event **events,
                                      int *total, int *cap)
{
    struct pgwt_event_reader reader;
    if (pgwt_reader_open(&reader, path) != 0)
        return;

    uint64_t from_mono = pgwt_reader_wall_to_mono(&reader, from_wall_ns);
    uint64_t to_mono   = pgwt_reader_wall_to_mono(&reader, to_wall_ns);

    int first_block = pgwt_reader_find_block(&reader, from_mono);
    struct pgwt_trace_event block_buf[PGWT_BLOCK_EVENTS];

    for (int b = first_block; b < reader.num_blocks; b++) {
        if (reader.block_index[b].timestamp_ns > to_mono)
            break;

        int n = pgwt_reader_decode_block(&reader, b, block_buf, PGWT_BLOCK_EVENTS);
        for (int i = 0; i < n; i++) {
            uint64_t ts_mono = block_buf[i].timestamp_ns;
            if (ts_mono < from_mono) continue;
            if (ts_mono > to_mono) break;

            if (*total >= *cap) {
                *cap *= 2;
                struct pgwt_trace_event *tmp = realloc(*events, *cap * sizeof(**events));
                if (!tmp) { pgwt_reader_close(&reader); return; }
                *events = tmp;
            }

            (*events)[*total] = block_buf[i];
            (*events)[*total].timestamp_ns =
                pgwt_reader_mono_to_wall(&reader, ts_mono);
            (*total)++;
        }
    }
    pgwt_reader_close(&reader);
}

/* Get latest wall-clock timestamp from current.trace — quick, O(1).
 * Opens file, reads last block header, converts, closes. */
static uint64_t get_current_trace_latest(const char *path)
{
    struct pgwt_event_reader reader;
    if (pgwt_reader_open(&reader, path) != 0)
        return 0;

    uint64_t latest = 0;
    if (reader.num_blocks > 0)
        latest = pgwt_reader_mono_to_wall(&reader,
            reader.block_index[reader.num_blocks - 1].timestamp_ns);

    pgwt_reader_close(&reader);
    return latest;
}

/* ── Event loading ────────────────────────────────────────── */

/*
 * Load events in [from_wall_ns, to_wall_ns] from trace files.
 * Immutable .trace.lz4: from cache (read once per session).
 * current.trace: on-demand block reads (no caching, no memory growth).
 * Returns malloc'd array. Caller must free(). Sets *out_count.
 */
static struct pgwt_trace_event *
server_load_events(struct pgwt_server *srv,
                   uint64_t from_wall_ns, uint64_t to_wall_ns,
                   int *out_count)
{
    *out_count = 0;

    /* Quick rescan for new files (directory listing only, no file opens) */
    srv->num_files = pgwt_scan_trace_files(srv->trace_dir, srv->files, 256);

    if (from_wall_ns == 0)
        from_wall_ns = srv->earliest_wall_ns;
    if (to_wall_ns == 0)
        to_wall_ns = srv->latest_wall_ns;

    int cap = 65536;
    struct pgwt_trace_event *events = malloc(cap * sizeof(*events));
    if (!events) return NULL;
    int total = 0;

    for (int fi = 0; fi < srv->num_files; fi++) {
        const char *path = srv->files[fi].path;

        if (is_current_trace(path)) {
            /* On-demand: read only blocks in [from, to] */
            load_current_trace_range(path, from_wall_ns, to_wall_ns,
                                     &events, &total, &cap);
        } else {
            /* Immutable: use cache */
            struct file_cache_entry *ce = get_cached_immutable(srv, path);
            if (!ce || !ce->events) continue;

            /* Skip file entirely if its time range doesn't overlap */
            if (ce->last_wall_ns > 0 && ce->first_wall_ns > to_wall_ns)
                continue;
            if (ce->last_wall_ns > 0 && ce->last_wall_ns < from_wall_ns)
                continue;

            /* Filter cached events by time range */
            for (int i = 0; i < ce->count; i++) {
                uint64_t ts = ce->events[i].timestamp_ns;
                if (ts < from_wall_ns) continue;
                if (ts > to_wall_ns) break;

                if (total >= cap) {
                    cap *= 2;
                    struct pgwt_trace_event *tmp = realloc(events, cap * sizeof(*tmp));
                    if (!tmp) { *out_count = total; return events; }
                    events = tmp;
                }
                events[total++] = ce->events[i];
            }
        }
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

    /* Try loading dynamic event names from sidecar file.
     * Written by the daemon when it queries pg_wait_events. */
    if (pgwt_load_names_json(trace_dir) == 0)
        fprintf(stderr, "pgwt-server: loaded event names from wait_event_names.json\n");

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

    fprintf(stderr, "pgwt-server: %d trace files, %d CPUs, ~%lld events\n",
            srv->num_files, srv->num_cpus, (long long)srv->total_events);

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
    /* Refresh: rescan directory, reload metadata, update latest_wall_ns. */
    srv->num_files = pgwt_scan_trace_files(srv->trace_dir, srv->files, 256);

    /* Reload query texts and backend metadata (daemon appends new entries
     * as it discovers backends/queries). These files are small — fast. */
    qt_map_clear(srv);
    server_load_query_texts(srv);
    free(srv->bm_map);
    srv->bm_map = NULL;
    srv->bm_capacity = 0;
    srv->bm_count = 0;
    server_load_backends(srv);

    /* Update latest_wall_ns from current.trace (one quick open+close) */
    for (int i = 0; i < srv->num_files; i++) {
        if (is_current_trace(srv->files[i].path)) {
            uint64_t t = get_current_trace_latest(srv->files[i].path);
            if (t > srv->latest_wall_ns)
                srv->latest_wall_ns = t;
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);
    cjson_add_uint64(root, "from_ns", srv->earliest_wall_ns);
    cjson_add_uint64(root, "to_ns", srv->latest_wall_ns);
    cJSON_AddNumberToObject(root, "num_events", (double)srv->total_events);
    cJSON_AddNumberToObject(root, "num_cpus", srv->num_cpus);

    /* Server wall clock — client uses this as "now" for relative time ranges */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    cjson_add_uint64(root, "now_ns", now_ns);

    emit_json(root);
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

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);

    if (aas.num_event_series > 0) {
        /* Event-breakdown mode */
        int ns = aas.num_event_series;
        cJSON_AddStringToObject(root, "breakdown", "events");

        cJSON *series = cJSON_AddArrayToObject(root, "series");
        for (int s = 0; s < ns; s++) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "event_id", aas.event_series[s].event_id);
            cJSON_AddStringToObject(item, "name", aas.event_series[s].name);
            cJSON_AddItemToArray(series, item);
        }

        cJSON *buckets_arr = cJSON_AddArrayToObject(root, "buckets");
        for (int i = 0; i < aas.num_buckets; i++) {
            cJSON *b = cJSON_CreateObject();
            cjson_add_uint64(b, "t", aas.buckets[i].start_ns);
            cJSON *aas_arr = cJSON_AddArrayToObject(b, "aas");
            for (int s = 0; s < ns; s++)
                cJSON_AddItemToArray(aas_arr,
                    cJSON_CreateNumber(aas.event_aas[i * ns + s]));
            cJSON_AddItemToArray(buckets_arr, b);
        }

        free(aas.event_aas);
    } else {
        /* Class-breakdown mode (default) */
        cJSON *buckets_arr = cJSON_AddArrayToObject(root, "buckets");
        for (int i = 0; i < aas.num_buckets; i++) {
            cJSON *b = cJSON_CreateObject();
            cjson_add_uint64(b, "t", aas.buckets[i].start_ns);
            for (int c = 0; c < PGWT_NUM_CLASSES; c++)
                cJSON_AddNumberToObject(b, pgwt_class_names[c],
                                        aas.buckets[i].class_aas[c]);
            cJSON_AddItemToArray(buckets_arr, b);
        }
    }

    cJSON_AddNumberToObject(root, "max_aas", aas.max_aas);
    cjson_add_uint64(root, "bucket_ns", aas.bucket_ns);
    emit_json(root);

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

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);

    cJSON *rows = cJSON_AddArrayToObject(root, "rows");
    for (int i = 0; i < tm.num_rows; i++) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "name", tm.rows[i].name);
        cJSON_AddNumberToObject(r, "ms", tm.rows[i].time_ms);
        cJSON_AddNumberToObject(r, "pct", tm.rows[i].pct_db_time);
        cJSON_AddNumberToObject(r, "aas", tm.rows[i].aas);
        cJSON_AddNumberToObject(r, "indent", tm.rows[i].indent);
        cJSON_AddItemToArray(rows, r);
    }

    cJSON_AddNumberToObject(root, "db_time_ms", tm.db_time_ms);
    cJSON_AddNumberToObject(root, "idle_time_ms", tm.idle_time_ms);
    cJSON_AddNumberToObject(root, "aas", tm.aas);
    cJSON_AddNumberToObject(root, "wall_ms", tm.wall_ms);
    emit_json(root);

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

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);

    cJSON *rows = cJSON_AddArrayToObject(root, "rows");
    for (int i = 0; i < res.num_rows; i++) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "event_id", res.rows[i].event_id);
        cJSON_AddStringToObject(r, "name", res.rows[i].name);
        cJSON_AddStringToObject(r, "class",
                                pgwt_class_name(res.rows[i].event_id));
        cjson_add_uint64(r, "count", res.rows[i].count);
        cJSON_AddNumberToObject(r, "total_ms", res.rows[i].total_ms);
        cJSON_AddNumberToObject(r, "avg_us", res.rows[i].avg_us);
        cJSON_AddNumberToObject(r, "p50_us", res.rows[i].p50_us);
        cJSON_AddNumberToObject(r, "p95_us", res.rows[i].p95_us);
        cJSON_AddNumberToObject(r, "p99_us", res.rows[i].p99_us);
        cJSON_AddNumberToObject(r, "max_us", res.rows[i].max_us);
        cJSON_AddNumberToObject(r, "pct", res.rows[i].pct_db);
        cJSON_AddNumberToObject(r, "aas", res.rows[i].aas);
        cJSON_AddItemToArray(rows, r);
    }

    cJSON_AddNumberToObject(root, "db_time_ms", res.db_time_ms);
    emit_json(root);

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

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);

    cJSON *rows = cJSON_AddArrayToObject(root, "rows");
    for (int i = 0; i < res.num_rows; i++) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "pid", res.rows[i].pid);
        cJSON_AddNumberToObject(r, "db_time_ms", res.rows[i].db_time_ms);
        cJSON_AddNumberToObject(r, "cpu_pct", res.rows[i].cpu_pct);
        cJSON_AddNumberToObject(r, "wait_pct", res.rows[i].wait_pct);
        cJSON_AddStringToObject(r, "top_wait", res.rows[i].top_wait);
        cJSON_AddNumberToObject(r, "top_wait_id", res.rows[i].top_wait_id);
        const struct bm_entry *bm = bm_map_lookup(srv, res.rows[i].pid);
        if (bm) {
            cJSON_AddStringToObject(r, "type", bm->type);
            if (bm->user[0]) cJSON_AddStringToObject(r, "user", bm->user);
            if (bm->db[0]) cJSON_AddStringToObject(r, "db", bm->db);
        }
        cJSON_AddItemToArray(rows, r);
    }

    emit_json(root);

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

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);

    cJSON *rows = cJSON_AddArrayToObject(root, "rows");
    for (int i = 0; i < res.num_rows; i++) {
        cJSON *r = cJSON_CreateObject();

        /* query_id as string to avoid JS precision loss */
        char qid_str[32];
        snprintf(qid_str, sizeof(qid_str), "%lld",
                 (long long)res.rows[i].query_id);
        cJSON_AddStringToObject(r, "query_id", qid_str);

        cjson_add_uint64(r, "count", res.rows[i].count);
        cJSON_AddNumberToObject(r, "total_ms", res.rows[i].total_ms);
        cJSON_AddNumberToObject(r, "avg_us", res.rows[i].avg_us);
        cJSON_AddNumberToObject(r, "pct", res.rows[i].pct_db);
        cJSON_AddStringToObject(r, "top_wait", res.rows[i].top_wait);
        cJSON_AddNumberToObject(r, "top_wait_id", res.rows[i].top_wait_id);

        /* Add query text if available */
        const char *qt = qt_map_lookup(srv, res.rows[i].query_id);
        if (qt)
            cJSON_AddStringToObject(r, "text", qt);

        /* Per-class time breakdown */
        cJSON *classes = cJSON_AddArrayToObject(r, "classes");
        for (int c = 0; c < 11; c++)
            cJSON_AddItemToArray(classes,
                cJSON_CreateNumber(res.rows[i].class_ms[c]));

        /* Per-event breakdown (when available from raw events path) */
        if (res.rows[i].num_events > 0) {
            char ename[64];
            cJSON *events_arr = cJSON_AddArrayToObject(r, "events");
            for (int e = 0; e < res.rows[i].num_events; e++) {
                uint32_t eid = res.rows[i].event_ids[e];
                if (eid == 0)
                    snprintf(ename, sizeof(ename), "CPU");
                else
                    pgwt_event_full_name(eid, ename, sizeof(ename));
                cJSON *ev = cJSON_CreateObject();
                cJSON_AddNumberToObject(ev, "id", eid);
                cJSON_AddStringToObject(ev, "name", ename);
                cJSON_AddNumberToObject(ev, "ms", res.rows[i].event_ms[e]);
                cJSON_AddItemToArray(events_arr, ev);
            }
        }

        cJSON_AddItemToArray(rows, r);
    }

    cJSON_AddNumberToObject(root, "db_time_ms", res.db_time_ms);
    emit_json(root);

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

    /* Latency bucket labels (UTF-8: µ = \xc2\xb5, ≥ = \xe2\x89\xa5) */
    static const char *labels[HISTOGRAM_BUCKETS] = {
        "<1\xc2\xb5s", "1-2\xc2\xb5s", "2-4\xc2\xb5s", "4-8\xc2\xb5s",
        "8-16\xc2\xb5s", "16-32\xc2\xb5s", "32-64\xc2\xb5s", "64-128\xc2\xb5s",
        "128-256\xc2\xb5s", "256-512\xc2\xb5s", "0.5-1ms", "1-2ms",
        "2-4ms", "4-8ms", "8-16ms", "\xe2\x89\xa5" "16ms"
    };

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);

    cJSON *times_arr = cJSON_AddArrayToObject(root, "times");
    for (int i = 0; i < res.num_buckets; i++)
        cJSON_AddItemToArray(times_arr, cjson_create_uint64(res.times[i]));

    cJSON *labels_arr = cJSON_AddArrayToObject(root, "labels");
    for (int i = 0; i < HISTOGRAM_BUCKETS; i++)
        cJSON_AddItemToArray(labels_arr, cJSON_CreateString(labels[i]));

    /* Sparse cells: only emit non-zero [time_idx, latency_idx, count] */
    cJSON *cells_arr = cJSON_AddArrayToObject(root, "cells");
    for (int t = 0; t < res.num_buckets; t++) {
        for (int l = 0; l < HISTOGRAM_BUCKETS; l++) {
            uint64_t v = res.grid[t * HISTOGRAM_BUCKETS + l];
            if (v == 0) continue;
            cJSON *cell = cJSON_CreateArray();
            cJSON_AddItemToArray(cell, cJSON_CreateNumber(t));
            cJSON_AddItemToArray(cell, cJSON_CreateNumber(l));
            cJSON_AddItemToArray(cell, cjson_create_uint64(v));
            cJSON_AddItemToArray(cells_arr, cell);
        }
    }

    cjson_add_uint64(root, "max_count", res.max_count);
    cjson_add_uint64(root, "total_events", res.total_events);
    cjson_add_uint64(root, "bucket_ns", res.bucket_ns);
    emit_json(root);

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

        /* Collect unique PIDs (linear scan is fine for ≤256 PIDs) */
        int found = 0;
        for (int p = 0; p < num_pids; p++) {
            if (pids[p] == ev->pid) { found = 1; break; }
        }
        if (!found && num_pids < TIMELINE_MAX_PIDS)
            pids[num_pids++] = ev->pid;
    }

    /* Compute coalesce gap: if too many events, merge adjacent same-class
       events within same PID when gap between them is < coalesce_ns.
       This reduces bar count while preserving the visual pattern. */
    uint64_t range_ns = (req->to_ns && req->from_ns) ?
                        (req->to_ns - req->from_ns) : 0;
    if (range_ns == 0 && count > 0) {
        /* Estimate range from actual events */
        uint64_t min_ts = UINT64_MAX, max_ts = 0;
        for (int i = 0; i < count; i++) {
            if (events[i].timestamp_ns < min_ts) min_ts = events[i].timestamp_ns;
            uint64_t end = events[i].timestamp_ns + events[i].duration_ns;
            if (end > max_ts) max_ts = end;
        }
        range_ns = max_ts - min_ts;
    }
    /* Coalesce gap = range / (2 * max_events), so we get at most ~2*max visible bars */
    uint64_t coalesce_ns = total_matching > TIMELINE_MAX_EVENTS ?
                           range_ns / (TIMELINE_MAX_EVENTS * 2) : 0;

    /* Build JSON response */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);

    cJSON *pids_arr = cJSON_AddArrayToObject(root, "pids");
    for (int p = 0; p < num_pids; p++)
        cJSON_AddItemToArray(pids_arr, cJSON_CreateNumber(pids[p]));

    cJSON *events_arr = cJSON_AddArrayToObject(root, "events");

    /* Second pass: emit events with coalescing */
    struct {
        uint32_t pid;
        uint64_t start_ns;
        uint64_t end_ns;
        uint32_t event_id;
        int      class_idx;
        uint64_t query_id;
        int      active;
    } pending[TIMELINE_MAX_PIDS];
    memset(pending, 0, sizeof(pending));

    int nr = 0;

    /* Flush a pending bar to the events array */
    #define FLUSH_PENDING(pidx) do { \
        if (pending[pidx].active && nr < TIMELINE_MAX_EVENTS) { \
            char _ename[64]; \
            if (pending[pidx].event_id == 0) \
                snprintf(_ename, sizeof(_ename), "CPU"); \
            else \
                pgwt_event_full_name(pending[pidx].event_id, _ename, sizeof(_ename)); \
            cJSON *_ev = cJSON_CreateObject(); \
            cjson_add_uint64(_ev, "s", pending[pidx].start_ns); \
            cjson_add_uint64(_ev, "d", pending[pidx].end_ns - pending[pidx].start_ns); \
            cJSON_AddNumberToObject(_ev, "e", pending[pidx].event_id); \
            cJSON_AddStringToObject(_ev, "n", _ename); \
            cJSON_AddNumberToObject(_ev, "c", pending[pidx].class_idx); \
            cJSON_AddNumberToObject(_ev, "p", pending[pidx].pid); \
            char _qbuf[32]; \
            snprintf(_qbuf, sizeof(_qbuf), "%llu", (unsigned long long)pending[pidx].query_id); \
            cJSON_AddStringToObject(_ev, "q", _qbuf); \
            cJSON_AddItemToArray(events_arr, _ev); \
            nr++; \
            pending[pidx].active = 0; \
        } \
    } while(0)

    /* Build PID→index hash table for O(1) lookup in second pass */
    int pid_idx_ht[1024];  /* hash table: slot → pids[] index, -1 = empty */
    memset(pid_idx_ht, -1, sizeof(pid_idx_ht));
    for (int p = 0; p < num_pids; p++) {
        uint32_t slot = (pids[p] * 0x45d9f3b) & 1023;
        while (pid_idx_ht[slot] >= 0)
            slot = (slot + 1) & 1023;
        pid_idx_ht[slot] = p;
    }

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (!pgwt_filter_matches(&req->filter, ev))
            continue;
        if (pgwt_is_idle_event(ev->old_event))
            continue;

        /* O(1) PID index lookup */
        int pidx = -1;
        {
            uint32_t slot = (ev->pid * 0x45d9f3b) & 1023;
            while (pid_idx_ht[slot] >= 0) {
                if (pids[pid_idx_ht[slot]] == ev->pid) {
                    pidx = pid_idx_ht[slot];
                    break;
                }
                slot = (slot + 1) & 1023;
            }
        }
        if (pidx < 0) continue;

        int cls = pgwt_wait_class_index(ev->old_event);

        if (pending[pidx].active) {
            /* Can we merge? Same class + gap within threshold */
            uint64_t ev_start = ev->timestamp_ns - ev->duration_ns;
            uint64_t gap = (ev_start > pending[pidx].end_ns) ?
                           (ev_start - pending[pidx].end_ns) : 0;
            if (pending[pidx].class_idx == cls && gap <= coalesce_ns) {
                /* Extend the pending bar */
                if (ev->timestamp_ns > pending[pidx].end_ns)
                    pending[pidx].end_ns = ev->timestamp_ns;
                continue;
            }
            /* Different class or too big a gap — flush */
            FLUSH_PENDING(pidx);
            if (nr >= TIMELINE_MAX_EVENTS) break;
        }

        /* Start new pending bar */
        pending[pidx].active = 1;
        pending[pidx].pid = ev->pid;
        pending[pidx].start_ns = ev->timestamp_ns - ev->duration_ns;
        pending[pidx].end_ns = ev->timestamp_ns;
        pending[pidx].event_id = ev->old_event;
        pending[pidx].class_idx = cls;
        pending[pidx].query_id = ev->query_id;
    }

    /* Flush remaining */
    for (int p = 0; p < num_pids && nr < TIMELINE_MAX_EVENTS; p++)
        FLUSH_PENDING(p);

    #undef FLUSH_PENDING

    cJSON_AddBoolToObject(root, "truncated", nr < total_matching);
    cJSON_AddNumberToObject(root, "total_count", total_matching);
    emit_json(root);

    free(events);
}

/* ── Dispatch ─────────────────────────────────────────────── */

static void handle_transitions(struct pgwt_server *srv, struct pgwt_request *req)
{
    int count;
    struct pgwt_trace_event *events =
        server_load_events(srv, req->from_ns, req->to_ns, &count);

    int max_rows = req->num_buckets > 0 ? req->num_buckets : 50;
    struct pgwt_transitions_result res;
    pgwt_compute_transitions(events, count, &req->filter, max_rows, &res);
    free(events);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);
    cjson_add_uint64(root, "total", res.total_transitions);

    /* Compute per-node total time directly from ALL events (not truncated rows).
     * Each event's old_event spent duration_ns — sum by event. */
    struct { uint32_t event_id; char name[64]; double total_ms; } node_acc[256];
    int num_nodes = 0;
    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (pgwt_is_idle_event(ev->old_event) || PGWT_IS_MARKER(ev->old_event))
            continue;
        if (ev->old_event == 0) continue;  /* skip CPU=0 with no time */
        /* Find or insert */
        int found = -1;
        for (int j = 0; j < num_nodes; j++) {
            if (node_acc[j].event_id == ev->old_event) {
                found = j; break;
            }
        }
        if (found >= 0) {
            node_acc[found].total_ms += ev->duration_ns / 1e6;
        } else if (num_nodes < 256) {
            node_acc[num_nodes].event_id = ev->old_event;
            pgwt_event_full_name(ev->old_event, node_acc[num_nodes].name,
                                 sizeof(node_acc[num_nodes].name));
            node_acc[num_nodes].total_ms = ev->duration_ns / 1e6;
            num_nodes++;
        }
    }
    /* Also ensure CPU* node exists with its total time */
    {
        double cpu_ms = 0;
        for (int i = 0; i < count; i++) {
            if (events[i].old_event == 0 && events[i].duration_ns > 0)
                cpu_ms += events[i].duration_ns / 1e6;
        }
        if (cpu_ms > 0) {
            int found = -1;
            for (int j = 0; j < num_nodes; j++) {
                if (node_acc[j].event_id == 0) { found = j; break; }
            }
            if (found >= 0) {
                node_acc[found].total_ms = cpu_ms;
            } else if (num_nodes < 256) {
                node_acc[num_nodes].event_id = 0;
                snprintf(node_acc[num_nodes].name, 64, "CPU*");
                node_acc[num_nodes].total_ms = cpu_ms;
                num_nodes++;
            }
        }
    }

    /* Nodes array */
    cJSON *nodes = cJSON_AddArrayToObject(root, "nodes");
    for (int i = 0; i < num_nodes; i++) {
        cJSON *node = cJSON_CreateObject();
        cJSON_AddStringToObject(node, "name", node_acc[i].name);
        cJSON_AddNumberToObject(node, "total_ms", node_acc[i].total_ms);
        /* Extract wait class for coloring */
        const char *colon = strchr(node_acc[i].name, ':');
        if (colon) {
            char cls[32];
            int len = colon - node_acc[i].name;
            if (len > 31) len = 31;
            memcpy(cls, node_acc[i].name, len);
            cls[len] = '\0';
            cJSON_AddStringToObject(node, "class", cls);
        } else {
            cJSON_AddStringToObject(node, "class", "cpu");
        }
        cJSON_AddItemToArray(nodes, node);
    }

    /* Links array */
    cJSON *links = cJSON_AddArrayToObject(root, "links");
    for (int i = 0; i < res.num_rows; i++) {
        cJSON *link = cJSON_CreateObject();
        cJSON_AddStringToObject(link, "source", res.rows[i].from_name);
        cJSON_AddStringToObject(link, "target", res.rows[i].to_name);
        cJSON_AddNumberToObject(link, "value", (double)res.rows[i].count);
        cJSON_AddNumberToObject(link, "duration_ms",
                                (double)res.rows[i].total_ns / 1e6);
        cJSON_AddItemToArray(links, link);
    }

    free(res.rows);
    emit_json(root);
}

static void handle_fingerprints(struct pgwt_server *srv, struct pgwt_request *req)
{
    int count;
    struct pgwt_trace_event *events =
        server_load_events(srv, req->from_ns, req->to_ns, &count);

    struct pgwt_fingerprint_result res;
    pgwt_compute_fingerprints(events, count, &req->filter, &res);
    free(events);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);

    cJSON *rows = cJSON_AddArrayToObject(root, "rows");
    for (int i = 0; i < res.num_rows; i++) {
        cJSON *r = cJSON_CreateObject();
        cjson_add_int64(r, "query_id", (int64_t)res.rows[i].query_id);
        cJSON_AddNumberToObject(r, "transitions",
                                (double)res.rows[i].total_transitions);
        cJSON_AddStringToObject(r, "signature", res.rows[i].signature);

        /* Class distribution */
        cJSON *pct = cJSON_CreateObject();
        for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
            if (res.rows[i].class_pct[c] >= 0.1)
                cJSON_AddNumberToObject(pct, pgwt_class_names[c],
                                        res.rows[i].class_pct[c]);
        }
        cJSON_AddItemToObject(r, "class_pct", pct);

        /* Top transition */
        char from_name[64], to_name[64];
        pgwt_event_full_name(res.rows[i].top_from, from_name, sizeof(from_name));
        pgwt_event_full_name(res.rows[i].top_to, to_name, sizeof(to_name));
        cJSON_AddStringToObject(r, "top_from", from_name);
        cJSON_AddStringToObject(r, "top_to", to_name);

        cJSON_AddItemToArray(rows, r);
    }

    free(res.rows);
    emit_json(root);
}

static void handle_lock_chains(struct pgwt_server *srv, struct pgwt_request *req)
{
    int count;
    struct pgwt_trace_event *events =
        server_load_events(srv, req->from_ns, req->to_ns, &count);

    struct pgwt_lock_chains_result res;
    pgwt_compute_lock_chains(events, count, &req->filter, 50, &res);
    free(events);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);

    cJSON *chains = cJSON_AddArrayToObject(root, "chains");
    for (int i = 0; i < res.num_links; i++) {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddNumberToObject(c, "waiter", res.links[i].waiter_pid);
        cJSON_AddNumberToObject(c, "blocker", res.links[i].blocker_pid);
        cJSON_AddStringToObject(c, "lock", res.links[i].lock_name);
        cJSON_AddNumberToObject(c, "wait_ms",
                                (double)res.links[i].wait_ns / 1e6);
        cjson_add_uint64(c, "timestamp_ns", res.links[i].timestamp_ns);
        cJSON_AddItemToArray(chains, c);
    }

    free(res.links);
    emit_json(root);
}

static void handle_interference(struct pgwt_server *srv, struct pgwt_request *req)
{
    int count;
    struct pgwt_trace_event *events =
        server_load_events(srv, req->from_ns, req->to_ns, &count);

    struct pgwt_interference_result res;
    pgwt_compute_interference(events, count, &req->filter, 30, &res);
    free(events);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);

    cJSON *rows = cJSON_AddArrayToObject(root, "rows");
    for (int i = 0; i < res.num_rows; i++) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "pid_a", res.rows[i].pid_a);
        cJSON_AddNumberToObject(r, "pid_b", res.rows[i].pid_b);
        cJSON_AddNumberToObject(r, "score", res.rows[i].score);
        cJSON_AddStringToObject(r, "top_event", res.rows[i].top_event_name);
        cJSON_AddNumberToObject(r, "overlap_ms",
                                (double)res.rows[i].overlap_ns / 1e6);
        cJSON_AddItemToArray(rows, r);
    }

    free(res.rows);
    emit_json(root);
}

static void handle_concurrency(struct pgwt_server *srv, struct pgwt_request *req)
{
    int count;
    struct pgwt_trace_event *events =
        server_load_events(srv, req->from_ns, req->to_ns, &count);

    uint64_t from = req->from_ns ? req->from_ns : srv->earliest_wall_ns;
    uint64_t to   = req->to_ns   ? req->to_ns   : srv->latest_wall_ns;
    int num_buckets = req->num_buckets > 0 ? req->num_buckets : 60;

    struct pgwt_concurrency_result res;
    pgwt_compute_concurrency(events, count, &req->filter,
                              from, to, num_buckets,
                              10000000ULL,  /* 10ms burst window */
                              4,            /* 4+ sessions = burst */
                              &res);
    free(events);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)req->id);
    cjson_add_uint64(root, "bucket_ns", res.bucket_ns);

    /* Peak concurrency per bucket */
    cJSON *peaks = cJSON_AddArrayToObject(root, "peaks");
    for (int i = 0; i < res.num_buckets; i++) {
        cJSON *p = cJSON_CreateObject();
        uint64_t t_ns = from + (uint64_t)i * res.bucket_ns;
        cjson_add_uint64(p, "t", t_ns);
        cJSON_AddNumberToObject(p, "t_ms", (double)(t_ns / 1000000ULL));
        cJSON_AddNumberToObject(p, "max", res.peak_sessions[i]);
        if (res.peak_event[i]) {
            char ename[64];
            pgwt_event_full_name(res.peak_event[i], ename, sizeof(ename));
            cJSON_AddStringToObject(p, "event", ename);
        }
        cJSON_AddItemToArray(peaks, p);
    }

    /* Bursts */
    cJSON *bursts_arr = cJSON_AddArrayToObject(root, "bursts");
    int nb = res.num_bursts < 20 ? res.num_bursts : 20;
    for (int i = 0; i < nb; i++) {
        cJSON *b = cJSON_CreateObject();
        cjson_add_uint64(b, "timestamp_ns", res.bursts[i].timestamp_ns);
        cJSON_AddNumberToObject(b, "timestamp_ms",
                                (double)(res.bursts[i].timestamp_ns / 1000000ULL));
        cJSON_AddStringToObject(b, "event", res.bursts[i].event_name);
        cJSON_AddNumberToObject(b, "sessions", res.bursts[i].num_sessions);

        cJSON *pids = cJSON_AddArrayToObject(b, "pids");
        for (int j = 0; j < res.bursts[i].num_pids; j++)
            cJSON_AddItemToArray(pids, cJSON_CreateNumber(res.bursts[i].pids[j]));

        cJSON_AddItemToArray(bursts_arr, b);
    }

    free(res.peak_sessions);
    free(res.peak_event);
    free(res.bursts);
    emit_json(root);
}

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
    else if (strcmp(req->cmd, "transitions") == 0)
        handle_transitions(srv, req);
    else if (strcmp(req->cmd, "fingerprints") == 0)
        handle_fingerprints(srv, req);
    else if (strcmp(req->cmd, "concurrency") == 0)
        handle_concurrency(srv, req);
    else if (strcmp(req->cmd, "lock_chains") == 0)
        handle_lock_chains(srv, req);
    else if (strcmp(req->cmd, "interference") == 0)
        handle_interference(srv, req);
    else {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "id", (double)req->id);
        char errmsg[64];
        snprintf(errmsg, sizeof(errmsg), "unknown command: %s", req->cmd);
        cJSON_AddStringToObject(root, "error", errmsg);
        emit_json(root);
    }
}

/* ── Main ─────────────────────────────────────────────────── */

/* ── Dump mode: text summary to stdout ──────────────────── */

static void dump_summary(struct pgwt_server *srv)
{
    uint64_t from = srv->earliest_wall_ns;
    uint64_t to = srv->latest_wall_ns;
    double wall_ms = (double)(to - from) / 1e6;
    struct pgwt_filter filt = {0};

    /* Load all events */
    int count;
    struct pgwt_trace_event *events = server_load_events(srv, from, to, &count);

    printf("════════════════════════════════════════════════════════════════════════════════\n");
    printf("pgwt-server — Summary    Events: %d    Duration: %.1fs\n", count, wall_ms / 1000.0);
    printf("════════════════════════════════════════════════════════════════════════════════\n");

    /* Time Model */
    struct pgwt_tm_result tm;
    pgwt_compute_time_model(events, count, &filt, wall_ms, &tm);

    printf("\n  === Time Model ===\n\n");
    printf("  AAS: %.2f    DB Time: %.1f ms    Idle: %.1f ms\n\n",
           tm.aas, tm.db_time_ms, tm.idle_time_ms);

    for (int i = 0; i < tm.num_rows; i++) {
        struct pgwt_tm_row *r = &tm.rows[i];
        if (r->indent == 0)
            printf("  %-32s %10.1f ms  %6.1f%%\n", r->name, r->time_ms, r->pct_db_time);
        else if (r->indent == 1)
            printf("    %-30s %10.1f ms  %6.1f%%\n", r->name, r->time_ms, r->pct_db_time);
        else
            printf("      %-28s %10.1f ms  %6.1f%%\n", r->name, r->time_ms, r->pct_db_time);
    }
    free(tm.rows);

    /* Top Events */
    struct pgwt_events_result ev;
    pgwt_compute_top_events(events, count, &filt, wall_ms, &ev);

    printf("\n  === Top Events ===\n\n");
    printf("  %-28s %10s %12s %10s %8s\n", "Wait Event", "Waits", "Total (ms)", "Avg (us)", "% DB");
    printf("  ─────────────────────────────────────────────────────────────────────────\n");

    int n = ev.num_rows < 15 ? ev.num_rows : 15;
    for (int i = 0; i < n; i++) {
        struct pgwt_event_row *r = &ev.rows[i];
        printf("  %-28s %10llu %12.1f %10.1f %7.1f%%\n",
               r->name, (unsigned long long)r->count, r->total_ms, r->avg_us, r->pct_db);
    }
    free(ev.rows);

    /* Top Sessions */
    struct pgwt_sessions_result sess;
    pgwt_compute_top_sessions(events, count, &filt, wall_ms, &sess);

    printf("\n  === Top Sessions ===\n\n");
    printf("  %7s %12s %7s %7s  %-20s\n",
           "PID", "DB Time(ms)", "CPU%", "Wait%", "Top Wait");
    printf("  ─────────────────────────────────────────────────────────────────────────\n");

    n = sess.num_rows < 15 ? sess.num_rows : 15;
    for (int i = 0; i < n; i++) {
        struct pgwt_session_row *r = &sess.rows[i];
        printf("  %7d %12.1f %6.1f%% %6.1f%%  %-20s\n",
               r->pid, r->db_time_ms, r->cpu_pct, r->wait_pct, r->top_wait);
    }
    free(sess.rows);

    /* Top Queries */
    struct pgwt_queries_result qry;
    pgwt_compute_top_queries(events, count, &filt, wall_ms, &qry);

    printf("\n  === Top Queries ===\n\n");
    printf("  %20s %10s %12s %8s  %-20s\n",
           "query_id", "Waits", "Total (ms)", "% DB", "Top Wait");
    printf("  ─────────────────────────────────────────────────────────────────────────\n");

    n = qry.num_rows < 15 ? qry.num_rows : 15;
    for (int i = 0; i < n; i++) {
        struct pgwt_query_row *r = &qry.rows[i];
        printf("  %20lld %10llu %12.1f %7.1f%%  %-20s\n",
               (long long)r->query_id, (unsigned long long)r->count,
               r->total_ms, r->pct_db, r->top_wait);
    }
    free(qry.rows);

    free(events);
    printf("\n");
}

int main(int argc, char **argv)
{
    int dump_mode = 0;
    const char *trace_dir = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump") == 0)
            dump_mode = 1;
        else if (argv[i][0] != '-')
            trace_dir = argv[i];
    }

    if (!trace_dir) {
        fprintf(stderr, "Usage: pgwt-server [--dump] <trace-dir>\n");
        return 1;
    }

    /* Line-buffered stdout is critical for SSH pipe */
    setvbuf(stdout, NULL, _IOLBF, 0);

    struct pgwt_server srv;
    if (server_init(&srv, trace_dir) != 0)
        return 1;

    if (dump_mode) {
        dump_summary(&srv);
        return 0;
    }

    char line[8192];
    while (fgets(line, sizeof(line), stdin)) {
        struct pgwt_request req;
        parse_request(line, &req);

        if (req.cmd[0] == '\0') {
            cJSON *root = cJSON_CreateObject();
            cJSON_AddNumberToObject(root, "id", (double)req.id);
            cJSON_AddStringToObject(root, "error", "missing cmd");
            emit_json(root);
            continue;
        }

        dispatch(&srv, &req);
    }

    return 0;
}
