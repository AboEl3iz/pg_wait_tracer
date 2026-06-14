/* gen_test_traces.c — Generate synthetic trace files for correctness testing
 *
 * Reads a scenario from a JSON file (or command-line args) and writes
 * .trace + .summary files plus backends.jsonl + query_texts.jsonl.
 *
 * Usage:
 *   gen_test_traces -o <output-dir> -s <scenario.json>
 *   gen_test_traces -o <output-dir> --inline '<json-array>'
 *
 * Scenario JSON format:
 * {
 *   "backends": [{"pid": 1000, "type": "client", "user": "postgres", "db": "testdb"}],
 *   "queries":  [{"id": 12345, "text": "SELECT pg_sleep(1)"}],
 *   "events": [
 *     {"pid": 1000, "ts": 1000000000, "dur": 500000, "old": 167772181, "new": 0, "qid": 12345}
 *   ],
 *   "sample_period_ns": 100000000,
 *   "samples": [
 *     {"pid": 1000, "ts": 1000000000, "event": 167772181, "qid": 12345}
 *   ]
 * }
 *
 * "events"  → written as a TRANSITIONS block (trace format v2). The existing
 *             columnar layout: ts, pid, old, new, dur, qid.
 * "samples" → written as a SAMPLES block (trace format v2). Reduced layout:
 *             ts, pid, event, qid — no old/new/dur. "event" is the single
 *             sampled wait_event_info; "sample_period_ns" (top-level, default
 *             100000000 = 10 Hz) is stored in the block header. Samples must
 *             be sorted by ts ascending. A scenario may have events, samples,
 *             or both. Both arrays produce A3 fixtures.
 *
 * Event IDs: old_event/new_event/event are uint32 wait_event_info values.
 *   CPU = 0, IO:DataFileRead = 0x0A000001, Lock:relation = 0x03000001, etc.
 *   Class byte << 24 | event_number.
 *
 * Build: cc -O2 -I../src -I../include -o gen_test_traces gen_test_traces.c \
 *        ../build/server_event_writer.o ../build/server_summary_writer.o \
 *        ../build/server_wait_event.o -llz4
 */
#include "event_writer.h"
#include "summary_writer.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <getopt.h>
#include <dirent.h>

#define MAX_EVENTS       100000
#define MAX_TEST_BACKENDS 64
#define MAX_QUERIES       64

struct test_backend {
    uint32_t pid;
    char     type[32];
    char     user[64];
    char     db[64];
};

struct test_query {
    uint64_t id;
    char     text[256];
};

struct scenario {
    struct pgwt_trace_event events[MAX_EVENTS];
    int num_events;

    /* SAMPLES block (trace format v2). The sampled event id is stored in
     * each record's new_event field (what the writer's sample encoder reads). */
    struct pgwt_trace_event samples[MAX_EVENTS];
    int num_samples;
    uint64_t sample_period_ns;

    struct test_backend backends[MAX_TEST_BACKENDS];
    int num_backends;

    struct test_query queries[MAX_QUERIES];
    int num_queries;
};

/* ── Minimal JSON parser (good enough for test scenarios) ─── */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const char *find_key(const char *json, const char *key)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += strlen(needle);
    p = skip_ws(p);
    if (*p != ':') return NULL;
    return skip_ws(p + 1);
}

static int parse_string(const char **pp, char *out, size_t out_size)
{
    const char *p = skip_ws(*pp);
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < out_size - 1) {
        if (*p == '\\' && *(p+1)) { p++; }
        out[i++] = *p++;
    }
    out[i] = '\0';
    if (*p == '"') p++;
    *pp = p;
    return 0;
}

/* Find matching bracket/brace end */
static const char *find_matching(const char *p, char open, char close)
{
    int depth = 0;
    bool in_string = false;
    for (; *p; p++) {
        if (*p == '"' && *(p-1) != '\\') in_string = !in_string;
        if (in_string) continue;
        if (*p == open) depth++;
        if (*p == close) { depth--; if (depth == 0) return p; }
    }
    return NULL;
}

static int parse_backends(const char *arr, struct scenario *sc)
{
    const char *p = skip_ws(arr);
    if (*p != '[') return -1;
    p++;

    while (*p) {
        p = skip_ws(p);
        if (*p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;

        const char *end = find_matching(p, '{', '}');
        if (!end) break;

        /* Extract fields from this object */
        struct test_backend *b = &sc->backends[sc->num_backends];
        memset(b, 0, sizeof(*b));

        const char *v;
        if ((v = find_key(p, "pid"))) { b->pid = (uint32_t)strtoull(v, NULL, 10); }
        if ((v = find_key(p, "type"))) { parse_string(&v, b->type, sizeof(b->type)); }
        if ((v = find_key(p, "user"))) { parse_string(&v, b->user, sizeof(b->user)); }
        if ((v = find_key(p, "db")))   { parse_string(&v, b->db, sizeof(b->db)); }

        sc->num_backends++;
        p = end + 1;
    }
    return 0;
}

static int parse_queries(const char *arr, struct scenario *sc)
{
    const char *p = skip_ws(arr);
    if (*p != '[') return -1;
    p++;

    while (*p) {
        p = skip_ws(p);
        if (*p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;

        const char *end = find_matching(p, '{', '}');
        if (!end) break;

        struct test_query *q = &sc->queries[sc->num_queries];
        memset(q, 0, sizeof(*q));

        const char *v;
        if ((v = find_key(p, "id"))) { q->id = strtoull(v, NULL, 10); }
        if ((v = find_key(p, "text"))) { parse_string(&v, q->text, sizeof(q->text)); }

        sc->num_queries++;
        p = end + 1;
    }
    return 0;
}

static int parse_events(const char *arr, struct scenario *sc)
{
    const char *p = skip_ws(arr);
    if (*p != '[') return -1;
    p++;

    while (*p && sc->num_events < MAX_EVENTS) {
        p = skip_ws(p);
        if (*p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;

        const char *end = find_matching(p, '{', '}');
        if (!end) break;

        struct pgwt_trace_event *ev = &sc->events[sc->num_events];
        memset(ev, 0, sizeof(*ev));

        const char *v;
        if ((v = find_key(p, "pid")))  ev->pid = (uint32_t)strtoull(v, NULL, 10);
        if ((v = find_key(p, "ts")))   ev->timestamp_ns = strtoull(v, NULL, 10);
        if ((v = find_key(p, "dur")))  ev->duration_ns = strtoull(v, NULL, 10);
        if ((v = find_key(p, "old")))  ev->old_event = (uint32_t)strtoull(v, NULL, 10);
        if ((v = find_key(p, "new")))  ev->new_event = (uint32_t)strtoull(v, NULL, 10);
        if ((v = find_key(p, "qid")))  ev->query_id = strtoull(v, NULL, 10);

        sc->num_events++;
        p = end + 1;
    }
    return 0;
}

static int parse_samples(const char *arr, struct scenario *sc)
{
    const char *p = skip_ws(arr);
    if (*p != '[') return -1;
    p++;

    while (*p && sc->num_samples < MAX_EVENTS) {
        p = skip_ws(p);
        if (*p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;

        const char *end = find_matching(p, '{', '}');
        if (!end) break;

        struct pgwt_trace_event *ev = &sc->samples[sc->num_samples];
        memset(ev, 0, sizeof(*ev));

        const char *v;
        if ((v = find_key(p, "pid")))   ev->pid = (uint32_t)strtoull(v, NULL, 10);
        if ((v = find_key(p, "ts")))    ev->timestamp_ns = strtoull(v, NULL, 10);
        /* sampled event id → new_event (the field the sample encoder reads) */
        if ((v = find_key(p, "event"))) ev->new_event = (uint32_t)strtoull(v, NULL, 10);
        if ((v = find_key(p, "qid")))   ev->query_id = strtoull(v, NULL, 10);

        sc->num_samples++;
        p = end + 1;
    }
    return 0;
}

static int parse_scenario(const char *json, struct scenario *sc)
{
    const char *v;

    sc->sample_period_ns = 100000000ULL;  /* default 10 Hz */

    if ((v = find_key(json, "backends")))
        parse_backends(v, sc);
    if ((v = find_key(json, "queries")))
        parse_queries(v, sc);
    if ((v = find_key(json, "events")))
        parse_events(v, sc);
    if ((v = find_key(json, "sample_period_ns")))
        sc->sample_period_ns = strtoull(v, NULL, 10);
    if ((v = find_key(json, "samples")))
        parse_samples(v, sc);

    return 0;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

/* ── Output generation ─────────────────────────────────────── */

static int write_backends_jsonl(const char *dir, struct scenario *sc)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/backends.jsonl", dir);
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return -1; }

    for (int i = 0; i < sc->num_backends; i++) {
        struct test_backend *b = &sc->backends[i];
        if (b->user[0] && b->db[0]) {
            fprintf(f, "{\"pid\":%u,\"type\":\"%s\",\"user\":\"%s\",\"db\":\"%s\",\"addr\":\"127.0.0.1(5432)\"}\n",
                    b->pid, b->type, b->user, b->db);
        } else {
            fprintf(f, "{\"pid\":%u,\"type\":\"%s\"}\n", b->pid, b->type);
        }
    }

    fclose(f);
    return 0;
}

static int write_query_texts(const char *dir, struct scenario *sc)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/query_texts.jsonl", dir);
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return -1; }

    for (int i = 0; i < sc->num_queries; i++) {
        struct test_query *q = &sc->queries[i];
        fprintf(f, "{\"q\":%llu,\"t\":\"%s\",\"ts\":%llu}\n",
                (unsigned long long)q->id, q->text,
                (unsigned long long)1000000000ULL);
    }

    fclose(f);
    return 0;
}

static int write_traces(const char *dir, struct scenario *sc)
{
    struct pgwt_event_writer ew;
    struct pgwt_summary_writer sw;

    if (pgwt_writer_init(&ew, dir, 18, 24, NULL) != 0) {
        fprintf(stderr, "Failed to init event writer\n");
        return -1;
    }

    if (pgwt_summary_writer_init(&sw, dir, 24, NULL) != 0) {
        fprintf(stderr, "Failed to init summary writer\n");
        pgwt_writer_destroy(&ew);
        return -1;
    }

    /* TRANSITIONS block(s): the "events" array. Also feeds the summary
     * writer (summaries are transition-based; sampled summaries are A3). */
    for (int i = 0; i < sc->num_events; i++) {
        if (pgwt_writer_push_event(&ew, &sc->events[i]) != 0) {
            fprintf(stderr, "Failed to write event %d\n", i);
            break;
        }
        pgwt_summary_push_event(&sw, &sc->events[i]);
    }

    /* SAMPLES block: the "samples" array (trace format v2). Written after
     * the transitions so a scenario with both produces a transition block
     * followed by a sample block. */
    if (sc->num_samples > 0) {
        if (pgwt_writer_push_samples(&ew, sc->samples, sc->num_samples,
                                     sc->sample_period_ns) != 0)
            fprintf(stderr, "Failed to write %d samples\n", sc->num_samples);
    }

    /* Flush summary */
    pgwt_summary_flush(&sw);

    pgwt_writer_close(&ew);
    pgwt_summary_close(&sw);
    pgwt_writer_destroy(&ew);
    pgwt_summary_destroy(&sw);

    fprintf(stderr, "Wrote %d events + %d samples to %s\n",
            sc->num_events, sc->num_samples, dir);
    return 0;
}

/* ── Patch file headers so mono_to_wall = 0 ────────────────── */

static int patch_file_header(const char *path)
{
    FILE *f = fopen(path, "r+b");
    if (!f) return -1;

    struct pgwt_trace_file_header hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        return -1;
    }

    /* Set start_time_ns = clock_offset_ns so mono_to_wall = 0
     * This makes wall-clock timestamps equal to monotonic timestamps,
     * allowing test scenarios to use known timestamp values. */
    hdr.start_time_ns = hdr.clock_offset_ns;

    fseek(f, 0, SEEK_SET);
    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

static int patch_trace_headers(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return -1;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strstr(ent->d_name, ".trace") || strstr(ent->d_name, ".summary")) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
            if (patch_file_header(path) != 0) {
                fprintf(stderr, "WARN: failed to patch header of %s\n", ent->d_name);
            }
        }
    }

    closedir(d);
    return 0;
}

/* ── Main ──────────────────────────────────────────────────── */

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s -o <output-dir> -s <scenario.json>\n", prog);
    fprintf(stderr, "       %s -o <output-dir> --inline '<json>'\n", prog);
}

int main(int argc, char **argv)
{
    const char *outdir = NULL;
    const char *scenario_file = NULL;
    const char *inline_json = NULL;

    static struct option long_opts[] = {
        {"output",  required_argument, NULL, 'o'},
        {"scenario", required_argument, NULL, 's'},
        {"inline",  required_argument, NULL, 'i'},
        {"help",    no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "o:s:i:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'o': outdir = optarg; break;
        case 's': scenario_file = optarg; break;
        case 'i': inline_json = optarg; break;
        default:  usage(argv[0]); return 1;
        }
    }

    if (!outdir || (!scenario_file && !inline_json)) {
        usage(argv[0]);
        return 1;
    }

    /* Create output directory */
    mkdir(outdir, 0755);

    /* Parse scenario */
    struct scenario *sc = calloc(1, sizeof(*sc));
    if (!sc) { perror("calloc"); return 1; }

    char *json;
    if (scenario_file) {
        json = read_file(scenario_file);
        if (!json) return 1;
    } else {
        json = strdup(inline_json);
    }

    if (parse_scenario(json, sc) != 0) {
        fprintf(stderr, "Failed to parse scenario\n");
        free(json);
        free(sc);
        return 1;
    }
    free(json);

    fprintf(stderr, "Scenario: %d events, %d samples, %d backends, %d queries\n",
            sc->num_events, sc->num_samples, sc->num_backends, sc->num_queries);

    /* Write output files */
    int rc = 0;
    if (write_backends_jsonl(outdir, sc) != 0) rc = 1;
    if (write_query_texts(outdir, sc) != 0) rc = 1;
    if (write_traces(outdir, sc) != 0) rc = 1;

    /* Patch file headers so mono_to_wall = 0 (wall timestamps == mono timestamps).
     * This is essential for test correctness: synthetic events use known monotonic
     * timestamps, and we need the server to treat them as-is without offset. */
    patch_trace_headers(outdir);

    free(sc);
    return rc;
}
