/* gen_bench_traces.c — Generate large trace files for performance benchmarking
 *
 * Usage: gen_bench_traces -o <output-dir> -n <num_events> [-p <num_pids>]
 *
 * Generates a realistic mix of wait events across multiple PIDs with
 * varying durations. Used by bench_server.py to measure compute throughput.
 */
#include "event_writer.h"
#include "pg_wait_tracer.h"
#include "wait_event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

/* Realistic event mix based on pgbench TPC-B profile */
static const uint32_t event_pool[] = {
    0,                  /* CPU */
    0,                  /* CPU (weighted more) */
    0,                  /* CPU */
    0x0A000015,         /* IO:DataFileRead */
    0x0A000018,         /* IO:DataFileWrite */
    0x0A00004B,         /* IO:WalWrite */
    0x0A00004D,         /* IO:WalSync */
    0x01000049,         /* LWLock:WALInsert */
    0x01000008,         /* LWLock:BufferContent */
    0x01000009,         /* LWLock:BufferMapping */
    0x03000008,         /* Lock:transactionid */
    0x03000009,         /* Lock:tuple */
    0x06000000,         /* Client:ClientRead */
    0x08000027,         /* IPC:ProcarrayGroupUpdate */
    0x09000006,         /* Timeout:SpinDelay */
};
#define EVENT_POOL_SIZE (sizeof(event_pool) / sizeof(event_pool[0]))

/* Simple PRNG (xorshift64) */
static uint64_t rng_state = 0x12345678ABCDEF01ULL;
static uint64_t rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

int main(int argc, char **argv)
{
    const char *outdir = NULL;
    int num_events = 1000000;
    int num_pids = 8;
    int opt;

    while ((opt = getopt(argc, argv, "o:n:p:")) != -1) {
        switch (opt) {
        case 'o': outdir = optarg; break;
        case 'n': num_events = atoi(optarg); break;
        case 'p': num_pids = atoi(optarg); break;
        default:
            fprintf(stderr, "Usage: %s -o <dir> -n <events> [-p <pids>]\n", argv[0]);
            return 1;
        }
    }

    if (!outdir) {
        fprintf(stderr, "Usage: %s -o <dir> -n <events> [-p <pids>]\n", argv[0]);
        return 1;
    }

    pgwt_init_event_names(18);

    struct pgwt_event_writer w;
    int ret = pgwt_writer_init(&w, outdir, 18, 0, NULL);
    if (ret != 0) {
        fprintf(stderr, "Failed to init writer in %s\n", outdir);
        return 1;
    }

    /* Base timestamp: 10 seconds into epoch for simplicity */
    uint64_t ts = 10000000000ULL; /* 10s in ns */
    uint32_t *pids = malloc(num_pids * sizeof(uint32_t));
    uint64_t *qids = malloc(num_pids * sizeof(uint64_t));
    for (int i = 0; i < num_pids; i++) {
        pids[i] = 1000 + i;
        qids[i] = 1000000 + (rng_next() % 50); /* 50 distinct queries */
    }

    fprintf(stderr, "Generating %d events across %d PIDs...\n", num_events, num_pids);

    for (int i = 0; i < num_events; i++) {
        int pid_idx = rng_next() % num_pids;
        int ev_idx = rng_next() % EVENT_POOL_SIZE;

        /* Duration: 1us to 10ms, log-distributed */
        uint64_t dur = 1000 + (rng_next() % 10000000);

        struct pgwt_trace_event ev = {
            .timestamp_ns = ts,
            .pid = pids[pid_idx],
            .old_event = event_pool[ev_idx],
            .new_event = event_pool[rng_next() % EVENT_POOL_SIZE],
            .duration_ns = dur,
            .query_id = qids[pid_idx],
        };

        pgwt_writer_push_event(&w, &ev);
        ts += dur;

        /* Rotate query_id occasionally */
        if ((rng_next() % 100) == 0)
            qids[pid_idx] = 1000000 + (rng_next() % 50);
    }

    pgwt_writer_close(&w);
    pgwt_writer_destroy(&w);
    free(pids);
    free(qids);

    fprintf(stderr, "Done: %d events in %s\n", num_events, outdir);
    return 0;
}
