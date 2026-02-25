/* event_reader.h — Trace file reader: block index, LZ4 decompress, columnar decode */
#ifndef PGWT_EVENT_READER_H
#define PGWT_EVENT_READER_H

#include "event_writer.h"
#include "pg_wait_tracer.h"

#include <stdint.h>
#include <stdio.h>

/* ── Single-file reader state ──────────────────────────── */

struct pgwt_event_reader {
    FILE         *fp;
    char          path[512];

    struct pgwt_trace_file_header header;

    struct pgwt_block_index_entry *block_index;
    int           num_blocks;

    /* Clock conversion: wall_ns = mono_ns + mono_to_wall */
    int64_t       mono_to_wall;

    /* Scratch buffers */
    uint8_t      *compress_buf;
    size_t        compress_buf_size;
    uint8_t      *decode_buf;
    size_t        decode_buf_size;
};

/* Open a trace file: read header, footer, block index.
 * Returns 0 on success, -1 on error. */
int pgwt_reader_open(struct pgwt_event_reader *r, const char *path);

/* Close file and free internal buffers. */
void pgwt_reader_close(struct pgwt_event_reader *r);

/* Decode a single block by index into out[].
 * Returns number of events decoded, or -1 on error. */
int pgwt_reader_decode_block(struct pgwt_event_reader *r, int block_idx,
                              struct pgwt_trace_event *out, int max_events);

/* Find the first block that could contain events at or after mono_ns.
 * Returns block index (0-based). */
int pgwt_reader_find_block(const struct pgwt_event_reader *r, uint64_t mono_ns);

/* Convert wall-clock nanoseconds to monotonic for this file. */
uint64_t pgwt_reader_wall_to_mono(const struct pgwt_event_reader *r, uint64_t wall_ns);

/* Convert monotonic nanoseconds to wall-clock for this file. */
uint64_t pgwt_reader_mono_to_wall(const struct pgwt_event_reader *r, uint64_t mono_ns);

/* ── Multi-file scanning ───────────────────────────────── */

struct pgwt_trace_file_entry {
    char     path[512];
    uint64_t start_wall_ns;
    int      year, month, day, hour;
};

/* Scan trace_dir for trace files. Returns count, sorted by time ascending. */
int pgwt_scan_trace_files(const char *trace_dir,
                           struct pgwt_trace_file_entry *entries,
                           int max_entries);

/* Parse time string → nanoseconds since Unix epoch.
 * Supports: ISO 8601 ("2025-02-25T14:30:00"), relative ("1h", "30m"), "now". */
int pgwt_parse_time(const char *str, uint64_t *wall_ns);

/* ── Replay accumulation ───────────────────────────────── */

struct pgwt_accumulator;

/* Replay decoded events into an accumulator, filtering by time range. */
void pgwt_replay_events(struct pgwt_accumulator *acc,
                         const struct pgwt_trace_event *events, int count,
                         uint64_t from_mono_ns, uint64_t to_mono_ns);

#endif /* PGWT_EVENT_READER_H */
