/* sampler.h — Sampled capture provider (Track A, D1)
 *
 * Pure-userspace ASH-style sampling: on each timer tick read every tracked
 * backend's PGPROC->wait_event_info with a SINGLE process_vm_readv() call
 * (one remote iovec per backend, 4 bytes each), turn each non-CPU reading
 * into a SAMPLES record, and hand the batch to the trace writer via
 * pgwt_writer_push_samples(). No BPF in the per-sample hot path; PostgreSQL
 * executes zero extra instructions.
 *
 * Addresses come from the backend registry (pid -> resolved wait_event_info
 * address, the same field the watchpoint tier resolves). query_id per sample
 * is joined from the BPF state_map (the on_report_query_id uprobe maintains
 * pid->query_id) — never read from PG memory in the sample path.
 *
 * Robustness: PG's main shm is inherited anonymous mmap, so the address is
 * identical across backends and one process_vm_readv from any live pid would
 * suffice — but we issue per-pid local/remote iovec pairs so a single
 * concurrently-exiting backend that returns EFAULT only loses its own entry.
 * On a short read we fall back to per-pid pread() of /proc/<pid>/mem for the
 * affected entries instead of aborting the whole tick.
 */
#ifndef PGWT_SAMPLER_H
#define PGWT_SAMPLER_H

#include "pg_wait_tracer.h"

#include <stdint.h>
#include <sys/types.h>

struct pgwt_daemon;
struct pgwt_metrics;

/* A registry snapshot the sampler reads each tick. Decouples the sampler's
 * core logic (testable without the daemon) from the live backend table:
 * the daemon fills this from d->backends, but the unit test fills it from a
 * fake registry. */
struct pgwt_sample_target {
    pid_t    pid;
    uint64_t wait_event_addr; /* PGPROC->wait_event_info address */
    uint64_t query_id;        /* joined from the registry/state_map */
};

struct pgwt_sampler {
    /* sample_period_ns is recorded in each SAMPLES block header (A3 weights
     * each sample by it). Equal to 1e9 / sample_rate_hz. */
    uint64_t sample_period_ns;

    /* Scratch buffers sized for MAX_BACKENDS, allocated once. */
    struct iovec *local_iov;     /* MAX_BACKENDS */
    struct iovec *remote_iov;    /* MAX_BACKENDS */
    uint32_t     *read_vals;     /* MAX_BACKENDS — wait_event_info per target */
    struct pgwt_trace_event *samples; /* MAX_BACKENDS — encoded batch */

    uint64_t samples_total;       /* cumulative SAMPLES records written */
    uint64_t read_faults_total;   /* per-pid pread fallbacks taken */
};

/* Provider hooks (see provider.h). Exposed for the vtable in sampler.c. */
int  pgwt_sampler_start(struct pgwt_daemon *d);
int  pgwt_sampler_stop(struct pgwt_daemon *d);
int  pgwt_sampler_poll(struct pgwt_daemon *d);
void pgwt_sampler_metrics(struct pgwt_daemon *d, struct pgwt_metrics *m);

/* ── Exposed for unit testing (no daemon, no BPF) ─────────────────────── */

/* Read each target's wait_event_info from process memory. Issues one
 * process_vm_readv() with up to `n` iovec pairs; on a short read, retries
 * the missing entries with per-pid pread(/proc/<pid>/mem). Writes results
 * into out_vals[i] (0 left in place for entries that could not be read).
 * read_faults (may be NULL) is incremented once per pread fallback taken.
 * Returns the number of entries successfully read. */
int pgwt_sampler_read_targets(const struct pgwt_sample_target *targets, int n,
                              uint32_t *out_vals, uint64_t *read_faults);

/* Build the SAMPLES batch from targets + their freshly-read values. A target
 * whose value is on-CPU (event 0) is skipped (no wait to record). `tick_ts`
 * is the sample timestamp stamped on every record. Returns the number of
 * sample records written into out (<= n). */
int pgwt_sampler_build_batch(const struct pgwt_sample_target *targets,
                             const uint32_t *vals, int n, uint64_t tick_ts,
                             struct pgwt_trace_event *out);

#endif /* PGWT_SAMPLER_H */
