/* daemon.c — Main event loop: epoll on ring buffer, timer, signals */
#include "daemon.h"
#include "backend.h"
#include "control.h"
#include "discovery.h"
#include "event_stream.h"
#include "event_writer.h"
#include "map_reader.h"
#include "output.h"
#include "perf_event.h"
#include "provider_coop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <sys/resource.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "pg_wait_tracer.skel.h"

/* Ring buffer callback for lifecycle events */
static int handle_lifecycle_event(void *ctx, void *data, size_t data_sz)
{
    struct pgwt_daemon *d = ctx;
    struct pgwt_lifecycle_event *ev = data;

    (void)data_sz;

    d->counters.lifecycle_events_total++;

    switch (ev->type) {
    case PGWT_LIFECYCLE_FORK:
        pgwt_handle_fork(d, ev->pid);
        break;
    case PGWT_LIFECYCLE_INIT:
        pgwt_handle_init(d, ev->pid, ev->addr);
        break;
    case PGWT_LIFECYCLE_EXIT:
        pgwt_handle_exit(d, ev->pid);
        break;
    case PGWT_LIFECYCLE_QUERY_TEXT: {
        /* Query text captured by BPF from debug_query_string */
        struct pgwt_query_text_event *qte = data;
        if (d->query_text_capture && qte->query_id != 0) {
            pgwt_qt_store(d->query_text_capture, qte->query_id,
                          qte->text, qte->pid);
        }
        break;
    }
    }
    return 0;
}

static void handle_timer(struct pgwt_daemon *d)
{
    uint64_t expirations;
    ssize_t r = read(d->timer_fd, &expirations, sizeof(expirations));
    (void)r;

    /* Health check: is PostgreSQL still alive? */
    if (d->daemon_mode) {
        if (kill(d->postmaster_pid, 0) != 0) {
            fprintf(stderr, "\npg_wait_tracer: PostgreSQL (PID %d) stopped\n",
                    d->postmaster_pid);
            d->exit_reason = PGWT_EXIT_PG_DEAD;
            d->running = false;
            return;
        }
    }

    /* In lightweight mode, read BPF accum_map into event_accum first */
    if (d->lightweight_mode)
        pgwt_read_accum_map(d);

    /* Copy cumulative event_accum to display accum,
     * then add open intervals from state_map.
     * Sampled mode has no watchpoint-maintained state_map intervals (entries
     * exist only to carry query_id), so reading it would manufacture bogus
     * open intervals — skip it. The live display for sampled mode is not
     * fidelity-aware until A3; recorded SAMPLES blocks carry the real data. */
    struct pgwt_time_model saved_tm = d->accum.tm;
    pgwt_accum_copy_used(&d->accum, d->event_accum);
    d->accum.prev_tm = saved_tm;
    if (pgwt_mode_uses_watchpoints(d))
        pgwt_read_state_map(d);

    if (d->ring.slots)
        pgwt_ring_push(&d->ring, &d->accum);

    if (!d->quiet) {
        switch (d->view) {
        case PGWT_VIEW_TIME_MODEL:
            pgwt_print_time_model(d);
            break;
        case PGWT_VIEW_SYSTEM_EVENT:
            pgwt_print_system_event(d);
            break;
        case PGWT_VIEW_SESSION_EVENT:
            pgwt_print_session_event(d);
            break;
        case PGWT_VIEW_HISTOGRAM:
            pgwt_print_histogram(d);
            break;
        case PGWT_VIEW_QUERY_EVENT:
            pgwt_print_query_event(d);
            break;
        case PGWT_VIEW_ACTIVE:
            pgwt_print_active(d);
            break;
        }
        fflush(stdout);
    }

    /* GC: sweep backend table every 60s for dead processes.
     * Handles PIDs that exited without triggering on_exit tracepoint
     * (e.g., SIGKILL, or race condition during high churn). */
    if (d->tick > 0 && d->tick % 60 == 0) {
        for (int i = 0; i < d->backends.count; i++) {
            struct pgwt_backend *be = &d->backends.entries[i];
            if (be->is_alive && be->pid > 0 && kill(be->pid, 0) != 0) {
                if (d->verbose)
                    fprintf(stderr, "INFO: GC: PID %d no longer alive, cleaning up\n",
                            be->pid);
                pgwt_handle_exit(d, be->pid);
            }
        }
    }

    /* Trace file: check hourly rotation, periodic cleanup */
    if (d->event_writer) {
        pgwt_writer_check_rotation(d->event_writer);
        if (d->tick > 0 && d->tick % 60 == 0)
            pgwt_writer_cleanup_old_files(d->event_writer);
    }
    if (d->summary_writer) {
        pgwt_summary_flush(d->summary_writer);
        pgwt_summary_check_rotation(d->summary_writer);
        if (d->tick > 0 && d->tick % 60 == 0)
            pgwt_summary_cleanup_old_files(d->summary_writer);
    }

    /* CAP-6: the BPF seen_query_ids dedup map (4096 entries) never ages;
     * once full, query TEXT for new query_ids is silently never captured
     * again. Log it once; seen_query_ids_full_total keeps counting. */
    if (!d->seen_qids_full_logged
        && pgwt_read_bpf_fail_counter(d, PGWT_BPF_FAIL_SEEN_QIDS) > 0) {
        d->seen_qids_full_logged = true;
        fprintf(stderr,
                "WARN: BPF seen_query_ids map is FULL (4096 unique query_ids)"
                " — query TEXT for new query_ids will no longer be captured "
                "(ids and waits are unaffected). Restart the daemon to reset;"
                " seen_query_ids_full_total counts the misses.\n");
    }

    /* Refresh recent event/sample rates for control-socket metrics */
    if (d->interval > 0) {
        d->counters.events_per_sec =
            (double)(d->counters.events_total - d->counters.prev_events_total)
            / d->interval;
        d->counters.prev_events_total = d->counters.events_total;

        d->counters.samples_per_sec =
            (double)(d->counters.samples_total - d->counters.prev_samples_total)
            / d->interval;
        d->counters.prev_samples_total = d->counters.samples_total;
    }

    d->tick++;
    if (d->count > 0 && d->tick >= d->count)
        d->running = false;
}

static void handle_sample_timer(struct pgwt_daemon *d)
{
    uint64_t expirations = 0;
    ssize_t r = read(d->sample_timer_fd, &expirations, sizeof(expirations));
    /* SMP-3: expirations > 1 means the daemon stalled and ticks coalesced —
     * those samples are gone. The sampler compensates by weighting each tick
     * with the MEASURED inter-tick elapsed time (see pgwt_sampler_poll), so
     * the loss is in resolution, not in total weight. Count it so a chronic
     * stall is visible on the control socket. */
    if (r == (ssize_t)sizeof(expirations) && expirations > 1)
        d->counters.sampler_ticks_missed_total += expirations - 1;
    if (d->provider && d->provider->poll)
        d->provider->poll(d);
}

/* CAP-12: raise RLIMIT_NOFILE to what MAX_BACKENDS needs. Full/tiered mode
 * holds up to two perf fds per backend (watchpoint + bootstrap) plus BPF
 * maps/ringbufs, trace files and the control socket; the common default of
 * 1024 is below that, and hitting it turns every further attach into a
 * quiet EMFILE failure (now also loudly reported in perf_event.c). */
static void bump_rlimit_nofile(struct pgwt_daemon *d)
{
    const rlim_t need = MAX_BACKENDS * 2 + 512;
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0)
        return;
    if (rl.rlim_cur >= need)
        return;

    struct rlimit want = rl;
    want.rlim_cur = need;
    if (want.rlim_max != RLIM_INFINITY && want.rlim_max < need)
        want.rlim_max = need;   /* root may raise the hard limit */
    if (setrlimit(RLIMIT_NOFILE, &want) == 0) {
        if (d->verbose)
            fprintf(stderr, "INFO: RLIMIT_NOFILE raised %llu -> %llu\n",
                    (unsigned long long)rl.rlim_cur,
                    (unsigned long long)need);
        return;
    }
    /* Could not raise the hard limit (not root?) — take what we can get. */
    want.rlim_max = rl.rlim_max;
    want.rlim_cur = (rl.rlim_max == RLIM_INFINITY || rl.rlim_max > need)
                  ? need : rl.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &want) == 0 && want.rlim_cur >= need)
        return;
    fprintf(stderr,
            "WARN: RLIMIT_NOFILE is %llu (< %llu needed for %d backends). "
            "Watchpoint attach will fail with EMFILE beyond the limit — "
            "raise the hard limit (ulimit -Hn / LimitNOFILE=).\n",
            (unsigned long long)want.rlim_cur, (unsigned long long)need,
            MAX_BACKENDS);
}

static void handle_signal(struct pgwt_daemon *d)
{
    struct signalfd_siginfo si;
    ssize_t r = read(d->signal_fd, &si, sizeof(si));
    (void)r;
    d->running = false;
}

int pgwt_daemon_init(struct pgwt_daemon *d)
{
    int err;

    /* CAP-12: enough fds for a full-size backend registry, before anything
     * starts opening watchpoints. */
    bump_rlimit_nofile(d);

    /* Select the capture provider for this mode. FULL (default) is the
     * original watchpoint path; SAMPLED is the userspace tier; TIERED runs
     * the sampler always-on (escalation engine lands in A4). */
    switch (d->mode) {
    case PGWT_MODE_SAMPLED:
    case PGWT_MODE_TIERED:
        d->provider = &pgwt_provider_sampled;
        break;
    case PGWT_MODE_COOP:
        /* A6 interface freeze: the coop provider is recognized but its
         * start() cleanly reports "not available in this build" (the
         * cooperative tier ships in the separate extension track). */
        d->provider = &pgwt_provider_coop;
        break;
    case PGWT_MODE_FULL:
    default:
        d->provider = &pgwt_provider_full;
        break;
    }

    /* Init backend table and accumulator */
    pgwt_backend_init(&d->backends);
    pgwt_accum_init(&d->accum);

    /* Init ring buffer for windowed analysis */
    if (d->num_windows > 0) {
        int cap = d->windows[d->num_windows - 1] / d->interval + 1;
        if (pgwt_ring_init(&d->ring, cap) != 0) {
            fprintf(stderr, "FATAL: cannot allocate snapshot ring buffer (%d slots)\n", cap);
            return -1;
        }
        if (d->verbose)
            fprintf(stderr, "INFO: ring buffer: %d slots (%.1f MB)\n",
                    cap, (double)cap * sizeof(struct pgwt_snapshot) / 1e6);
    }

    /* Open and configure BPF skeleton */
    d->skel = pg_wait_tracer_bpf__open();
    if (!d->skel) {
        fprintf(stderr, "FATAL: failed to open BPF skeleton\n");
        return -1;
    }

    /* Set rodata constants before loading */
    d->skel->rodata->target_postmaster_pid = d->postmaster_pid;
    d->skel->rodata->my_wait_ptr_addr = d->my_wait_ptr_addr;
    /* PG<17: my_wait_ptr_addr is MyProc (PGPROC*); on_bootstrap adds this
     * offset to *MyProc to reach wait_event_info. 0 on PG17+. */
    d->skel->rodata->pgproc_wait_offset =
        d->use_myproc ? (uint32_t)d->pgproc_wait_offset : 0;
    d->skel->rodata->my_be_entry_addr = d->my_be_entry_addr;
    d->skel->rodata->st_query_id_offset = d->st_query_id_offset;
    d->skel->rodata->lightweight_mode = d->lightweight_mode;
    d->skel->rodata->skip_query_id = d->skip_query_id;

    /* PG13 query attribution (Route B1): the ExecutorStart uprobe walks
     * QueryDesc->plannedstmt->queryId into the state_map. Enabled only when
     * pg_stat_statements is loaded (use_pg13_query_attr). */
    d->skel->rodata->pg13_query_attr = d->use_pg13_query_attr ? 1 : 0;
    d->skel->rodata->pg13_qd_plannedstmt_off = (uint32_t)d->pg13_qd_plannedstmt_off;
    d->skel->rodata->pg13_ps_queryid_off = (uint32_t)d->pg13_ps_queryid_off;
    d->skel->rodata->pg13_qd_sourcetext_off = (uint32_t)d->pg13_qd_sourcetext_off;

    /* CAP-1: state_map is compiled at MAX_BACKENDS entries (the registry
     * capacity). PGWT_STATE_MAP_ENTRIES shrinks it before load — a TEST hook
     * so CI can prove the map-full loud path with a handful of connections
     * instead of >1024 real ones. Never set it in production. */
    {
        const char *sme = getenv("PGWT_STATE_MAP_ENTRIES");
        if (sme && atoi(sme) > 0) {
            uint32_t entries = (uint32_t)atoi(sme);
            bpf_map__set_max_entries(d->skel->maps.state_map, entries);
            fprintf(stderr, "WARN: PGWT_STATE_MAP_ENTRIES=%u — state_map "
                    "shrunk below MAX_BACKENDS (test hook; backends beyond "
                    "this record nothing, loudly)\n", entries);
        }
    }

    /* Resolve debug_query_string address for BPF query text capture */
    if (d->pg_binary_saved) {
        uint64_t dqs_addr = pgwt_find_symbol_offset(d->pg_binary_saved,
                                                     "debug_query_string");
        d->skel->rodata->debug_query_string_addr = dqs_addr;
        if (d->verbose && dqs_addr)
            fprintf(stderr, "INFO: debug_query_string at 0x%lx\n",
                    (unsigned long)dqs_addr);
    }

    /* Load BPF programs (runs verifier) */
    err = pg_wait_tracer_bpf__load(d->skel);
    if (err) {
        fprintf(stderr, "FATAL: BPF load failed: %s\n", strerror(-err));
        pg_wait_tracer_bpf__destroy(d->skel);
        d->skel = NULL;
        return -1;
    }

    /* Attach tracepoints (fork/exit — auto-attach) */
    d->skel->links.on_fork = bpf_program__attach(d->skel->progs.on_fork);
    if (!d->skel->links.on_fork) {
        fprintf(stderr, "FATAL: cannot attach on_fork tracepoint: %s\n",
                strerror(errno));
        goto fail;
    }

    d->skel->links.on_exit = bpf_program__attach(d->skel->progs.on_exit);
    if (!d->skel->links.on_exit) {
        fprintf(stderr, "FATAL: cannot attach on_exit tracepoint: %s\n",
                strerror(errno));
        goto fail;
    }

    /* Attach query lifecycle probes (only in full trace mode) */
    if (!d->lightweight_mode && !d->skip_usdt && d->pg_binary_saved) {
        const char *bin = d->pg_binary_saved;

        /* Query-id capture uprobe. Must be active before USDT probes fire so
         * EXEC_START always has a correct query_id. Two variants:
         *   PG14+ : pgstat_report_query_id (arg = query_id), bypassing shmem.
         *   PG13  : no in-core query_id / no pgstat_report_query_id. Instead,
         *           with pg_stat_statements loaded (use_pg13_query_attr),
         *           uprobe standard_ExecutorStart(QueryDesc*) and walk
         *           queryDesc->plannedstmt->queryId. Feeds the SAME state_map
         *           slot, so the sampler + watchpoint pipelines are unchanged.
         *
         *           We probe standard_ExecutorStart, NOT the public
         *           ExecutorStart wrapper. With pg_stat_statements loaded,
         *           ExecutorStart_hook is set, so the ExecutorStart wrapper is
         *           a tiny trampoline that tail-jumps (jmp *hook) into the
         *           hook chain; an entry uprobe placed on that trampoline does
         *           not fire reliably (the tail-jump never returns through the
         *           wrapper body). standard_ExecutorStart is the real function
         *           and is always reached at the bottom of the hook chain
         *           (pgss's hook calls it), by which point pgss has already
         *           populated PlannedStmt.queryId. Same arg0 (QueryDesc*). */
        if (d->use_pg13_query_attr) {
            uint64_t es_va = pgwt_find_symbol_offset(bin, "standard_ExecutorStart");
            uint64_t es_off = es_va > 0x400000 ? es_va - 0x400000 : es_va;
            if (es_off) {
                LIBBPF_OPTS(bpf_uprobe_opts, uprobe_opts, .retprobe = false);
                d->skel->links.on_executor_start =
                    bpf_program__attach_uprobe_opts(d->skel->progs.on_executor_start,
                                                    -1, bin, es_off, &uprobe_opts);
                if (d->verbose)
                    fprintf(stderr, "INFO: standard_ExecutorStart at offset 0x%lx "
                            "(PG13 query attribution via pg_stat_statements)\n",
                            (unsigned long)es_off);
                if (!d->skel->links.on_executor_start)
                    fprintf(stderr, "WARN: could not attach standard_ExecutorStart "
                            "uprobe (PG13 query attribution disabled)\n");
            } else {
                fprintf(stderr, "WARN: symbol 'standard_ExecutorStart' not found "
                        "(PG13 query attribution disabled)\n");
            }
        } else {
            uint64_t qid_func_va = pgwt_find_symbol_offset(bin, "pgstat_report_query_id");
            uint64_t qid_func_off = qid_func_va > 0x400000 ? qid_func_va - 0x400000 : qid_func_va;
            if (qid_func_off) {
                LIBBPF_OPTS(bpf_uprobe_opts, uprobe_opts, .retprobe = false);
                d->skel->links.on_report_query_id =
                    bpf_program__attach_uprobe_opts(d->skel->progs.on_report_query_id,
                                                    -1, bin, qid_func_off, &uprobe_opts);
                if (d->verbose)
                    fprintf(stderr, "INFO: pgstat_report_query_id at offset 0x%lx\n",
                            (unsigned long)qid_func_off);
            }
        }

        /* USDT marker probes write into event_ringbuf, which only the FULL
         * tier consumes. In sampled mode they would be pure overhead with no
         * reader, so attach them only when watchpoints are in use. The
         * query_id uprobe above stays in every mode — the sampler joins
         * pid->query_id from the state_map it maintains. */
        if (pgwt_mode_uses_watchpoints(d)) {
            d->skel->links.on_exec_start =
                bpf_program__attach_usdt(d->skel->progs.on_exec_start,
                                         -1, bin,
                                         "postgresql", "query__execute__start", NULL);
            d->skel->links.on_exec_done =
                bpf_program__attach_usdt(d->skel->progs.on_exec_done,
                                         -1, bin,
                                         "postgresql", "query__execute__done", NULL);
            d->skel->links.on_plan_start =
                bpf_program__attach_usdt(d->skel->progs.on_plan_start,
                                         -1, bin,
                                         "postgresql", "query__plan__start", NULL);
            d->skel->links.on_plan_done =
                bpf_program__attach_usdt(d->skel->progs.on_plan_done,
                                         -1, bin,
                                         "postgresql", "query__plan__done", NULL);

            bool qid_attached = d->use_pg13_query_attr
                ? (d->skel->links.on_executor_start != NULL)
                : (d->skel->links.on_report_query_id != NULL);
            if (d->skel->links.on_exec_start && d->skel->links.on_exec_done
                && qid_attached)
                fprintf(stderr, "INFO: attached USDT + query_id uprobe\n");
            else
                fprintf(stderr, "WARN: could not attach query probes (lifecycle tracking disabled)\n");
        } else if (d->skel->links.on_report_query_id || d->skel->links.on_executor_start) {
            fprintf(stderr, "INFO: attached query_id uprobe (sampled mode)\n");
        }
    }

    /* Set up lifecycle ring buffer consumer */
    int rb_map_fd = bpf_map__fd(d->skel->maps.lifecycle_rb);
    d->rb = ring_buffer__new(rb_map_fd, handle_lifecycle_event, d, NULL);
    if (!d->rb) {
        fprintf(stderr, "FATAL: cannot create ring buffer: %s\n",
                strerror(errno));
        goto fail;
    }

    /* Set up event ring buffer consumer */
    d->event_accum = calloc(1, sizeof(*d->event_accum));
    if (!d->event_accum) {
        fprintf(stderr, "FATAL: cannot allocate event accumulator\n");
        goto fail;
    }
    pgwt_accum_init(d->event_accum);

    /* The event ringbuf consumer carries watchpoint transitions (FULL tier).
     * Lightweight mode accumulates in BPF; pure sampled mode arms no
     * watchpoints, so nothing is ever written there. TIERED mode starts
     * de-escalated (no watchpoints yet) but MUST have the ringbuf ready so an
     * escalation can begin consuming immediately — D5: skeleton + ringbuf are
     * loaded at daemon start, escalation only attaches watchpoints + starts
     * consuming. An idle ringbuf costs nothing. */
    bool needs_event_rb = !d->lightweight_mode
        && (pgwt_mode_uses_watchpoints(d) || d->mode == PGWT_MODE_TIERED);
    if (needs_event_rb) {
        int event_rb_map_fd = bpf_map__fd(d->skel->maps.event_ringbuf);
        d->event_rb = ring_buffer__new(event_rb_map_fd, pgwt_handle_trace_event, d, NULL);
        if (!d->event_rb) {
            fprintf(stderr, "FATAL: cannot create event ring buffer: %s\n",
                    strerror(errno));
            goto fail;
        }
    }

    /* Init trace file writer (opt-in via --trace-dir) */
    if (d->trace_dir) {
        d->event_writer = calloc(1, sizeof(*d->event_writer));
        if (!d->event_writer) {
            fprintf(stderr, "FATAL: cannot allocate event writer\n");
            goto fail;
        }
        int ret = d->trace_retention > 0 ? d->trace_retention : 24;
        if (pgwt_writer_init(d->event_writer, d->trace_dir,
                             d->pg_major_version, ret,
                             d->trace_group) != 0) {
            fprintf(stderr, "FATAL: cannot initialize trace writer\n");
            free(d->event_writer);
            d->event_writer = NULL;
            goto fail;
        }
        d->event_writer->verbose = d->verbose;
        if (d->verbose)
            fprintf(stderr, "INFO: trace writer: %s (retention %dh)\n",
                    d->trace_dir, ret);

        /* Init summary writer (alongside event writer) */
        d->summary_writer = calloc(1, sizeof(*d->summary_writer));
        if (!d->summary_writer) {
            fprintf(stderr, "FATAL: cannot allocate summary writer\n");
            goto fail;
        }
        if (pgwt_summary_writer_init(d->summary_writer, d->trace_dir,
                                      ret, d->trace_group) != 0) {
            fprintf(stderr, "FATAL: cannot initialize summary writer\n");
            free(d->summary_writer);
            d->summary_writer = NULL;
            goto fail;
        }
        d->summary_writer->verbose = d->verbose;

        /* Init query text capture (requires st_activity_raw offset + MyBEEntry) */
        if (d->st_activity_offset > 0 && d->my_be_entry_addr != 0) {
            d->query_text_capture = calloc(1, sizeof(*d->query_text_capture));
            if (d->query_text_capture) {
                if (pgwt_qt_init(d->query_text_capture, d->trace_dir,
                                  d->my_be_entry_addr,
                                  d->st_activity_offset) != 0) {
                    free(d->query_text_capture);
                    d->query_text_capture = NULL;
                } else {
                    d->query_text_capture->verbose = d->verbose;
                    if (d->verbose)
                        fprintf(stderr, "INFO: query text capture enabled\n");
                }
            }
        }

        /* Init backend metadata writer */
        d->backend_meta = calloc(1, sizeof(*d->backend_meta));
        if (d->backend_meta) {
            if (pgwt_bm_init(d->backend_meta, d->trace_dir) != 0) {
                free(d->backend_meta);
                d->backend_meta = NULL;
            } else if (d->verbose) {
                fprintf(stderr, "INFO: backend metadata writer enabled\n");
            }
        }
    }

    /* Scan existing backends (tracepoints are already attached,
     * so any new forks during scan will be caught) */
    int n = pgwt_scan_existing_backends(d);
    if (n == -2) {
        /* Runtime offset validation refused (garbage class byte). The old
         * code path degraded this to a WARN and kept running — the exact
         * silent-garbage failure CAP-2 exists to prevent. Abort. */
        goto fail;
    } else if (n < 0) {
        fprintf(stderr, "WARN: scan_existing_backends failed\n");
    } else if (d->verbose) {
        fprintf(stderr, "INFO: attached to %d existing backends\n", n);
    }

    /* CAP-2/3: if the scan resolved backends but none produced a NON-ZERO
     * class-valid reading, re-poll briefly and refuse to run on the
     * hardcoded-offset path (PG<17) if the offset still cannot be confirmed
     * — a wrong offset into zeroed memory reads zero forever, and blessing
     * it would trace garbage (or nothing) labeled as real. */
    if (pgwt_confirm_wait_offset(d) != 0)
        goto fail;

    /* Create timer fd */
    d->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (d->timer_fd < 0) {
        perror("timerfd_create");
        goto fail;
    }
    struct itimerspec its = {
        .it_interval = { .tv_sec = d->interval },
        .it_value    = { .tv_sec = d->interval },
    };
    timerfd_settime(d->timer_fd, 0, &its, NULL);

    /* High-rate sampler timer (sampled/tiered tiers): fires at sample_rate_hz
     * independently of the per-second display interval. */
    if (d->provider && d->provider->fidelity == PGWT_FIDELITY_SAMPLED) {
        d->sample_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
        if (d->sample_timer_fd < 0) {
            perror("timerfd_create (sampler)");
            goto fail;
        }
        int hz = d->sample_rate_hz > 0 ? d->sample_rate_hz : 10;
        uint64_t period_ns = 1000000000ULL / (uint64_t)hz;
        struct itimerspec sits = {
            .it_interval = { .tv_sec = (time_t)(period_ns / 1000000000ULL),
                             .tv_nsec = (long)(period_ns % 1000000000ULL) },
            .it_value    = { .tv_sec = (time_t)(period_ns / 1000000000ULL),
                             .tv_nsec = (long)(period_ns % 1000000000ULL) },
        };
        timerfd_settime(d->sample_timer_fd, 0, &sits, NULL);
    }

    /* Create signal fd */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    d->signal_fd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (d->signal_fd < 0) {
        perror("signalfd");
        goto fail;
    }

    /* Create epoll */
    d->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (d->epoll_fd < 0) {
        perror("epoll_create1");
        goto fail;
    }

    struct epoll_event ev;

    /* Add lifecycle ring buffer fd */
    int rb_fd = ring_buffer__epoll_fd(d->rb);
    ev.events = EPOLLIN;
    ev.data.fd = rb_fd;
    epoll_ctl(d->epoll_fd, EPOLL_CTL_ADD, rb_fd, &ev);

    /* Add event ring buffer fd (skip in lightweight mode) */
    int event_rb_fd = -1;
    if (d->event_rb) {
        event_rb_fd = ring_buffer__epoll_fd(d->event_rb);
        ev.events = EPOLLIN;
        ev.data.fd = event_rb_fd;
        epoll_ctl(d->epoll_fd, EPOLL_CTL_ADD, event_rb_fd, &ev);
    }

    /* Add timer fd */
    ev.events = EPOLLIN;
    ev.data.fd = d->timer_fd;
    epoll_ctl(d->epoll_fd, EPOLL_CTL_ADD, d->timer_fd, &ev);

    /* Add sampler timer fd (sampled/tiered tiers) */
    if (d->sample_timer_fd >= 0) {
        ev.events = EPOLLIN;
        ev.data.fd = d->sample_timer_fd;
        epoll_ctl(d->epoll_fd, EPOLL_CTL_ADD, d->sample_timer_fd, &ev);
    }

    /* Add signal fd */
    ev.events = EPOLLIN;
    ev.data.fd = d->signal_fd;
    epoll_ctl(d->epoll_fd, EPOLL_CTL_ADD, d->signal_fd, &ev);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    d->start_ts = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    d->running = true;

    /* Control socket at {trace_dir}/pgwt.sock (D4).
     * Failure is non-fatal: tracing must not depend on the control plane. */
    if (d->trace_dir) {
        d->control = calloc(1, sizeof(*d->control));
        if (d->control) {
            if (pgwt_control_init(d->control, d, d->epoll_fd) != 0) {
                free(d->control);
                d->control = NULL;
            } else if (d->verbose) {
                fprintf(stderr, "INFO: control socket: %s/pgwt.sock\n",
                        d->trace_dir);
            }
        }
    }

    /* Escalation engine (D5/A4). Enabled only in --mode tiered; creates the
     * bounded-window deadline timerfd on the daemon epoll. Failure is
     * non-fatal — tiered then runs sampled-only with no escalation path. */
    if (pgwt_escalation_init(d, d->escalation_budget_s) != 0) {
        fprintf(stderr, "WARN: escalation engine unavailable; "
                "tiered mode will run sampled-only\n");
    }

    /* Anomaly-trigger rules (A5): armed only when the escalation engine is
     * live (tiered mode with a working deadline timer). Rate-derived defaults
     * are then overridden by any --anomaly-* flags. */
    pgwt_anomaly_init(&d->anomaly, d->escalation.enabled, d->sample_rate_hz);
    if (d->anomaly.enabled) {
        if (d->anomaly_aas_factor > 0.0)
            d->anomaly.aas_factor = d->anomaly_aas_factor;
        if (d->anomaly_aas_ticks > 0)
            d->anomaly.aas_ticks = d->anomaly_aas_ticks;
        if (d->anomaly_lock_fraction >= 0.0)
            d->anomaly.lock_fraction = d->anomaly_lock_fraction;
        if (d->anomaly_cooldown_s >= 0)
            d->anomaly.cooldown_ns =
                (uint64_t)d->anomaly_cooldown_s * 1000000000ULL;
        if (d->anomaly_window_s > 0)
            d->anomaly.escalation_s = d->anomaly_window_s;
        if (d->verbose)
            fprintf(stderr,
                    "INFO: anomaly triggers armed: aas>%.1f*baseline for %d "
                    "ticks, lock_frac>%.2f for %d ticks, cooldown %llus, "
                    "window %ds\n",
                    d->anomaly.aas_factor, d->anomaly.aas_ticks,
                    d->anomaly.lock_fraction, d->anomaly.lock_ticks,
                    (unsigned long long)(d->anomaly.cooldown_ns / 1000000000ULL),
                    d->anomaly.escalation_s);
    }

    /* Arm the active capture provider. For the full tier this is a no-op
     * (watchpoints were armed during the backend scan); for the sampled tier
     * it allocates the sampler state. */
    if (d->provider && d->provider->start) {
        if (d->provider->start(d) != 0) {
            fprintf(stderr, "FATAL: capture provider '%s' failed to start\n",
                    d->provider->name);
            goto fail;
        }
    }
    if (d->verbose)
        fprintf(stderr, "INFO: capture provider: %s (%s fidelity)\n",
                d->provider->name,
                d->provider->fidelity == PGWT_FIDELITY_EXACT ? "exact" : "sampled");

    if (!d->quiet)
        pgwt_print_header(d);
    return 0;

fail:
    pgwt_daemon_cleanup(d);
    return -1;
}

int pgwt_daemon_run(struct pgwt_daemon *d)
{
    struct epoll_event events[8];
    int rb_fd = ring_buffer__epoll_fd(d->rb);
    int event_rb_fd = d->event_rb ? ring_buffer__epoll_fd(d->event_rb) : -1;

    uint64_t deadline = 0;
    if (d->duration > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        deadline = (uint64_t)ts.tv_sec + d->duration;
    }

    /* BPF uses BPF_RB_NO_WAKEUP for event_ringbuf to avoid per-event
     * eventfd_signal overhead. We poll the ringbuf at 10ms intervals. */
    int poll_ms = d->event_rb ? 10 : -1;

    while (d->running) {
        int timeout_ms = poll_ms;
        if (deadline > 0) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            if ((uint64_t)ts.tv_sec >= deadline) break;
            int remaining = (deadline - ts.tv_sec) * 1000;
            if (timeout_ms < 0 || remaining < timeout_ms)
                timeout_ms = remaining;
        }

        int n = epoll_wait(d->epoll_fd, events, 8, timeout_ms);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == d->timer_fd) {
                handle_timer(d);
            } else if (d->sample_timer_fd >= 0
                       && events[i].data.fd == d->sample_timer_fd) {
                handle_sample_timer(d);
            } else if (events[i].data.fd == rb_fd) {
                ring_buffer__consume(d->rb);
            } else if (events[i].data.fd == event_rb_fd) {
                ring_buffer__consume(d->event_rb);
            } else if (events[i].data.fd == d->signal_fd) {
                handle_signal(d);
            } else if (pgwt_escalation_is_timer_fd(d, events[i].data.fd)) {
                pgwt_escalation_on_timer(d);
            } else if (d->control) {
                pgwt_control_handle_fd(d->control, events[i].data.fd);
            }
        }

        /* Drain event ringbuf on every iteration (poll-based with NO_WAKEUP) */
        if (d->event_rb)
            ring_buffer__consume(d->event_rb);
    }

    /* Drain remaining events before cleanup */
    if (d->event_rb)
        ring_buffer__consume(d->event_rb);
    ring_buffer__consume(d->rb);

    if (d->exit_reason != PGWT_EXIT_PG_DEAD)
        fprintf(stderr, "\npg_wait_tracer: shutting down\n");
    return 0;
}

void pgwt_daemon_cleanup(struct pgwt_daemon *d)
{
    /* Close any open escalation window (detaches watchpoints, writes the END
     * marker) before the event writer is torn down. */
    pgwt_escalation_cleanup(d);

    /* Stop the active capture provider (detach/free its state). */
    if (d->provider && d->provider->stop)
        d->provider->stop(d);

    if (d->control) {
        pgwt_control_cleanup(d->control);
        free(d->control);
        d->control = NULL;
    }
    if (d->backend_meta) {
        pgwt_bm_close(d->backend_meta);
        free(d->backend_meta);
        d->backend_meta = NULL;
    }
    if (d->query_text_capture) {
        if (d->query_text_capture->verbose)
            fprintf(stderr, "INFO: query text capture: %d unique queries captured\n",
                    d->query_text_capture->num_seen);
        pgwt_qt_close(d->query_text_capture);
        free(d->query_text_capture);
        d->query_text_capture = NULL;
    }
    if (d->summary_writer) {
        pgwt_summary_close(d->summary_writer);
        pgwt_summary_destroy(d->summary_writer);
        free(d->summary_writer);
        d->summary_writer = NULL;
    }
    if (d->event_writer) {
        pgwt_writer_close(d->event_writer);
        pgwt_writer_destroy(d->event_writer);
        free(d->event_writer);
        d->event_writer = NULL;
    }

    pgwt_ring_free(&d->ring);
    pgwt_close_all_backends(&d->backends);

    if (d->event_rb) {
        ring_buffer__free(d->event_rb);
        d->event_rb = NULL;
    }
    if (d->event_accum) {
        free(d->event_accum);
        d->event_accum = NULL;
    }
    if (d->rb) {
        ring_buffer__free(d->rb);
        d->rb = NULL;
    }
    if (d->skel) {
        pg_wait_tracer_bpf__destroy(d->skel);
        d->skel = NULL;
    }
    if (d->epoll_fd >= 0) { close(d->epoll_fd); d->epoll_fd = -1; }
    if (d->timer_fd >= 0) { close(d->timer_fd); d->timer_fd = -1; }
    if (d->sample_timer_fd >= 0) { close(d->sample_timer_fd); d->sample_timer_fd = -1; }
    if (d->signal_fd >= 0) { close(d->signal_fd); d->signal_fd = -1; }
}
