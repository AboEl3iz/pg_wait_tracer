/* test_replay_fidelity.c — T1/FID-5: --replay is fidelity-aware.
 *
 * Before the fix, pgwt_replay_events consumed old_event/duration_ns
 * blindly: decoded SAMPLES records (old_event = 0, duration = 0) inflated
 * the CPU bucket's count while contributing zero time — a tiered trace
 * replayed as a near-idle database — and markers created junk "Unknown"
 * entries in every accumulator table.
 *
 * This test writes a synthetic trace (transitions with exec + escalation
 * markers, then a SAMPLES block) through the real writer, decodes it with
 * the real reader, replays it through the real accumulation, and asserts:
 *   - exact waits accumulate exactly (count, total, histogram);
 *   - each sample is worth one sample_period_ns of estimated time for
 *     counts / time model / per-query totals;
 *   - samples contribute NOTHING to histograms/min/max (no fabricated
 *     latencies) and do not inflate the CPU bucket;
 *   - markers are skipped everywhere (no marker-id event entries, no
 *     PGWT_ESC_PACK query ids);
 *   - the replay stats report what was accumulated (for the loud notice).
 *
 * Links the BPF-free accumulator core (map_reader.c with -DPGWT_SERVER)
 * plus the daemon-flavor event_reader.c — no bpftool needed.
 */
#include "event_reader.h"
#include "event_writer.h"
#include "map_reader.h"
#include "wait_event.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define IO_READ  0x0A000015u   /* IO:DataFileRead */
#define LOCK_REL 0x03000000u   /* Lock:relation */

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); } \
    else      { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

#define CHECK_EQ_U64(got, want, msg) do { \
    uint64_t g_ = (got), w_ = (want); \
    if (g_ == w_) { printf("  PASS: %s\n", msg); } \
    else { printf("  FAIL: %s (got %llu, want %llu)\n", msg, \
                  (unsigned long long)g_, (unsigned long long)w_); \
           failures++; } \
} while (0)

static struct pgwt_trace_event ev(uint32_t pid, uint64_t ts, uint64_t dur,
                                  uint32_t old_e, uint32_t new_e,
                                  uint64_t qid)
{
    struct pgwt_trace_event e = {0};
    e.pid = pid;
    e.timestamp_ns = ts;
    e.duration_ns = dur;
    e.old_event = old_e;
    e.new_event = new_e;
    e.query_id = qid;
    return e;
}

static const struct pgwt_event_stats *
find_sys(const struct pgwt_accumulator *acc, uint32_t we)
{
    for (int i = 0; i < acc->num_system_events; i++)
        if (acc->system_events[i].wait_event == we &&
            acc->system_events[i].count > 0)
            return &acc->system_events[i];
    return NULL;
}

int main(void)
{
    char dir[] = "/tmp/pgwt_replay_fid_XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }

    const uint64_t BASE = 10000000000000ULL;   /* 10000 s */
    const uint64_t MS = 1000000ULL;
    const uint64_t PERIOD = 100 * MS;          /* 100 ms */

    /* ── Write the trace ─────────────────────────────────── */
    struct pgwt_event_writer w;
    if (pgwt_writer_init(&w, dir, 18, 24, NULL) != 0) {
        fprintf(stderr, "writer init failed\n");
        return 1;
    }

    struct pgwt_trace_event trans[] = {
        /* escalation START marker (pid 0, ESC_PACK payload) */
        ev(0, BASE, 0, PGWT_MARKER_ESCALATE_START, PGWT_MARKER_ESCALATE_START,
           PGWT_ESC_PACK(60, 1)),
        /* exec markers around two real IO waits of 10 ms */
        ev(1000, BASE + 1 * MS, 0, PGWT_MARKER_EXEC_START,
           PGWT_MARKER_EXEC_START, 100),
        ev(1000, BASE + 20 * MS, 10 * MS, IO_READ, 0, 100),
        ev(1000, BASE + 40 * MS, 10 * MS, IO_READ, 0, 100),
        ev(1000, BASE + 41 * MS, 0, PGWT_MARKER_EXEC_END,
           PGWT_MARKER_EXEC_END, 100),
        /* escalation END marker */
        ev(0, BASE + 50 * MS, 0, PGWT_MARKER_ESCALATE_END,
           PGWT_MARKER_ESCALATE_END, PGWT_ESC_PACK(60, 2)),
    };
    for (size_t i = 0; i < sizeof(trans) / sizeof(trans[0]); i++)
        pgwt_writer_push_event(&w, &trans[i]);

    /* 5 Lock samples for pid 1001 / query 200 at 100 ms period */
    struct pgwt_trace_event smp[5];
    for (int i = 0; i < 5; i++) {
        smp[i] = ev(1001, BASE + 100 * MS + (uint64_t)i * PERIOD, 0,
                    0, LOCK_REL, 200);
    }
    pgwt_writer_push_samples(&w, smp, 5, PERIOD);
    pgwt_writer_close(&w);
    pgwt_writer_destroy(&w);

    /* ── Read + replay ───────────────────────────────────── */
    char trace_path[600];
    snprintf(trace_path, sizeof(trace_path), "%s/current.trace", dir);

    struct pgwt_event_reader r;
    if (pgwt_reader_open(&r, trace_path) != 0) {
        fprintf(stderr, "reader open failed\n");
        return 1;
    }
    CHECK(r.num_blocks >= 2, "trace has transitions + samples blocks");

    struct pgwt_accumulator *acc = malloc(sizeof(*acc));
    pgwt_accum_init(acc);
    struct pgwt_replay_stats st = {0};
    struct pgwt_trace_event *buf = malloc(PGWT_BLOCK_EVENTS * sizeof(*buf));

    for (int b = 0; b < r.num_blocks; b++) {
        struct pgwt_block_info bi;
        int n = pgwt_reader_decode_block_info(&r, b, buf,
                                              PGWT_BLOCK_EVENTS, &bi);
        if (n <= 0) continue;
        pgwt_replay_events(acc, buf, n, 0, 0, &bi, &st);
    }
    pgwt_reader_close(&r);

    /* ── Assertions ──────────────────────────────────────── */

    /* Replay stats (drive the loud fidelity notice) */
    CHECK_EQ_U64(st.transitions, 2, "2 exact transitions accumulated");
    CHECK_EQ_U64(st.samples, 5, "5 samples accumulated");
    CHECK_EQ_U64(st.markers_skipped, 4, "4 markers skipped");
    CHECK_EQ_U64(st.sample_period_ns, PERIOD, "sample period reported");

    /* Exact IO wait: full stats including histogram */
    const struct pgwt_event_stats *io = find_sys(acc, IO_READ);
    CHECK(io != NULL, "IO:DataFileRead entry exists");
    if (io) {
        CHECK_EQ_U64(io->count, 2, "IO count = 2");
        CHECK_EQ_U64(io->total_ns, 20 * MS, "IO total = 20 ms (exact)");
        uint64_t hist_total = 0;
        for (int i = 0; i < HISTOGRAM_BUCKETS; i++)
            hist_total += io->histogram[i];
        CHECK_EQ_U64(hist_total, 2, "IO histogram fed by exact records");
        CHECK_EQ_U64(io->max_ns, 10 * MS, "IO max = 10 ms");
    }

    /* Sampled Lock wait: ASH-estimated time, NO histogram/min/max */
    const struct pgwt_event_stats *lk = find_sys(acc, LOCK_REL);
    CHECK(lk != NULL, "Lock:relation entry exists (samples visible)");
    if (lk) {
        CHECK_EQ_U64(lk->count, 5, "Lock count = 5 samples");
        CHECK_EQ_U64(lk->total_ns, 5 * PERIOD,
                     "Lock total = 5 x period (ASH estimate, not 0)");
        uint64_t hist_total = 0;
        for (int i = 0; i < HISTOGRAM_BUCKETS; i++)
            hist_total += lk->histogram[i];
        CHECK_EQ_U64(hist_total, 0,
                     "Lock histogram EMPTY (no fabricated latencies)");
        CHECK_EQ_U64(lk->max_ns, 0, "Lock max not fabricated");
    }

    /* Samples must NOT be misread as CPU (old_event = 0 on the wire) */
    const struct pgwt_event_stats *cpu = find_sys(acc, 0);
    CHECK(cpu == NULL, "no phantom CPU entries from sample records");

    /* Markers create no event entries and no phantom query ids */
    int marker_entries = 0;
    for (int i = 0; i < acc->num_system_events; i++)
        if (PGWT_IS_MARKER(acc->system_events[i].wait_event) &&
            acc->system_events[i].count > 0)
            marker_entries++;
    CHECK_EQ_U64(marker_entries, 0, "no marker entries in system events");

    int esc_qids = 0;
    uint64_t q100_ns = 0, q200_ns = 0;
    for (int i = 0; i < acc->num_query_events; i++) {
        const struct pgwt_query_event_stats *qe = &acc->query_events[i];
        if (qe->count == 0) continue;
        if (qe->query_id == PGWT_ESC_PACK(60, 1) ||
            qe->query_id == PGWT_ESC_PACK(60, 2))
            esc_qids++;
        if (qe->query_id == 100) q100_ns += qe->total_ns;
        if (qe->query_id == 200) q200_ns += qe->total_ns;
    }
    CHECK_EQ_U64(esc_qids, 0, "no PGWT_ESC_PACK phantom query ids");
    CHECK_EQ_U64(q100_ns, 20 * MS, "query 100 total = exact 20 ms");
    CHECK_EQ_U64(q200_ns, 5 * PERIOD, "query 200 total = 5 x period");

    /* Time model: lock class = sampled estimate, io class = exact */
    CHECK_EQ_U64(acc->tm.lock_time_ns, 5 * PERIOD,
                 "time model Lock = 500 ms (sampled, was 0 before fix)");
    CHECK_EQ_U64(acc->tm.io_time_ns, 20 * MS, "time model IO = 20 ms");
    CHECK_EQ_U64(acc->tm.db_time_ns, 20 * MS + 5 * PERIOD,
                 "time model DB time = exact + sampled");

    free(buf);
    free(acc);

    /* cleanup */
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) { /* best effort */ }

    printf("%s\n", failures == 0 ? "ALL PASSED" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
