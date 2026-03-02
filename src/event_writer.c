/* event_writer.c — Raw event file writer: columnar encoding + LZ4
 *
 * Buffers events in blocks of 4096, encodes column-wise, LZ4-compresses,
 * writes to hourly-rotating trace files with a block index footer. */
#include "event_writer.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <grp.h>
#include <lz4.h>

/* ── Varint (unsigned LEB128) ─────────────────────────────── */

int pgwt_encode_varint(uint64_t val, uint8_t *out)
{
    int n = 0;
    do {
        uint8_t byte = val & 0x7F;
        val >>= 7;
        if (val) byte |= 0x80;
        out[n++] = byte;
    } while (val);
    return n;
}

int pgwt_decode_varint(const uint8_t *in, size_t avail, uint64_t *val)
{
    uint64_t result = 0;
    int shift = 0, n = 0;
    do {
        if (n >= (int)avail || shift > 63) return -1;
        uint8_t byte = in[n];
        result |= (uint64_t)(byte & 0x7F) << shift;
        shift += 7;
        n++;
        if (!(byte & 0x80)) break;
    } while (1);
    *val = result;
    return n;
}

/* ── Columnar block encoder ───────────────────────────────── */

size_t pgwt_encode_block(const struct pgwt_trace_event *events, int count,
                         uint8_t *out, size_t out_size)
{
    uint8_t *p = out;
    (void)out_size;  /* caller guarantees sufficient space */

    /* Column 1: timestamps, delta-encoded as varint */
    uint64_t prev_ts = 0;
    for (int i = 0; i < count; i++) {
        uint64_t delta = events[i].timestamp_ns - prev_ts;
        p += pgwt_encode_varint(delta, p);
        prev_ts = events[i].timestamp_ns;
    }

    /* Column 2: PIDs as raw uint32 */
    for (int i = 0; i < count; i++) {
        uint32_t pid = events[i].pid;
        memcpy(p, &pid, 4); p += 4;
    }

    /* Column 3: old_events as raw uint32 */
    for (int i = 0; i < count; i++) {
        uint32_t ev = events[i].old_event;
        memcpy(p, &ev, 4); p += 4;
    }

    /* Column 4: new_events as raw uint32 */
    for (int i = 0; i < count; i++) {
        uint32_t ev = events[i].new_event;
        memcpy(p, &ev, 4); p += 4;
    }

    /* Column 5: durations as varint */
    for (int i = 0; i < count; i++) {
        p += pgwt_encode_varint(events[i].duration_ns, p);
    }

    /* Column 6: query_ids as raw uint64 */
    for (int i = 0; i < count; i++) {
        uint64_t qid = events[i].query_id;
        memcpy(p, &qid, 8); p += 8;
    }

    return (size_t)(p - out);
}

/* ── File operations (static) ─────────────────────────────── */

static int write_footer(struct pgwt_event_writer *w)
{
    for (int i = 0; i < w->num_blocks; i++) {
        if (fwrite(&w->block_index[i], sizeof(w->block_index[i]), 1, w->fp) != 1)
            return -1;
    }
    uint32_t nb = (uint32_t)w->num_blocks;
    if (fwrite(&nb, sizeof(nb), 1, w->fp) != 1)
        return -1;
    return 0;
}

static int open_trace_file(struct pgwt_event_writer *w, uint64_t first_mono_ns)
{
    (void)first_mono_ns;

    snprintf(w->current_path, sizeof(w->current_path),
             "%s/current.trace", w->trace_dir);

    w->fp = fopen(w->current_path, "wb");
    if (!w->fp) {
        fprintf(stderr, "WARN: cannot create %s: %s\n",
                w->current_path, strerror(errno));
        return -1;
    }

    /* Set group ownership and permissions for non-root access */
    if (w->trace_gid != (gid_t)-1) {
        fchown(fileno(w->fp), (uid_t)-1, w->trace_gid);
        fchmod(fileno(w->fp), 0640);
    }

    /* Capture wall-clock and monotonic timestamps */
    struct timespec wall, mono;
    clock_gettime(CLOCK_REALTIME, &wall);
    clock_gettime(CLOCK_MONOTONIC, &mono);
    w->file_start_wall_ns = (uint64_t)wall.tv_sec * 1000000000ULL + wall.tv_nsec;
    w->file_start_mono_ns = (uint64_t)mono.tv_sec * 1000000000ULL + mono.tv_nsec;

    /* Record current hour for rotation */
    time_t now = wall.tv_sec;
    struct tm tm;
    localtime_r(&now, &tm);
    w->current_hour = tm.tm_yday * 24 + tm.tm_hour;

    /* Write file header */
    struct pgwt_trace_file_header hdr = {
        .magic = PGWT_TRACE_MAGIC,
        .version = PGWT_TRACE_VERSION,
        .flags = PGWT_FLAG_LZ4,
        .pg_version = (uint32_t)w->pg_version,
        .start_time_ns = w->file_start_wall_ns,
        .clock_offset_ns = w->file_start_mono_ns,
    };
    if (fwrite(&hdr, sizeof(hdr), 1, w->fp) != 1) {
        fprintf(stderr, "WARN: cannot write trace file header: %s\n", strerror(errno));
        fclose(w->fp);
        w->fp = NULL;
        return -1;
    }

    w->num_blocks = 0;
    return 0;
}

static int flush_block(struct pgwt_event_writer *w)
{
    if (w->num_events == 0 || !w->fp) return 0;

    /* Columnar encode */
    size_t encoded_size = pgwt_encode_block(
        w->events, w->num_events, w->encode_buf, w->encode_buf_size);

    /* LZ4 compress */
    int compressed_size = LZ4_compress_default(
        (const char *)w->encode_buf, (char *)w->compress_buf,
        (int)encoded_size, (int)w->compress_buf_size);
    if (compressed_size <= 0) {
        fprintf(stderr, "WARN: LZ4 compression failed\n");
        w->num_events = 0;
        return -1;
    }

    /* Grow block index if needed */
    if (w->num_blocks >= w->block_index_cap) {
        int new_cap = w->block_index_cap * 2;
        struct pgwt_block_index_entry *new_idx =
            realloc(w->block_index, new_cap * sizeof(*new_idx));
        if (!new_idx) {
            fprintf(stderr, "WARN: cannot grow block index\n");
            w->enabled = false;
            return -1;
        }
        w->block_index = new_idx;
        w->block_index_cap = new_cap;
    }

    /* Record block index entry */
    long file_offset = ftell(w->fp);
    w->block_index[w->num_blocks++] = (struct pgwt_block_index_entry){
        .timestamp_ns = w->events[0].timestamp_ns,
        .file_offset = (uint64_t)file_offset,
    };

    /* Write block header + compressed data */
    struct pgwt_trace_block_header bh = {
        .first_timestamp_ns = w->events[0].timestamp_ns,
        .last_timestamp_ns = w->events[w->num_events - 1].timestamp_ns,
        .num_events = (uint32_t)w->num_events,
        .compressed_size = (uint32_t)compressed_size,
        .uncompressed_size = (uint32_t)encoded_size,
    };
    if (fwrite(&bh, sizeof(bh), 1, w->fp) != 1 ||
        fwrite(w->compress_buf, 1, compressed_size, w->fp) != (size_t)compressed_size) {
        fprintf(stderr, "WARN: trace file write failed: %s\n", strerror(errno));
        w->enabled = false;
        return -1;
    }

    w->total_events_written += w->num_events;
    w->total_bytes_written += sizeof(bh) + compressed_size;
    w->num_events = 0;
    return 0;
}

/* ── Public API ───────────────────────────────────────────── */

int pgwt_writer_init(struct pgwt_event_writer *w, const char *trace_dir,
                     int pg_version, int retention_hours,
                     const char *group_name)
{
    memset(w, 0, sizeof(*w));
    snprintf(w->trace_dir, sizeof(w->trace_dir), "%s", trace_dir);
    w->pg_version = pg_version;
    w->retention_hours = retention_hours;
    w->enabled = true;
    w->current_hour = -1;
    w->trace_gid = (gid_t)-1;

    /* Resolve trace group name → GID */
    if (group_name) {
        struct group *gr = getgrnam(group_name);
        if (gr) {
            w->trace_gid = gr->gr_gid;
        } else {
            fprintf(stderr, "WARN: group '%s' not found — "
                    "trace files will be readable by root only\n", group_name);
        }
    }

    /* Create trace directory with group-readable permissions */
    mkdir(w->trace_dir, 0750);  /* ignore EEXIST */
    if (w->trace_gid != (gid_t)-1) {
        chown(w->trace_dir, (uid_t)-1, w->trace_gid);
        chmod(w->trace_dir, 0750);
    }

    /* Allocate scratch buffers */
    w->encode_buf_size = PGWT_BLOCK_EVENTS * 36 + PGWT_BLOCK_EVENTS * 10;
    w->encode_buf = malloc(w->encode_buf_size);

    w->compress_buf_size = LZ4_compressBound((int)w->encode_buf_size);
    w->compress_buf = malloc(w->compress_buf_size);

    w->block_index_cap = 256;
    w->block_index = malloc(w->block_index_cap * sizeof(*w->block_index));

    if (!w->encode_buf || !w->compress_buf || !w->block_index) {
        pgwt_writer_destroy(w);
        return -1;
    }

    return 0;
}

int pgwt_writer_push_event(struct pgwt_event_writer *w,
                           const struct pgwt_trace_event *evt)
{
    if (!w->enabled) return 0;

    /* Lazy file open on first event */
    if (!w->fp) {
        if (open_trace_file(w, evt->timestamp_ns) != 0) {
            w->enabled = false;
            return -1;
        }
    }

    w->events[w->num_events++] = *evt;

    if (w->num_events >= PGWT_BLOCK_EVENTS)
        return flush_block(w);

    return 0;
}

int pgwt_writer_check_rotation(struct pgwt_event_writer *w)
{
    if (!w->enabled || !w->fp) return 0;

    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    struct tm tm;
    time_t now = wall.tv_sec;
    localtime_r(&now, &tm);
    int current_hour = tm.tm_yday * 24 + tm.tm_hour;

    if (current_hour == w->current_hour)
        return 0;

    /* Hour changed — rotate */
    flush_block(w);
    write_footer(w);

    if (w->verbose) {
        double ratio = w->total_events_written > 0
            ? (double)(w->total_events_written * 36) / w->total_bytes_written
            : 0;
        fprintf(stderr, "INFO: trace file closed: %lu events, %lu bytes "
                "(%.1fx compression)\n",
                (unsigned long)w->total_events_written,
                (unsigned long)w->total_bytes_written, ratio);
    }

    fclose(w->fp);
    w->fp = NULL;

    /* Rename current.trace → YYYY-MM-DD_HH.trace.lz4 */
    time_t file_start_sec = (time_t)(w->file_start_wall_ns / 1000000000ULL);
    struct tm file_tm;
    localtime_r(&file_start_sec, &file_tm);
    char new_path[512];
    snprintf(new_path, sizeof(new_path), "%s/%04d-%02d-%02d_%02d.trace.lz4",
             w->trace_dir,
             file_tm.tm_year + 1900, file_tm.tm_mon + 1,
             file_tm.tm_mday, file_tm.tm_hour);
    rename(w->current_path, new_path);

    /* Reset stats for next file */
    w->total_events_written = 0;
    w->total_bytes_written = 0;

    return 0;
}

int pgwt_writer_close(struct pgwt_event_writer *w)
{
    if (!w->fp) return 0;

    flush_block(w);
    write_footer(w);

    if (w->verbose) {
        double ratio = w->total_events_written > 0
            ? (double)(w->total_events_written * 36) / w->total_bytes_written
            : 0;
        fprintf(stderr, "INFO: trace file closed: %lu events, %lu bytes "
                "(%.1fx compression)\n",
                (unsigned long)w->total_events_written,
                (unsigned long)w->total_bytes_written, ratio);
    }

    fclose(w->fp);
    w->fp = NULL;
    return 0;
}

int pgwt_writer_cleanup_old_files(struct pgwt_event_writer *w)
{
    if (w->retention_hours <= 0) return 0;

    DIR *dir = opendir(w->trace_dir);
    if (!dir) return -1;

    time_t cutoff = time(NULL) - (time_t)w->retention_hours * 3600;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!strstr(ent->d_name, ".trace.lz4"))
            continue;

        struct tm ftm = {0};
        int hour;
        if (sscanf(ent->d_name, "%4d-%2d-%2d_%2d.trace.lz4",
                   &ftm.tm_year, &ftm.tm_mon, &ftm.tm_mday, &hour) != 4)
            continue;

        ftm.tm_year -= 1900;
        ftm.tm_mon -= 1;
        ftm.tm_hour = hour;
        ftm.tm_isdst = -1;
        time_t file_time = mktime(&ftm);

        if (file_time > 0 && file_time < cutoff) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", w->trace_dir, ent->d_name);
            if (unlink(path) == 0 && w->verbose)
                fprintf(stderr, "INFO: deleted old trace file %s\n", ent->d_name);
        }
    }
    closedir(dir);
    return 0;
}

void pgwt_writer_destroy(struct pgwt_event_writer *w)
{
    free(w->encode_buf);
    w->encode_buf = NULL;
    free(w->compress_buf);
    w->compress_buf = NULL;
    free(w->block_index);
    w->block_index = NULL;
}
