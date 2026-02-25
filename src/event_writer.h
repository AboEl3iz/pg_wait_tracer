/* event_writer.h — Raw event file writer: columnar encoding + LZ4 */
#ifndef PGWT_EVENT_WRITER_H
#define PGWT_EVENT_WRITER_H

#include "pg_wait_tracer.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

/* ── On-disk format constants ─────────────────────────────── */

#define PGWT_TRACE_MAGIC      0x54574750   /* "PGWT" little-endian */
#define PGWT_TRACE_VERSION    1
#define PGWT_FLAG_LZ4         0x0001

#define PGWT_BLOCK_EVENTS     4096         /* events per block */

/* File header (28 bytes) */
struct pgwt_trace_file_header {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t pg_version;
    uint64_t start_time_ns;      /* CLOCK_REALTIME at file creation */
    uint64_t clock_offset_ns;    /* CLOCK_MONOTONIC at file creation */
} __attribute__((packed));

/* Block header (28 bytes) */
struct pgwt_trace_block_header {
    uint64_t first_timestamp_ns;
    uint64_t last_timestamp_ns;
    uint32_t num_events;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
} __attribute__((packed));

/* Block index entry (16 bytes) */
struct pgwt_block_index_entry {
    uint64_t timestamp_ns;
    uint64_t file_offset;
} __attribute__((packed));

/* ── Writer state ─────────────────────────────────────────── */

struct pgwt_event_writer {
    /* Configuration */
    char          trace_dir[256];
    int           pg_version;
    int           retention_hours;
    bool          enabled;
    bool          verbose;

    /* Current file */
    FILE         *fp;
    char          current_path[512];
    int           current_hour;           /* day*24 + hour for rotation */
    uint64_t      file_start_wall_ns;
    uint64_t      file_start_mono_ns;

    /* Block index (written as footer) */
    struct pgwt_block_index_entry *block_index;
    int           num_blocks;
    int           block_index_cap;

    /* Event buffer (current block) */
    struct pgwt_trace_event events[PGWT_BLOCK_EVENTS];
    int           num_events;

    /* Scratch buffers (allocated once) */
    uint8_t      *encode_buf;
    size_t        encode_buf_size;
    uint8_t      *compress_buf;
    size_t        compress_buf_size;

    /* Stats */
    uint64_t      total_events_written;
    uint64_t      total_bytes_written;
};

/* ── Public API ───────────────────────────────────────────── */

int  pgwt_writer_init(struct pgwt_event_writer *w, const char *trace_dir,
                      int pg_version, int retention_hours);
int  pgwt_writer_push_event(struct pgwt_event_writer *w,
                            const struct pgwt_trace_event *evt);
int  pgwt_writer_check_rotation(struct pgwt_event_writer *w);
int  pgwt_writer_close(struct pgwt_event_writer *w);
int  pgwt_writer_cleanup_old_files(struct pgwt_event_writer *w);
void pgwt_writer_destroy(struct pgwt_event_writer *w);

/* ── Exposed for unit testing ─────────────────────────────── */

size_t pgwt_encode_block(const struct pgwt_trace_event *events, int count,
                         uint8_t *out, size_t out_size);
int pgwt_encode_varint(uint64_t val, uint8_t *out);
int pgwt_decode_varint(const uint8_t *in, size_t avail, uint64_t *val);

#endif /* PGWT_EVENT_WRITER_H */
