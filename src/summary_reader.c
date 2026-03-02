/* summary_reader.c — Summary file reader: block decode, time-range seek, bulk load */
#include "summary_reader.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <lz4.h>

/* ── Single-file reader ────────────────────────────────── */

int pgwt_summary_reader_open(struct pgwt_summary_reader *r, const char *path)
{
    memset(r, 0, sizeof(*r));
    snprintf(r->path, sizeof(r->path), "%s", path);

    r->fp = fopen(path, "rb");
    if (!r->fp) {
        fprintf(stderr, "WARN: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    /* Read file header */
    if (fread(&r->header, sizeof(r->header), 1, r->fp) != 1) {
        fprintf(stderr, "WARN: cannot read header from %s\n", path);
        fclose(r->fp); r->fp = NULL;
        return -1;
    }

    if (r->header.magic != PGWT_SUMMARY_MAGIC) {
        fprintf(stderr, "WARN: bad magic in %s (0x%08x, expected PGWS)\n",
                path, r->header.magic);
        fclose(r->fp); r->fp = NULL;
        return -1;
    }
    if (r->header.version != PGWT_SUMMARY_VERSION) {
        fprintf(stderr, "WARN: unsupported summary version %d in %s\n",
                r->header.version, path);
        fclose(r->fp); r->fp = NULL;
        return -1;
    }

    r->mono_to_wall = (int64_t)r->header.start_time_ns
                    - (int64_t)r->header.clock_offset_ns;

    /* Read footer: last 4 bytes = num_blocks */
    if (fseek(r->fp, -4, SEEK_END) != 0) {
        fprintf(stderr, "WARN: cannot seek to footer in %s\n", path);
        fclose(r->fp); r->fp = NULL;
        return -1;
    }

    uint32_t nb;
    if (fread(&nb, sizeof(nb), 1, r->fp) != 1 || nb == 0) {
        fprintf(stderr, "WARN: cannot read block count from %s\n", path);
        fclose(r->fp); r->fp = NULL;
        return -1;
    }
    r->num_blocks = (int)nb;

    /* Read block index entries */
    long index_start = ftell(r->fp) - 4
                     - (long)nb * (long)sizeof(struct pgwt_block_index_entry);
    if (index_start < (long)sizeof(struct pgwt_trace_file_header)) {
        fprintf(stderr, "WARN: invalid block index in %s\n", path);
        fclose(r->fp); r->fp = NULL;
        return -1;
    }
    fseek(r->fp, index_start, SEEK_SET);

    r->block_index = malloc(nb * sizeof(struct pgwt_block_index_entry));
    if (!r->block_index) {
        fclose(r->fp); r->fp = NULL;
        return -1;
    }
    if (fread(r->block_index, sizeof(struct pgwt_block_index_entry), nb, r->fp) != nb) {
        fprintf(stderr, "WARN: cannot read block index from %s\n", path);
        free(r->block_index); r->block_index = NULL;
        fclose(r->fp); r->fp = NULL;
        return -1;
    }

    /* Allocate scratch buffers (summary records are up to ~260 KB uncompressed) */
    r->decode_buf_size = 300 * 1024;
    r->decode_buf = malloc(r->decode_buf_size);
    r->compress_buf_size = r->decode_buf_size;
    r->compress_buf = malloc(r->compress_buf_size);

    if (!r->decode_buf || !r->compress_buf) {
        pgwt_summary_reader_close(r);
        return -1;
    }

    return 0;
}

void pgwt_summary_reader_close(struct pgwt_summary_reader *r)
{
    if (r->fp) { fclose(r->fp); r->fp = NULL; }
    free(r->block_index); r->block_index = NULL;
    free(r->compress_buf); r->compress_buf = NULL;
    free(r->decode_buf); r->decode_buf = NULL;
}

int pgwt_summary_reader_decode_block(struct pgwt_summary_reader *r,
                                      int block_idx,
                                      struct pgwt_summary_accum *out)
{
    if (block_idx < 0 || block_idx >= r->num_blocks || !r->fp)
        return -1;

    fseek(r->fp, (long)r->block_index[block_idx].file_offset, SEEK_SET);

    /* Read block header */
    struct pgwt_summary_block_header bh;
    if (fread(&bh, sizeof(bh), 1, r->fp) != 1)
        return -1;

    /* Grow compress buffer if needed */
    if (bh.compressed_size > r->compress_buf_size) {
        uint8_t *tmp = realloc(r->compress_buf, bh.compressed_size);
        if (!tmp) return -1;
        r->compress_buf = tmp;
        r->compress_buf_size = bh.compressed_size;
    }
    if (fread(r->compress_buf, 1, bh.compressed_size, r->fp) != bh.compressed_size)
        return -1;

    /* Grow decode buffer if needed */
    if (bh.uncompressed_size > r->decode_buf_size) {
        uint8_t *tmp = realloc(r->decode_buf, bh.uncompressed_size);
        if (!tmp) return -1;
        r->decode_buf = tmp;
        r->decode_buf_size = bh.uncompressed_size;
    }

    /* LZ4 decompress */
    int decompressed = LZ4_decompress_safe(
        (const char *)r->compress_buf, (char *)r->decode_buf,
        (int)bh.compressed_size, (int)r->decode_buf_size);
    if (decompressed < 0)
        return -1;

    /* Initialize output accumulator from block header */
    memset(out, 0, sizeof(*out));
    out->second_wall_ns = bh.wall_ns;
    out->num_events = bh.num_events;
    out->num_sessions = bh.num_sessions;
    out->num_queries = bh.num_queries;

    /* Deserialize payload */
    if (pgwt_summary_deserialize(r->decode_buf, (size_t)decompressed, out) != 0)
        return -1;

    return 0;
}

int pgwt_summary_reader_find_block(const struct pgwt_summary_reader *r,
                                    uint64_t wall_ns)
{
    /* Block index timestamps are wall-clock for summary files */
    int lo = 0, hi = r->num_blocks;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (r->block_index[mid].timestamp_ns <= wall_ns)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (lo > 0) ? lo - 1 : 0;
}

/* ── Multi-file scanning ───────────────────────────────── */

int pgwt_scan_summary_files(const char *trace_dir,
                             struct pgwt_summary_file_entry *entries,
                             int max_entries)
{
    DIR *dir = opendir(trace_dir);
    if (!dir) return -1;

    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && n < max_entries) {
        struct pgwt_summary_file_entry *e = &entries[n];

        if (sscanf(ent->d_name, "%4d-%2d-%2d_%2d.summary.lz4",
                   &e->year, &e->month, &e->day, &e->hour) == 4) {
            snprintf(e->path, sizeof(e->path), "%s/%s", trace_dir, ent->d_name);
            struct tm tm = {0};
            tm.tm_year = e->year - 1900;
            tm.tm_mon  = e->month - 1;
            tm.tm_mday = e->day;
            tm.tm_hour = e->hour;
            tm.tm_isdst = -1;
            time_t t = mktime(&tm);
            e->start_wall_ns = (uint64_t)t * 1000000000ULL;
            n++;
        } else if (strcmp(ent->d_name, "current.summary") == 0) {
            snprintf(e->path, sizeof(e->path), "%s/%s", trace_dir, ent->d_name);
            e->year = e->month = e->day = e->hour = 0;
            e->start_wall_ns = 0;
            FILE *fp = fopen(e->path, "rb");
            if (fp) {
                struct pgwt_trace_file_header hdr;
                if (fread(&hdr, sizeof(hdr), 1, fp) == 1 &&
                    hdr.magic == PGWT_SUMMARY_MAGIC) {
                    e->start_wall_ns = hdr.start_time_ns;
                }
                fclose(fp);
            }
            if (e->start_wall_ns > 0)
                n++;
        }
    }
    closedir(dir);

    /* Sort by start_wall_ns ascending */
    for (int i = 1; i < n; i++) {
        struct pgwt_summary_file_entry tmp = entries[i];
        int j = i - 1;
        while (j >= 0 && entries[j].start_wall_ns > tmp.start_wall_ns) {
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = tmp;
    }

    return n;
}

/* ── Bulk load ─────────────────────────────────────────── */

int pgwt_load_summaries(const char *trace_dir,
                         uint64_t from_wall_ns, uint64_t to_wall_ns,
                         struct pgwt_summary_accum **out)
{
    *out = NULL;

    /* Scan available summary files */
    struct pgwt_summary_file_entry files[256];
    int nfiles = pgwt_scan_summary_files(trace_dir, files, 256);
    if (nfiles <= 0)
        return nfiles;

    /* Estimate max records: up to 3600 per file × nfiles */
    int max_records = nfiles * 3600;
    struct pgwt_summary_accum *records = malloc(max_records * sizeof(*records));
    if (!records) return -1;

    int total = 0;

    for (int f = 0; f < nfiles; f++) {
        /* Quick range check: skip files obviously outside range */
        uint64_t file_end_ns = files[f].start_wall_ns + 3600ULL * 1000000000ULL;
        if (to_wall_ns > 0 && files[f].start_wall_ns > to_wall_ns)
            continue;
        if (from_wall_ns > 0 && file_end_ns < from_wall_ns)
            continue;

        struct pgwt_summary_reader reader;
        if (pgwt_summary_reader_open(&reader, files[f].path) != 0)
            continue;

        /* Find starting block */
        int start_blk = 0;
        if (from_wall_ns > 0)
            start_blk = pgwt_summary_reader_find_block(&reader, from_wall_ns);

        for (int b = start_blk; b < reader.num_blocks; b++) {
            /* Check if block is past the end of range */
            if (to_wall_ns > 0 &&
                reader.block_index[b].timestamp_ns > to_wall_ns)
                break;

            if (total >= max_records) {
                /* Grow array */
                max_records *= 2;
                struct pgwt_summary_accum *tmp =
                    realloc(records, max_records * sizeof(*records));
                if (!tmp) {
                    pgwt_summary_reader_close(&reader);
                    *out = records;
                    return total;
                }
                records = tmp;
            }

            if (pgwt_summary_reader_decode_block(&reader, b, &records[total]) == 0) {
                /* Filter by exact time range */
                uint64_t rec_ns = records[total].second_wall_ns;
                if (from_wall_ns > 0 && rec_ns < from_wall_ns)
                    continue;
                if (to_wall_ns > 0 && rec_ns > to_wall_ns)
                    continue;
                total++;
            }
        }

        pgwt_summary_reader_close(&reader);
    }

    *out = records;
    return total;
}
