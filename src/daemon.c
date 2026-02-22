/* daemon.c — Main event loop: epoll on ring buffer, timer, signals */
#include "daemon.h"
#include "backend.h"
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

    pgwt_read_maps(d);

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
    }
    fflush(stdout);
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

    /* Set up ring buffer consumer */
    int rb_map_fd = bpf_map__fd(d->skel->maps.lifecycle_rb);
    d->rb = ring_buffer__new(rb_map_fd, handle_lifecycle_event, d, NULL);
    if (!d->rb) {
        fprintf(stderr, "FATAL: cannot create ring buffer: %s\n",
                strerror(errno));
        goto fail;
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

    /* Add ring buffer fd */
    int rb_fd = ring_buffer__epoll_fd(d->rb);
    ev.events = EPOLLIN;
    ev.data.fd = rb_fd;
    epoll_ctl(d->epoll_fd, EPOLL_CTL_ADD, rb_fd, &ev);

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

    uint64_t deadline = 0;
    if (d->duration > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        deadline = (uint64_t)ts.tv_sec + d->duration;
    }

    while (d->running) {
        int timeout_ms = -1;
        if (deadline > 0) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            if ((uint64_t)ts.tv_sec >= deadline) break;
            timeout_ms = (deadline - ts.tv_sec) * 1000;
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
            } else if (events[i].data.fd == d->signal_fd) {
                handle_signal(d);
            }
        }
    }

    fprintf(stderr, "\npg_wait_tracer: shutting down\n");
    return 0;
}

void pgwt_daemon_cleanup(struct pgwt_daemon *d)
{
    pgwt_close_all_backends(&d->backends);

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
