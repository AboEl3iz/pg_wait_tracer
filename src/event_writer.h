/* event_writer.h — Raw event file writer: columnar encoding + LZ4 */
#ifndef PGWT_EVENT_WRITER_H
#define PGWT_EVENT_WRITER_H

#include "pg_wait_tracer.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>

/* ── On-disk format constants ─────────────────────────────── */

#define PGWT_TRACE_MAGIC      0x54574750   /* "PGWT" little-endian */
#define PGWT_TRACE_VERSION    2
#define PGWT_FLAG_LZ4         0x0001

#define PGWT_BLOCK_EVENTS     4096         /* events per block */

/* Block type (trace format v2). Identifies what a block's columns mean.
 *   TRANSITIONS — wait-event transition intervals (exact fidelity): the
 *                 full columnar layout (ts, pid, old_event, new_event,
 *                 duration_ns, query_id). This is what the watchpoint
 *                 ("full") provider writes.
 *   SAMPLES     — point observations of a backend's current wait event
 *                 (sampled fidelity): a reduced columnar layout
 *                 (ts, pid, event, query_id) — no old_event, no duration.
 *                 This is what the A2 sampler writes. Each sample means
 *                 "at timestamp T, pid P was in event E"; A3 treats it as
 *                 a point observation worth `sample_period_ns`, never an
 *                 interval.
 *
 *                 Event id 0 (T2): a first-class CPU sample — "at T, pid P
 *                 was on-CPU doing requested work". The capture side gates
 *                 which we==0 readings are recorded (client backends only
 *                 inside a command; background types always; see
 *                 docs/AAS_SEMANTICS_DECISION.md), so consumers treat id 0
 *                 exactly like any active sample. Pre-T2 writers never
 *                 emitted id 0, so old traces read back unchanged; old
 *                 readers decode id 0 as the CPU pseudo-event they already
 *                 knew from transitions. No layout change. */
enum pgwt_block_type {
    PGWT_BLOCK_TRANSITIONS = 0,
    PGWT_BLOCK_SAMPLES     = 1,
};

/* File header (28 bytes) */
struct pgwt_trace_file_header {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t pg_version;
    uint64_t start_time_ns;      /* CLOCK_REALTIME at file creation */
    uint64_t clock_offset_ns;    /* CLOCK_MONOTONIC at file creation */
} __attribute__((packed));

/* Block header (v2: 40 bytes).
 * block_type/sample_period_ns are new in v2. sample_period_ns is the
 * nominal interval between samples in a SAMPLES block (0 for TRANSITIONS).
 * reserved keeps the 8-byte fields naturally aligned and leaves room for
 * a future v3 without another size bump. */
struct pgwt_trace_block_header {
    uint64_t first_timestamp_ns;
    uint64_t last_timestamp_ns;
    uint32_t num_events;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t block_type;         /* enum pgwt_block_type */
    uint16_t reserved;           /* 0; reserved for future use */
    uint64_t sample_period_ns;   /* SAMPLES: nominal sample interval; TRANSITIONS: 0 */
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
    gid_t         trace_gid;      /* group for trace files, (gid_t)-1 = no chown */
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
                      int pg_version, int retention_hours,
                      const char *group_name);
int  pgwt_writer_push_event(struct pgwt_event_writer *w,
                            const struct pgwt_trace_event *evt);

/* Write a SAMPLES block (trace format v2). The A2 sampler calls this once
 * per drain with the samples it collected this tick-batch. Only the pid,
 * new_event (= sampled event), query_id, and timestamp_ns fields of each
 * struct are used; old_event/duration_ns are ignored (samples carry
 * neither). `sample_period_ns` is recorded in the block header so the
 * compute layer (A3) can weight each sample. Samples must be sorted by
 * timestamp ascending (delta-varint encoding requires it). A pending
 * TRANSITIONS block, if any, is flushed first so block boundaries stay
 * clean. Returns 0 on success, -1 on error. */
int  pgwt_writer_push_samples(struct pgwt_event_writer *w,
                              const struct pgwt_trace_event *samples,
                              int count, uint64_t sample_period_ns);

int  pgwt_writer_check_rotation(struct pgwt_event_writer *w);
int  pgwt_writer_close(struct pgwt_event_writer *w);
int  pgwt_writer_cleanup_old_files(struct pgwt_event_writer *w);
void pgwt_writer_destroy(struct pgwt_event_writer *w);

/* ── Exposed for unit testing ─────────────────────────────── */

/* Encode a TRANSITIONS block (full columnar layout). */
size_t pgwt_encode_block(const struct pgwt_trace_event *events, int count,
                         uint8_t *out, size_t out_size);
/* Encode a SAMPLES block (reduced layout: ts, pid, event, query_id).
 * Reads each event's new_event field as the sampled event id. */
size_t pgwt_encode_sample_block(const struct pgwt_trace_event *events, int count,
                                uint8_t *out, size_t out_size);
int pgwt_encode_varint(uint64_t val, uint8_t *out);
int pgwt_decode_varint(const uint8_t *in, size_t avail, uint64_t *val);

#endif /* PGWT_EVENT_WRITER_H */
