/* daemon.c — Main event loop: epoll on ring buffer, timer, signals */
#include "daemon.h"
#include "backend.h"
#include "discovery.h"
#include "event_stream.h"
#include "event_writer.h"
#include "map_reader.h"
#include "output.h"
#include "perf_event.h"

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

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "pg_wait_tracer.skel.h"

/* Ring buffer callback for lifecycle events */
static int handle_lifecycle_event(void *ctx, void *data, size_t data_sz)
{
    struct pgwt_daemon *d = ctx;
    struct pgwt_lifecycle_event *ev = data;

    (void)data_sz;

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
     * then add open intervals from state_map */
    struct pgwt_time_model saved_tm = d->accum.tm;
    pgwt_accum_copy_used(&d->accum, d->event_accum);
    d->accum.prev_tm = saved_tm;
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

    d->tick++;
    if (d->count > 0 && d->tick >= d->count)
        d->running = false;
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
    d->skel->rodata->my_be_entry_addr = d->my_be_entry_addr;
    d->skel->rodata->st_query_id_offset = d->st_query_id_offset;
    d->skel->rodata->lightweight_mode = d->lightweight_mode;
    d->skel->rodata->skip_query_id = d->skip_query_id;

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

        /* Uprobe on pgstat_report_query_id FIRST — must be active before
         * USDT probes fire, so EXEC_START always has a correct query_id.
         * Captures query_id from function argument, bypassing shared memory. */
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

        /* USDT probes AFTER uprobe — query_id is already cached when these fire */
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

        if (d->skel->links.on_exec_start && d->skel->links.on_exec_done
            && d->skel->links.on_report_query_id)
            fprintf(stderr, "INFO: attached USDT + query_id uprobe\n");
        else
            fprintf(stderr, "WARN: could not attach query probes (lifecycle tracking disabled)\n");
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

    /* In lightweight mode, skip event ringbuf consumer (BPF accumulates directly) */
    if (!d->lightweight_mode) {
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
    if (n < 0) {
        fprintf(stderr, "WARN: scan_existing_backends failed\n");
    } else if (d->verbose) {
        fprintf(stderr, "INFO: attached to %d existing backends\n", n);
    }

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

    /* Add signal fd */
    ev.events = EPOLLIN;
    ev.data.fd = d->signal_fd;
    epoll_ctl(d->epoll_fd, EPOLL_CTL_ADD, d->signal_fd, &ev);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    d->start_ts = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    d->running = true;

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
            } else if (events[i].data.fd == rb_fd) {
                ring_buffer__consume(d->rb);
            } else if (events[i].data.fd == event_rb_fd) {
                ring_buffer__consume(d->event_rb);
            } else if (events[i].data.fd == d->signal_fd) {
                handle_signal(d);
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
    if (d->signal_fd >= 0) { close(d->signal_fd); d->signal_fd = -1; }
}
