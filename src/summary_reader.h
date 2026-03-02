/* summary_reader.h — Summary file reader: block index, LZ4 decompress, deserialize */
#ifndef PGWT_SUMMARY_READER_H
#define PGWT_SUMMARY_READER_H

#include "summary_writer.h"
#include "event_writer.h"

#include <stdint.h>
#include <stdio.h>

/* ── Single-file reader state ──────────────────────────── */

struct pgwt_summary_reader {
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

/* Open a summary file: read header, footer, block index.
 * Returns 0 on success, -1 on error. */
int pgwt_summary_reader_open(struct pgwt_summary_reader *r, const char *path);

/* Close file and free internal buffers. */
void pgwt_summary_reader_close(struct pgwt_summary_reader *r);

/* Decode a single block by index into an accumulator.
 * Returns 0 on success, -1 on error. */
int pgwt_summary_reader_decode_block(struct pgwt_summary_reader *r,
                                      int block_idx,
                                      struct pgwt_summary_accum *out);

/* Find the first block at or after wall_ns. Returns block index. */
int pgwt_summary_reader_find_block(const struct pgwt_summary_reader *r,
                                    uint64_t wall_ns);

/* ── Multi-file scanning ───────────────────────────────── */

struct pgwt_summary_file_entry {
    char     path[512];
    uint64_t start_wall_ns;
    int      year, month, day, hour;
};

/* Scan trace_dir for summary files. Returns count, sorted by time ascending. */
int pgwt_scan_summary_files(const char *trace_dir,
                             struct pgwt_summary_file_entry *entries,
                             int max_entries);

/* ── Bulk load: read all summary records in a time range ── */

/* Load summary records from files in trace_dir covering [from_ns, to_ns].
 * Allocates and returns array of pgwt_summary_accum records.
 * Caller must free(*out) when done.
 * Returns count of records loaded, or -1 on error. */
int pgwt_load_summaries(const char *trace_dir,
                         uint64_t from_wall_ns, uint64_t to_wall_ns,
                         struct pgwt_summary_accum **out);

#endif /* PGWT_SUMMARY_READER_H */
