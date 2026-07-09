/* sampler.c — Sampled capture provider (Track A, D1; hardened in T4)
 *
 * See sampler.h for the design. The core read/build logic
 * (pgwt_sampler_read_targets / pgwt_sampler_build_batch) and the pure
 * helpers (health state machine, effective period, qid join index) are free
 * of BPF and daemon dependencies so the unit test can exercise them against
 * a controlled target process. The provider hooks (start/stop/poll/metrics)
 * bridge to the live daemon: they build the per-tick target list from the
 * backend registry + BPF state_map and push the batch into the trace writer.
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

/* Read one target's value against its OWN pid via /proc/<pid>/mem.
 * Returns 1 on success (val written), 0 on failure (errno preserved). */
static int read_target_own_pid(const struct pgwt_sample_target *t,
                               uint32_t *val)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", t->pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    int ok = (pread(fd, val, sizeof(*val),
                    (off_t)t->wait_event_addr) == (ssize_t)sizeof(*val));
    close(fd);
    return ok;
}

int pgwt_sampler_read_targets(const struct pgwt_sample_target *targets, int n,
                              uint32_t *out_vals, uint64_t *read_faults)
{
    if (n <= 0)
        return 0;

    struct iovec *liov = calloc(n, sizeof(*liov));
    struct iovec *riov = calloc(n, sizeof(*riov));
    int *idx = calloc(n, sizeof(*idx));   /* batch position -> target index */
    if (!liov || !riov || !idx) {
        free(liov);
        free(riov);
        free(idx);
        return 0;
    }

    int got = 0;
    int nb = 0;   /* number of batchable (shared-memory) targets */
    for (int i = 0; i < n; i++) {
        out_vals[i] = 0;
        /* SMP-2: only targets whose address is verified to live in a
         * MAP_SHARED mapping may be read through another pid. A private
         * address mapped at the same VA in every child reads SUCCESSFULLY
         * through a foreign pid and returns the READER's value —
         * misattributed, silently. Everything else goes per-pid below. */
        if (targets[i].is_shared != 1)
            continue;
        liov[nb].iov_base = &out_vals[i];
        liov[nb].iov_len  = sizeof(uint32_t);
        riov[nb].iov_base = (void *)(uintptr_t)targets[i].wait_event_addr;
        riov[nb].iov_len  = sizeof(uint32_t);
        idx[nb] = i;
        nb++;
    }

    /* Shared-memory batch: the addresses are backed by the SAME pages in
     * every backend, so one process_vm_readv through a single reader pid
     * resolves them all in one syscall. process_vm_readv reads iovecs in
     * order and stops at the first faulting entry; we sweep — read as far
     * as possible, per-pid pread the single faulting entry, then RESUME the
     * combined read for the remainder. One outlier costs one pread, not N. */
    int base = 0;   /* first not-yet-read batch position */
    while (base < nb) {
        pid_t reader_pid = targets[idx[base]].pid;
        int rem = nb - base;
        ssize_t r = process_vm_readv(reader_pid, liov + base, rem,
                                     riov + base, rem, 0);
        int done = (r > 0) ? (int)(r / sizeof(uint32_t)) : 0;
        got  += done;
        base += done;
        if (base >= nb)
            break;

        /* batch entry `base` faulted under reader_pid (or the reader itself
         * is gone). Read it against its own pid. */
        uint32_t val = 0;
        if (read_target_own_pid(&targets[idx[base]], &val)) {
            out_vals[idx[base]] = val;
            got++;
        }
        if (read_faults)
            (*read_faults)++;
        base++;   /* move past the entry we just handled (or skipped) */
    }

    /* Per-pid reads for everything not proven shared (SMP-2). */
    for (int i = 0; i < n; i++) {
        if (targets[i].is_shared == 1)
            continue;
        uint32_t val = 0;
        if (read_target_own_pid(&targets[i], &val)) {
            out_vals[i] = val;
            got++;
        }
    }

    free(liov);
    free(riov);
    free(idx);
    return got;
}

int pgwt_sampler_build_batch(const struct pgwt_sample_target *targets,
                             const uint32_t *vals, int n, uint64_t tick_ts,
                             struct pgwt_trace_event *out,
                             uint64_t *invalid_reads)
{
    int count = 0;
    for (int i = 0; i < n; i++) {
        uint32_t we = vals[i];
        /* event 0 == on CPU: not a wait, no sample to record (ASH counts
         * the active session via its non-idle waits and CPU separately;
         * A3 derives CPU from coverage, not from on-CPU samples). */
        if (we == 0)
            continue;

        /* CAP-2/5 backstop (all PG versions, every tick): a value with an
         * unknown class byte is garbage from a wrong offset/address — it
         * must NEVER be recorded as data. Count it so the daemon screams. */
        if (pgwt_classify_wei(we) == PGWT_WEI_GARBAGE) {
            if (invalid_reads)
                (*invalid_reads)++;
            continue;
        }

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

uint64_t pgwt_sampler_effective_period(uint64_t nominal_ns,
                                       uint64_t last_tick_ns,
                                       uint64_t now_ns)
{
    if (last_tick_ns == 0 || now_ns <= last_tick_ns)
        return nominal_ns;
    uint64_t elapsed = now_ns - last_tick_ns;
    if (elapsed < nominal_ns)
        return nominal_ns;   /* the timer never fires early; jitter only */
    if (elapsed > 60ULL * 1000000000ULL)
        return 60ULL * 1000000000ULL;   /* sanity clamp on absurd stalls */
    return elapsed;
}

enum pgwt_sampler_health_action
pgwt_sampler_health_note(struct pgwt_sampler_health *h, int n_targets,
                         int n_read, int err_no, uint64_t now_ns)
{
    /* Re-log period while persistently failing: once a minute. */
    const uint64_t RELOG_NS = 60ULL * 1000000000ULL;

    if (n_targets <= 0)
        return PGWT_SAMPLER_LOG_NONE;   /* nothing to read — neutral tick */

    if (n_read > 0) {
        int was_unhealthy = !h->healthy;
        h->healthy = 1;
        h->consec_failed_ticks = 0;
        return was_unhealthy ? PGWT_SAMPLER_LOG_RECOVERED
                             : PGWT_SAMPLER_LOG_NONE;
    }

    /* Total failure: N targets, zero readable. Indistinguishable from an
     * idle database in the data — must be loud out-of-band (SMP-1). */
    h->consec_failed_ticks++;
    h->failed_ticks_total++;
    h->last_errno = err_no;
    h->healthy = 0;
    if (h->consec_failed_ticks == 1 || now_ns - h->last_log_ns >= RELOG_NS) {
        h->last_log_ns = now_ns;
        return PGWT_SAMPLER_LOG_DEGRADED;
    }
    return PGWT_SAMPLER_LOG_NONE;
}

/* ── SMP-4: pid -> query_id join index ────────────────────────────────── */

static int qid_entry_cmp(const void *a, const void *b)
{
    uint32_t pa = ((const struct pgwt_qid_entry *)a)->pid;
    uint32_t pb = ((const struct pgwt_qid_entry *)b)->pid;
    return (pa > pb) - (pa < pb);
}

void pgwt_qid_index_sort(struct pgwt_qid_entry *entries, int n)
{
    qsort(entries, (size_t)n, sizeof(*entries), qid_entry_cmp);
}

uint64_t pgwt_qid_index_lookup(const struct pgwt_qid_entry *entries, int n,
                               uint32_t pid)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (entries[mid].pid == pid)
            return entries[mid].query_id;
        if (entries[mid].pid < pid)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

/* ── Daemon-side provider hooks (need the BPF skeleton) ───────────────── */

#ifndef PGWT_SERVER

#include "daemon.h"
#include "backend.h"
#include "event_writer.h"
#include "discovery.h"
#include "map_reader.h"
#include "wait_event.h"

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
 * on_report_query_id uprobe. Returns 0 if unknown. Per-pid fallback for
 * kernels without BPF_MAP_LOOKUP_BATCH (SMP-4). */
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

/* SMP-4: dump the whole state_map in (at most) a few BPF_MAP_LOOKUP_BATCH
 * syscalls and build a sorted pid->query_id index, instead of one
 * bpf_map_lookup_elem syscall per backend per tick (10k syscalls/s at
 * 1000 backends × 10 Hz). Returns the number of index entries, or -1 when
 * batch lookup is unsupported (caller falls back to per-pid lookups). */
static int dump_qid_index(struct pgwt_daemon *d, struct pgwt_sampler *s,
                          struct pgwt_qid_entry *out)
{
    if (!d->skel || s->qid_batch_supported == 0)
        return -1;
    int fd = bpf_map__fd(d->skel->maps.state_map);
    if (fd < 0)
        return -1;

    uint32_t in_batch = 0, out_batch = 0;
    void *in = NULL;
    int total = 0;
    while (total < s->qid_cap) {
        uint32_t count = (uint32_t)(s->qid_cap - total);
        int err = bpf_map_lookup_batch(fd, in, &out_batch,
                                       s->qid_keys + total,
                                       s->qid_vals + total, &count, NULL);
        if (err == 0 || err == -ENOENT) {
            total += (int)count;
            if (err == -ENOENT)
                break;   /* map exhausted */
            in_batch = out_batch;
            in = &in_batch;
            continue;
        }
        if (total == 0) {
            /* First call failed outright: kernel lacks batch support
             * (EINVAL/ENOTSUPP...). Remember and fall back per-pid. */
            s->qid_batch_supported = 0;
            if (d->verbose)
                fprintf(stderr, "INFO: BPF_MAP_LOOKUP_BATCH unavailable "
                        "(%s) — using per-pid query_id lookups\n",
                        strerror(-err));
            return -1;
        }
        break;   /* partial dump: use what we have */
    }
    s->qid_batch_supported = 1;

    for (int i = 0; i < total; i++) {
        out[i].pid = s->qid_keys[i];
        out[i].query_id = s->qid_vals[i].last_query_id;
    }
    pgwt_qid_index_sort(out, total);
    return total;
}

/* Seed an empty state_map entry for a sampled-mode backend. The
 * on_report_query_id uprobe only UPDATES existing entries (it never creates
 * them), so without a seed the sampler would never see a query_id — there is
 * no on_watchpoint in sampled mode to create the entry. Idempotent
 * (BPF_NOEXIST). CAP-1: a full map here means this backend's samples lose
 * query attribution — counted + logged loudly, never silent. */
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
    int rc = bpf_map_update_elem(fd, &key, &st, BPF_NOEXIST);
    if (rc != 0 && rc != -EEXIST && errno != EEXIST) {
        d->counters.state_map_full_total++;
        if (!d->state_map_full_logged) {
            d->state_map_full_logged = true;
            fprintf(stderr,
                    "ERROR: BPF state_map is FULL — cannot seed PID %d (and "
                    "any further backend).\n"
                    "  Affected backends' samples lose query attribution. "
                    "state_map_full_total on the control socket counts every "
                    "affected backend.\n",
                    pid);
        }
    }
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
    s->health.healthy = 1;
    s->qid_batch_supported = -1;

    /* qid dump buffers sized to the LOADED state_map capacity (it can be
     * shrunk via PGWT_STATE_MAP_ENTRIES in test builds). */
    s->qid_cap = MAX_BACKENDS;
    if (d->skel) {
        uint32_t me = bpf_map__max_entries(d->skel->maps.state_map);
        if (me > 0 && (int)me < s->qid_cap)
            s->qid_cap = (int)me;
    }

    s->read_vals = calloc(MAX_BACKENDS, sizeof(*s->read_vals));
    s->samples   = calloc(MAX_BACKENDS, sizeof(*s->samples));
    s->qid_keys  = calloc(s->qid_cap, sizeof(*s->qid_keys));
    s->qid_vals  = calloc(s->qid_cap, sizeof(*s->qid_vals));
    if (!s->read_vals || !s->samples || !s->qid_keys || !s->qid_vals) {
        free(s->read_vals);
        free(s->samples);
        free(s->qid_keys);
        free(s->qid_vals);
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
    free(s->qid_keys);
    free(s->qid_vals);
    free(s);
    d->sampler = NULL;
    return 0;
}

/* Fold one sample batch into the live in-process accumulator so the running
 * --view output reflects sampled-mode data in real time.
 *
 * In full/lightweight mode the event_ringbuf consumer (pgwt_handle_trace_event)
 * is what populates d->event_accum, which the timer copies into d->accum for
 * display. Sampled mode has no ringbuf consumer — the sampler is the data
 * source — so without this step the live view stays empty ("(no data yet)")
 * even though samples are correctly captured and written to the trace file.
 *
 * Each sample is an ASH point observation worth one sample period of wall time;
 * we weight it by period_ns exactly as the offline reader/server do
 * (event_reader.c sets FLAG_SAMPLE; server.c normalizes old_event/duration_ns).
 * The build_batch records keep their on-disk shape (new_event set, old/duration
 * zero) so the trace-file format is unchanged; we read new_event here and apply
 * the per-sample weight locally. */
static void pgwt_sampler_accumulate(struct pgwt_daemon *d,
                                    const struct pgwt_trace_event *samples,
                                    int count, uint64_t period_ns)
{
    struct pgwt_accumulator *acc = d->event_accum;
    if (!acc)
        return;

    for (int i = 0; i < count; i++) {
        uint32_t we  = samples[i].new_event;   /* sampled wait event */
        uint64_t dur = period_ns;              /* ASH weight per sample */

        if (PGWT_IS_MARKER(we))
            continue;

        /* Per-PID accumulation */
        struct pgwt_pid_accum *pa = pgwt_get_or_create_pid(acc, samples[i].pid);
        if (pa) {
            struct pgwt_event_stats *es = pgwt_get_or_create_event(pa, we);
            if (es) {
                es->count++;
                es->total_ns += dur;
                if (dur < es->min_ns) es->min_ns = dur;
                if (dur > es->max_ns) es->max_ns = dur;
                uint32_t bucket = pgwt_duration_to_bucket(dur);
                if (bucket < HISTOGRAM_BUCKETS)
                    es->histogram[bucket]++;
            }
            if (!pgwt_is_idle_event(we)) {
                pa->wait_time_ns += dur;
                pa->db_time_ns   += dur;
            }
        }

        /* System-wide accumulation */
        struct pgwt_event_stats *se = pgwt_get_or_create_system_event(acc, we);
        if (se) {
            se->count++;
            se->total_ns += dur;
            if (dur < se->min_ns) se->min_ns = dur;
            if (dur > se->max_ns) se->max_ns = dur;
            uint32_t bucket = pgwt_duration_to_bucket(dur);
            if (bucket < HISTOGRAM_BUCKETS)
                se->histogram[bucket]++;
        }

        /* Time model by class */
        pgwt_update_time_model(&acc->tm, we, dur);

        /* Query attribution */
        if (samples[i].query_id != 0) {
            struct pgwt_query_event_stats *qe =
                pgwt_get_or_create_query_event(acc, samples[i].query_id, we);
            if (qe) {
                qe->count++;
                qe->total_ns += dur;
                if (dur < qe->min_ns) qe->min_ns = dur;
                if (dur > qe->max_ns) qe->max_ns = dur;
            }
        }
    }
}

/* One sampling tick. Build the target list from the live registry, read all
 * wait_event_info (shared-memory batch + per-pid fallbacks), encode non-CPU
 * readings, and push a SAMPLES block. Called from the daemon timer. */
int pgwt_sampler_poll(struct pgwt_daemon *d)
{
    struct pgwt_sampler *s = d->sampler;
    if (!s)
        return 0;

    uint64_t tick_ts = mono_ns();
    /* SMP-3: weight this tick by the MEASURED inter-tick elapsed time, not
     * the nominal period — when the daemon stalls (attach storms, load) and
     * timer expirations coalesce, the nominal weight would silently deflate
     * AAS/DB-Time exactly when it matters. The SAMPLES block header carries
     * the per-block period, so the on-disk format is unchanged. */
    uint64_t period_ns = pgwt_sampler_effective_period(s->sample_period_ns,
                                                       s->last_tick_ns,
                                                       tick_ts);
    s->last_tick_ns = tick_ts;

    /* SMP-4: one batched state_map dump per tick for the query_id join.
     * Falls back to per-pid lookups on kernels without batch support. */
    struct pgwt_qid_entry qidx_buf[MAX_BACKENDS];
    int qidx_n = dump_qid_index(d, s, qidx_buf);

    /* Reuse a stack target array sized to the live count; MAX_BACKENDS cap. */
    static struct pgwt_sample_target targets[MAX_BACKENDS];
    int n = 0;
    for (int i = 0; i < d->backends.count && n < MAX_BACKENDS; i++) {
        struct pgwt_backend *be = &d->backends.entries[i];
        if (!be->is_alive || be->pid <= 0)
            continue;
        /* In sampled mode no bootstrap watchpoint resolves wp_addr, so a
         * freshly-forked backend arrives with wp_addr == 0. Lazily resolve
         * it via the version-appropriate path (PG17+ my_wait_event_info
         * global; PG<17 MyProc + offset — same global VA in every backend,
         * the deref gives that backend's own PGPROC slot). Skip this tick if
         * the backend hasn't set the pointer yet. */
        if (be->wp_addr == 0) {
            be->wp_addr = pgwt_resolve_backend_wait_addr(d, be->pid);
            if (be->wp_addr == 0)
                continue;
            be->wp_addr_shared = -1;
            /* First time we resolved this backend — seed its state_map entry
             * so the query_id uprobe can populate it. */
            seed_state_entry(d, be->pid, be->wp_addr);
        }
        /* SMP-2: classify the address once — only verified-shared addresses
         * may be batched through a foreign pid. */
        if (be->wp_addr_shared < 0)
            be->wp_addr_shared =
                (pgwt_addr_is_shared(be->pid, be->wp_addr) == 1) ? 1 : 0;
        targets[n].pid             = be->pid;
        targets[n].wait_event_addr = be->wp_addr;
        targets[n].is_shared       = be->wp_addr_shared;
        targets[n].query_id        = (qidx_n >= 0)
            ? pgwt_qid_index_lookup(qidx_buf, qidx_n, (uint32_t)be->pid)
            : lookup_query_id(d, be->pid);
        n++;
    }
    if (n == 0)
        return 0;

    uint64_t faults_before = s->read_faults_total;
    errno = 0;
    int got = pgwt_sampler_read_targets(targets, n, s->read_vals,
                                        &s->read_faults_total);
    int read_errno = errno;
    d->counters.sample_read_faults_total +=
        (s->read_faults_total - faults_before);

    /* SMP-1: a tick where NOTHING could be read looks exactly like an idle
     * database in the data — it must be loud out-of-band. */
    switch (pgwt_sampler_health_note(&s->health, n, got, read_errno, tick_ts)) {
    case PGWT_SAMPLER_LOG_DEGRADED:
        fprintf(stderr,
                "ERROR: sampler read failure: 0 of %d backends readable "
                "(last error: %s; %llu consecutive failed ticks).\n"
                "  Sampled data is NOT being captured — an idle-looking view "
                "now means 'blind', not 'idle'. status.sampler_healthy=false "
                "until reads recover.\n",
                n, strerror(s->health.last_errno),
                (unsigned long long)s->health.consec_failed_ticks);
        break;
    case PGWT_SAMPLER_LOG_RECOVERED:
        fprintf(stderr, "INFO: sampler reads recovered (%llu failed ticks "
                "total)\n",
                (unsigned long long)s->health.failed_ticks_total);
        break;
    default:
        break;
    }

    uint64_t invalid_before = d->counters.invalid_wait_reads_total;
    int count = pgwt_sampler_build_batch(targets, s->read_vals, n, tick_ts,
                                         s->samples,
                                         &d->counters.invalid_wait_reads_total);
    if (d->counters.invalid_wait_reads_total != invalid_before
        && !d->invalid_wait_reads_logged) {
        d->invalid_wait_reads_logged = true;
        fprintf(stderr,
                "ERROR: sampler read wait_event_info value(s) with an invalid "
                "wait-event class byte — the resolved address/offset is wrong "
                "for this PostgreSQL build.\n"
                "  Garbage readings are dropped, never recorded "
                "(invalid_wait_reads_total counts them).\n");
    }

    /* Fold the batch into the live accumulator so the running --view reflects
     * sampled data in real time (there is no ringbuf consumer in this tier). */
    pgwt_sampler_accumulate(d, s->samples, count, period_ns);

    /* Anomaly-trigger rules (A5) run on the live sample batch every tick — in
     * tiered mode they may AUTO-escalate to full fidelity. Evaluated even when
     * count == 0 (an all-idle / all-CPU tick is a legitimate low-AAS reading
     * that the rolling baseline must learn from). No-op outside tiered mode. */
    pgwt_anomaly_tick(d, s->samples, count);

    if (count == 0)
        return 0;

    if (d->event_writer)
        pgwt_writer_push_samples(d->event_writer, s->samples, count,
                                 period_ns);

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
