/* sampler.c — Sampled capture provider (Track A, D1)
 *
 * See sampler.h for the design. The core read/build logic
 * (pgwt_sampler_read_targets / pgwt_sampler_build_batch) is free of BPF and
 * daemon dependencies so the unit test can exercise it against a controlled
 * target process. The provider hooks (start/stop/poll/metrics) bridge to the
 * live daemon: they build the per-tick target list from the backend registry
 * + BPF state_map and push the batch into the trace writer.
 *
 * The provider vtable itself lives at the bottom and is compiled into both
 * the daemon (with BPF) and never into pgwt-server. The BPF-touching parts
 * are guarded by !PGWT_SERVER so the unit test can link the BPF-free core.
 */
#define _GNU_SOURCE   /* process_vm_readv */
#include "sampler.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/uio.h>

/* ── BPF-free core (testable without a daemon) ────────────────────────── */

int pgwt_sampler_read_targets(const struct pgwt_sample_target *targets, int n,
                              uint32_t *out_vals, uint64_t *read_faults)
{
    if (n <= 0)
        return 0;

    struct iovec *liov = calloc(n, sizeof(*liov));
    struct iovec *riov = calloc(n, sizeof(*riov));
    if (!liov || !riov) {
        free(liov);
        free(riov);
        return 0;
    }

    for (int i = 0; i < n; i++) {
        out_vals[i] = 0;
        liov[i].iov_base = &out_vals[i];
        liov[i].iov_len  = sizeof(uint32_t);
        riov[i].iov_base = (void *)(uintptr_t)targets[i].wait_event_addr;
        riov[i].iov_len  = sizeof(uint32_t);
    }

    /* All backends share PG's inherited anonymous-mmap shm, so every
     * wait_event_info lives in the *same* address space — reading them via a
     * single pid in one syscall is correct and cheapest. We read via the
     * first target's pid; the remote iovecs point at every backend's
     * (identical-across-processes) address. process_vm_readv reads in order
     * and returns the number of bytes successfully transferred, so a partial
     * result tells us exactly how many leading entries landed. */
    pid_t reader_pid = targets[0].pid;
    int got = 0;
    ssize_t r = process_vm_readv(reader_pid, liov, n, riov, n, 0);

    if (r == (ssize_t)(n * sizeof(uint32_t))) {
        got = n;
    } else if (r > 0) {
        /* Partial: the first (r / 4) entries are valid; the entry that
         * faulted and everything after it must be retried per-pid. */
        got = (int)(r / sizeof(uint32_t));
    }
    /* r <= 0 (e.g. reader_pid exited between registry snapshot and syscall):
     * got stays 0 and we fall through to the per-pid path for all entries. */

    /* Per-pid fallback for the unread tail. Each is read against its OWN pid
     * (the shared address still resolves there) so one dead backend can't
     * sink the rest. */
    for (int i = got; i < n; i++) {
        uint32_t val = 0;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/mem", targets[i].pid);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            if (pread(fd, &val, sizeof(val),
                      (off_t)targets[i].wait_event_addr) == (ssize_t)sizeof(val)) {
                out_vals[i] = val;
                got++;
            }
            close(fd);
        }
        if (read_faults)
            (*read_faults)++;
    }

    free(liov);
    free(riov);
    return got;
}

int pgwt_sampler_build_batch(const struct pgwt_sample_target *targets,
                             const uint32_t *vals, int n, uint64_t tick_ts,
                             struct pgwt_trace_event *out)
{
    int count = 0;
    for (int i = 0; i < n; i++) {
        uint32_t we = vals[i];
        /* event 0 == on CPU: not a wait, no sample to record (ASH counts
         * the active session via its non-idle waits and CPU separately;
         * A3 derives CPU from coverage, not from on-CPU samples). */
        if (we == 0)
            continue;

        struct pgwt_trace_event *e = &out[count++];
        e->timestamp_ns = tick_ts;
        e->pid          = (uint32_t)targets[i].pid;
        e->old_event    = 0;       /* samples carry no previous state */
        e->new_event    = we;      /* the sampled wait event */
        e->flags        = 0;       /* SAMPLE flag is set by the reader */
        e->duration_ns  = 0;       /* samples carry no duration */
        e->query_id     = targets[i].query_id;
    }
    return count;
}

/* ── Daemon-side provider hooks (need the BPF skeleton) ───────────────── */

#ifndef PGWT_SERVER

#include "daemon.h"
#include "backend.h"
#include "event_writer.h"
#include "discovery.h"

#include <time.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "pg_wait_tracer.skel.h"

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Read pid -> query_id from the BPF state_map, maintained by the
 * on_report_query_id uprobe. Returns 0 if unknown. */
static uint64_t lookup_query_id(struct pgwt_daemon *d, pid_t pid)
{
    if (!d->skel)
        return 0;
    int fd = bpf_map__fd(d->skel->maps.state_map);
    if (fd < 0)
        return 0;
    uint32_t key = (uint32_t)pid;
    struct pgwt_pid_state st;
    if (bpf_map_lookup_elem(fd, &key, &st) == 0)
        return st.last_query_id;
    return 0;
}

/* Seed an empty state_map entry for a sampled-mode backend. The
 * on_report_query_id uprobe only UPDATES existing entries (it never creates
 * them), so without a seed the sampler would never see a query_id — there is
 * no on_watchpoint in sampled mode to create the entry. Idempotent
 * (BPF_NOEXIST). */
static void seed_state_entry(struct pgwt_daemon *d, pid_t pid, uint64_t addr)
{
    if (!d->skel)
        return;
    int fd = bpf_map__fd(d->skel->maps.state_map);
    if (fd < 0)
        return;
    uint32_t key = (uint32_t)pid;
    struct pgwt_pid_state st = {
        .last_event = 0,
        .last_ts = mono_ns(),
        .last_query_id = 0,
        .wait_event_addr = addr,
    };
    bpf_map_update_elem(fd, &key, &st, BPF_NOEXIST);
}

int pgwt_sampler_start(struct pgwt_daemon *d)
{
    struct pgwt_sampler *s = calloc(1, sizeof(*s));
    if (!s) {
        fprintf(stderr, "FATAL: cannot allocate sampler state\n");
        return -1;
    }

    int hz = d->sample_rate_hz > 0 ? d->sample_rate_hz : 10;
    s->sample_period_ns = 1000000000ULL / (uint64_t)hz;

    s->read_vals = calloc(MAX_BACKENDS, sizeof(*s->read_vals));
    s->samples   = calloc(MAX_BACKENDS, sizeof(*s->samples));
    if (!s->read_vals || !s->samples) {
        free(s->read_vals);
        free(s->samples);
        free(s);
        fprintf(stderr, "FATAL: cannot allocate sampler buffers\n");
        return -1;
    }

    d->sampler = s;
    if (d->verbose)
        fprintf(stderr, "INFO: sampler started: %d Hz (period %llu ns)\n",
                hz, (unsigned long long)s->sample_period_ns);
    return 0;
}

int pgwt_sampler_stop(struct pgwt_daemon *d)
{
    struct pgwt_sampler *s = d->sampler;
    if (!s)
        return 0;
    free(s->read_vals);
    free(s->samples);
    free(s);
    d->sampler = NULL;
    return 0;
}

/* One sampling tick. Build the target list from the live registry, read all
 * wait_event_info in one syscall (+ per-pid fallback), encode non-CPU
 * readings, and push a SAMPLES block. Called from the daemon timer. */
int pgwt_sampler_poll(struct pgwt_daemon *d)
{
    struct pgwt_sampler *s = d->sampler;
    if (!s)
        return 0;

    /* Reuse a stack target array sized to the live count; MAX_BACKENDS cap. */
    static struct pgwt_sample_target targets[MAX_BACKENDS];
    int n = 0;
    for (int i = 0; i < d->backends.count && n < MAX_BACKENDS; i++) {
        struct pgwt_backend *be = &d->backends.entries[i];
        if (!be->is_alive || be->pid <= 0)
            continue;
        /* In sampled mode no bootstrap watchpoint resolves wp_addr, so a
         * freshly-forked backend arrives with wp_addr == 0. Lazily resolve
         * it by dereferencing the my_wait_event_info global in that backend
         * (same VA in every backend; the deref gives its own PGPROC slot).
         * Skip this tick if the backend hasn't set the pointer yet. */
        if (be->wp_addr == 0) {
            be->wp_addr = pgwt_read_pointer(be->pid, d->my_wait_ptr_addr);
            if (be->wp_addr == 0)
                continue;
            /* First time we resolved this backend — seed its state_map entry
             * so the query_id uprobe can populate it. */
            seed_state_entry(d, be->pid, be->wp_addr);
        }
        targets[n].pid             = be->pid;
        targets[n].wait_event_addr = be->wp_addr;
        targets[n].query_id        = lookup_query_id(d, be->pid);
        n++;
    }
    if (n == 0)
        return 0;

    uint64_t tick_ts = mono_ns();
    uint64_t faults_before = s->read_faults_total;
    pgwt_sampler_read_targets(targets, n, s->read_vals, &s->read_faults_total);
    d->counters.sample_read_faults_total +=
        (s->read_faults_total - faults_before);

    int count = pgwt_sampler_build_batch(targets, s->read_vals, n, tick_ts,
                                         s->samples);
    if (count == 0)
        return 0;

    if (d->event_writer)
        pgwt_writer_push_samples(d->event_writer, s->samples, count,
                                 s->sample_period_ns);

    s->samples_total += (uint64_t)count;
    d->counters.samples_total += (uint64_t)count;
    return 0;
}

void pgwt_sampler_metrics(struct pgwt_daemon *d, struct pgwt_metrics *m)
{
    if (d->sampler) {
        m->samples_total      = d->sampler->samples_total;
        m->sample_read_faults = d->sampler->read_faults_total;
    }
}

/* ── Provider vtables ─────────────────────────────────────────────────── */

const struct pgwt_capture_provider pgwt_provider_sampled = {
    .name         = "sampled",
    .fidelity     = PGWT_FIDELITY_SAMPLED,
    .start        = pgwt_sampler_start,
    .stop         = pgwt_sampler_stop,
    .poll         = pgwt_sampler_poll,
    .self_metrics = pgwt_sampler_metrics,
};

#endif /* !PGWT_SERVER */
