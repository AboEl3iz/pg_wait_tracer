/* query_text.c — SQL query text capture from PgBackendStatus.st_activity_raw
 *
 * Captures the actual running SQL (not normalized) when a new query_id
 * appears in the event stream. Writes to query_texts.jsonl sidecar file. */
#include "query_text.h"
#include "pg_wait_tracer.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

/* ── Hash set for seen query_ids ─────────────────────────── */

static uint64_t hash64(uint64_t x)
{
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return x;
}

/* Returns 1 if query_id was already in the set, 0 if newly inserted. */
static int seen_check_or_insert(struct pgwt_query_text_capture *qt,
                                 uint64_t query_id)
{
    uint32_t idx = (uint32_t)(hash64(query_id) & (QT_HT_SIZE - 1));
    for (int i = 0; i < QT_HT_SIZE; i++) {
        if (qt->seen[idx] == query_id)
            return 1;  /* already seen */
        if (qt->seen[idx] == 0) {
            /* Empty slot — insert */
            qt->seen[idx] = query_id;
            qt->num_seen++;
            return 0;  /* newly inserted */
        }
        idx = (idx + 1) & (QT_HT_SIZE - 1);
    }
    /* Table full — treat as "seen" to avoid repeated reads */
    return 1;
}

/* ── Read st_activity_raw from backend process ───────────── */

/* Read the activity string for a backend via /proc/<pid>/mem.
 * Returns length of string read, or 0 on failure. */
static int read_activity(const struct pgwt_query_text_capture *qt,
                          pid_t pid, char *out, int out_size)
{
    char mem_path[64];
    snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", pid);

    int fd = open(mem_path, O_RDONLY);
    if (fd < 0)
        return 0;

    /* Step 1: Read MyBEEntry pointer (PgBackendStatus*) */
    uint64_t be_ptr = 0;
    if (pread(fd, &be_ptr, sizeof(be_ptr), qt->my_be_entry_addr) != sizeof(be_ptr)
        || be_ptr == 0) {
        close(fd);
        return 0;
    }

    /* Step 2: Read st_activity_raw pointer (char*) from PgBackendStatus */
    uint64_t activity_ptr = 0;
    if (pread(fd, &activity_ptr, sizeof(activity_ptr),
              be_ptr + qt->st_activity_offset) != sizeof(activity_ptr)
        || activity_ptr == 0) {
        close(fd);
        return 0;
    }

    /* Step 3: Read the actual string from the activity pointer */
    memset(out, 0, out_size);
    ssize_t n = pread(fd, out, out_size - 1, activity_ptr);
    close(fd);

    if (n <= 0)
        return 0;

    out[out_size - 1] = '\0';

    /* Find actual string length (null-terminated) */
    int len = (int)strnlen(out, n);
    out[len] = '\0';
    return len;
}

/* ── JSON-safe string writing ────────────────────────────── */

/* Write a JSON-escaped string to file.  Escapes: " \ \n \r \t and control chars. */
static void write_json_string(FILE *fp, const char *s, int len)
{
    fputc('"', fp);
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  fputs("\\\"", fp); break;
        case '\\': fputs("\\\\", fp); break;
        case '\n': fputs("\\n", fp);  break;
        case '\r': fputs("\\r", fp);  break;
        case '\t': fputs("\\t", fp);  break;
        default:
            if (c < 0x20) {
                /* Control character — use \uXXXX */
                fprintf(fp, "\\u%04x", c);
            } else {
                fputc(c, fp);
            }
        }
    }
    fputc('"', fp);
}

/* ── Public API ──────────────────────────────────────────── */

int pgwt_qt_init(struct pgwt_query_text_capture *qt,
                 const char *trace_dir,
                 uint64_t my_be_entry_addr,
                 int st_activity_offset)
{
    memset(qt, 0, sizeof(*qt));
    snprintf(qt->trace_dir, sizeof(qt->trace_dir), "%s", trace_dir);
    qt->my_be_entry_addr = my_be_entry_addr;
    qt->st_activity_offset = st_activity_offset;
    qt->enabled = true;

    /* Allocate reusable read buffer (1 MB on heap, not stack) */
    qt->read_buf = malloc(QT_MAX_TEXT);
    if (!qt->read_buf) {
        fprintf(stderr, "WARN: cannot allocate %d byte read buffer\n", QT_MAX_TEXT);
        qt->enabled = false;
        return -1;
    }

    /* Open (truncate) query_texts.jsonl */
    char path[512];
    snprintf(path, sizeof(path), "%s/query_texts.jsonl", trace_dir);
    qt->fp = fopen(path, "w");
    if (!qt->fp) {
        fprintf(stderr, "WARN: cannot create %s: %s\n", path, strerror(errno));
        qt->enabled = false;
        return -1;
    }

    return 0;
}

void pgwt_qt_check(struct pgwt_query_text_capture *qt,
                   pid_t pid, uint64_t query_id, uint64_t wall_ns)
{
    if (!qt->enabled || !qt->fp || query_id == 0)
        return;

    /* Check if already seen */
    if (seen_check_or_insert(qt, query_id))
        return;

    /* New query_id — read st_activity_raw from the backend */
    int len = read_activity(qt, pid, qt->read_buf, QT_MAX_TEXT);
    if (len <= 0) {
        if (qt->verbose)
            fprintf(stderr, "WARN: cannot read st_activity for PID %d query_id %llu\n",
                    pid, (unsigned long long)query_id);
        return;
    }

    /* Get wall-clock timestamp (only called for new query_ids, so fine) */
    if (wall_ns == 0) {
        struct timespec wall;
        clock_gettime(CLOCK_REALTIME, &wall);
        wall_ns = (uint64_t)wall.tv_sec * 1000000000ULL + wall.tv_nsec;
    }

    /* Write JSONL line: {"q":"<query_id>","t":"<text>","ts":<wall_ns>}
     * query_id as string to preserve full int64 precision (JSON numbers
     * are doubles with only 53 bits). Signed — can be negative. */
    fprintf(qt->fp, "{\"q\":\"%lld\",\"t\":", (long long)(int64_t)query_id);
    write_json_string(qt->fp, qt->read_buf, len);
    fprintf(qt->fp, ",\"ts\":%llu}\n", (unsigned long long)wall_ns);
    fflush(qt->fp);

    if (qt->verbose)
        fprintf(stderr, "INFO: captured query text for query_id %llu: %.60s%s\n",
                (unsigned long long)query_id,
                qt->read_buf,
                len > 60 ? "..." : "");
}

void pgwt_qt_store(struct pgwt_query_text_capture *qt,
                   uint64_t query_id, const char *text, pid_t pid)
{
    if (!qt->enabled || !qt->fp || query_id == 0 || !text || !text[0])
        return;

    /* Check if already seen */
    if (seen_check_or_insert(qt, query_id))
        return;

    int len = (int)strnlen(text, PGWT_QUERY_TEXT_LEN - 1);

    /* Get wall-clock timestamp */
    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    uint64_t wall_ns = (uint64_t)wall.tv_sec * 1000000000ULL + wall.tv_nsec;

    /* Write JSONL line */
    fprintf(qt->fp, "{\"q\":\"%lld\",\"t\":", (long long)(int64_t)query_id);
    write_json_string(qt->fp, text, len);
    fprintf(qt->fp, ",\"ts\":%llu}\n", (unsigned long long)wall_ns);
    fflush(qt->fp);

    if (qt->verbose)
        fprintf(stderr, "INFO: captured query text for query_id %llu: %.60s%s\n",
                (unsigned long long)query_id,
                text, len > 60 ? "..." : "");
}

void pgwt_qt_close(struct pgwt_query_text_capture *qt)
{
    if (qt->fp) {
        fclose(qt->fp);
        qt->fp = NULL;
    }
    free(qt->read_buf);
    qt->read_buf = NULL;
    qt->enabled = false;
}
