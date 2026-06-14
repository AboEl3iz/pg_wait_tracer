/* provider_full.c — Full (watchpoint) capture provider (Track A, A.0)
 *
 * Wraps the original hardware-watchpoint + BPF path behind the provider
 * interface with NO behavioral change. The watchpoint attach/detach
 * lifecycle stays in backend.c (driven by the fork/exit tracepoints and
 * pgwt_scan_existing_backends); this provider owns the runtime drain of the
 * event_ringbuf and the full-tier self-metrics.
 *
 * start/stop are no-ops: the watchpoints are armed during pgwt_daemon_init
 * (scan_existing_backends) and as backends fork, exactly as before. poll()
 * is the event_ringbuf consume that daemon.c used to call inline.
 */
#include "provider.h"
#include "daemon.h"

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "pg_wait_tracer.skel.h"

static int full_start(struct pgwt_daemon *d)
{
    (void)d;
    return 0;  /* watchpoints armed in daemon_init / on fork — unchanged */
}

static int full_stop(struct pgwt_daemon *d)
{
    (void)d;
    return 0;  /* watchpoints closed in pgwt_close_all_backends — unchanged */
}

static int full_poll(struct pgwt_daemon *d)
{
    if (d->event_rb)
        ring_buffer__consume(d->event_rb);
    return 0;
}

/* Read the BPF-side event_ringbuf drop counter (A2: a per-CPU array map the
 * BPF program bumps when bpf_ringbuf_output() fails). Sum across CPUs. */
static void full_metrics(struct pgwt_daemon *d, struct pgwt_metrics *m)
{
    if (!d->skel)
        return;
    int fd = bpf_map__fd(d->skel->maps.ringbuf_drops);
    if (fd < 0)
        return;

    int ncpu = libbpf_num_possible_cpus();
    if (ncpu <= 0)
        ncpu = 1;
    uint64_t per_cpu[ncpu];
    uint32_t key = 0;
    if (bpf_map_lookup_elem(fd, &key, per_cpu) == 0) {
        uint64_t total = 0;
        for (int i = 0; i < ncpu; i++)
            total += per_cpu[i];
        m->ringbuf_drops_total = total;
    }
}

const struct pgwt_capture_provider pgwt_provider_full = {
    .name         = "full",
    .fidelity     = PGWT_FIDELITY_EXACT,
    .start        = full_start,
    .stop         = full_stop,
    .poll         = full_poll,
    .self_metrics = full_metrics,
};
