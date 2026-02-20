/* daemon.h — Daemon state and event loop */
#ifndef PGWT_DAEMON_H
#define PGWT_DAEMON_H

#include "pg_wait_tracer.h"
#include "backend.h"
#include "map_reader.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

/* Forward declaration for skeleton */
struct pg_wait_tracer_bpf;
struct ring_buffer;

/* View modes */
enum pgwt_view {
    PGWT_VIEW_TIME_MODEL = 0,
    PGWT_VIEW_SYSTEM_EVENT,
    PGWT_VIEW_SESSION_EVENT,
    PGWT_VIEW_HISTOGRAM,
};

struct pgwt_daemon {
    /* BPF */
    struct pg_wait_tracer_bpf *skel;
    struct ring_buffer *rb;

    /* epoll */
    int epoll_fd;
    int timer_fd;
    int signal_fd;

    /* Configuration */
    pid_t       postmaster_pid;
    uint64_t    my_wait_ptr_addr;
    int         interval;         /* seconds */
    int         duration;         /* seconds, 0 = unlimited */
    enum pgwt_view view;
    const char *event_filter;     /* for histogram view */
    pid_t       pid_filter;       /* for session_event detail */
    bool        verbose;

    /* State */
    struct pgwt_backend_table backends;
    struct pgwt_accumulator accum;
    volatile bool running;
    uint64_t start_ts;
};

/* Initialize daemon: load BPF, attach tracepoints, scan backends. */
int pgwt_daemon_init(struct pgwt_daemon *d);

/* Run the main event loop. Returns on signal or duration timeout. */
int pgwt_daemon_run(struct pgwt_daemon *d);

/* Clean up all resources. */
void pgwt_daemon_cleanup(struct pgwt_daemon *d);

#endif /* PGWT_DAEMON_H */
