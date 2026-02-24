/* daemon.h — Daemon state and event loop */
#ifndef PGWT_DAEMON_H
#define PGWT_DAEMON_H

#include "pg_wait_tracer.h"
#include "backend.h"
#include "map_reader.h"
#include "snapshot.h"

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
    PGWT_VIEW_QUERY_EVENT,
};

/* Output formats */
enum pgwt_format {
    PGWT_FMT_TUI = 0,   /* screen-clearing interactive (default for TTY) */
    PGWT_FMT_TEXT,       /* no screen clear, timestamp per interval (default for pipe) */
    PGWT_FMT_JSON,       /* JSONL — one JSON object per interval (future) */
    PGWT_FMT_CSV,        /* flat rows, one per event per interval (future) */
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
    uint64_t    my_be_entry_addr; /* address of MyBEEntry (for query_id) */
    int         interval;         /* seconds */
    int         duration;         /* seconds, 0 = unlimited */
    enum pgwt_view view;
    const char *event_filter;     /* for histogram view */
    pid_t       pid_filter;       /* for session_event detail */
    bool        verbose;
    int         pg_major_version;   /* 14, 15, 16, 17, or 18 */
    int         st_query_id_offset; /* 0 = not available */
    enum pgwt_format format;         /* output format (TUI/TEXT/JSON/CSV) */
    int         count;               /* max intervals, 0 = unlimited */
    int         tick;                /* current interval number */
    uint64_t    query_id_filter;     /* filter query_event to one query, 0 = no filter */

    /* Time windows */
#define PGWT_MAX_WINDOWS 3
    int         num_windows;                    /* 0 = no windowing */
    int         windows[PGWT_MAX_WINDOWS];      /* window sizes in seconds */
    struct pgwt_ring ring;                       /* snapshot ring buffer */

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
