/* provider_coop.c — Cooperative capture provider (Track A, Phase A6)
 *
 * INTERFACE FREEZE ONLY — see provider_coop.h for the full frozen contract
 * the extension track must satisfy. This file is a STUB: it registers a coop
 * provider that advertises PGWT_FIDELITY_EXACT (the cooperative source reports
 * every wait transition), but whose start() cleanly reports that the
 * cooperative provider is not available in this build and refuses activation.
 * No crash, clear message; the daemon aborts startup just as it would for any
 * provider whose capture mechanism is unavailable.
 *
 * The extension track replaces these stub bodies with the real
 * implementation (open the shared-memory ring the patched PG / extension
 * fills, drain transitions in poll() into the SAME writer path the full tier
 * uses — src/event_stream.c handle_event → src/event_writer.c). Nothing else
 * in the daemon changes: provider selection, --mode coop parsing, and the
 * vtable are already wired.
 *
 * Deliberately BPF-free (no skeleton include) so the stub and its unit test
 * build without bpftool / in CI's server-only jobs, like sampler.c's core.
 */
#include "provider_coop.h"
#include "daemon.h"

#include <stdio.h>

/* The single, stable not-available message. Kept as a symbol so the unit
 * test can assert on it without duplicating the string. */
const char pgwt_coop_unavailable_msg[] =
    "cooperative provider not available in this build "
    "(requires the pg_wait_tracer PostgreSQL extension / patched PG)";

static int coop_start(struct pgwt_daemon *d)
{
    /* The cooperative source (patched PG / extension) is implemented in the
     * separate extension track and is not present in this build. Report
     * cleanly and refuse activation — the daemon turns the -1 into a clear
     * "capture provider 'coop' failed to start" abort (src/daemon.c). */
    fprintf(stderr, "ERROR: %s\n", pgwt_coop_unavailable_msg);
    if (d && d->verbose)
        fprintf(stderr,
                "INFO: --mode coop is interface-only here (A6 freeze); the "
                "cooperative tier ships in the extension track\n");
    return -1;
}

static int coop_stop(struct pgwt_daemon *d)
{
    (void)d;
    return 0;  /* never armed — nothing to disarm */
}

static int coop_poll(struct pgwt_daemon *d)
{
    (void)d;
    return 0;  /* never armed — nothing to drain */
}

static void coop_metrics(struct pgwt_daemon *d, struct pgwt_metrics *m)
{
    (void)d;
    (void)m;  /* no cooperative-tier counters until the extension lands */
}

const struct pgwt_capture_provider pgwt_provider_coop = {
    .name         = "coop",
    .fidelity     = PGWT_FIDELITY_EXACT,
    .start        = coop_start,
    .stop         = coop_stop,
    .poll         = coop_poll,
    .self_metrics = coop_metrics,
};
