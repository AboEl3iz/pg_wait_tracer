/* daemon.h — Daemon state and event loop */
#ifndef PGWT_DAEMON_H
#define PGWT_DAEMON_H

#include "pg_wait_tracer.h"
#include "backend.h"
#include "event_writer.h"
#include "summary_writer.h"
#include "query_text.h"
#include "backend_meta.h"
#include "map_reader.h"
#include "snapshot.h"
#include "provider.h"
#include "escalation.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

/* Forward declaration for skeleton */
struct pg_wait_tracer_bpf;
struct ring_buffer;
struct pgwt_control;
struct pgwt_sampler;

/* Self-observability counters (D6). Plain uint64 increments on the
 * single-threaded event path — exposed via the control socket
 * (src/control.c) with stable snake_case metric names. */
struct pgwt_counters {
    uint64_t events_total;             /* trace events consumed from event_ringbuf */
    uint64_t lifecycle_events_total;   /* fork/init/exit/query_text events */
    uint64_t wp_attach_failures_total; /* watchpoint attach failures (incl. bootstrap) */
    uint64_t prev_events_total;        /* events_total at previous timer tick */
    double   events_per_sec;           /* recent rate, refreshed each tick */

    /* Sampled tier (A2). Maintained by the sampler in sampler.c. */
    uint64_t samples_total;            /* SAMPLES records written */
    uint64_t prev_samples_total;       /* samples_total at previous timer tick */
    double   samples_per_sec;          /* recent sample rate, refreshed each tick */
    uint64_t sample_read_faults_total; /* process_vm_readv partial/EFAULT fallbacks */
};

/* View modes */
enum pgwt_view {
    PGWT_VIEW_TIME_MODEL = 0,
    PGWT_VIEW_SYSTEM_EVENT,
    PGWT_VIEW_SESSION_EVENT,
    PGWT_VIEW_HISTOGRAM,
    PGWT_VIEW_QUERY_EVENT,
    PGWT_VIEW_ACTIVE,
};

/* Exit reasons (for supervision loop) */
enum pgwt_exit_reason {
    PGWT_EXIT_NORMAL = 0,   /* signal, duration, or count */
    PGWT_EXIT_PG_DEAD,      /* postmaster died */
};

/* Sort modes (for active sessions view) */
enum pgwt_sort_mode {
    PGWT_SORT_WAIT_TIME = 0,  /* default: by current wait duration */
    PGWT_SORT_DB_TIME,
    PGWT_SORT_PID,
    PGWT_SORT_EVENT,
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
    struct ring_buffer *rb;          /* lifecycle ringbuf consumer */
    struct ring_buffer *event_rb;    /* trace event ringbuf consumer */

    /* epoll */
    int epoll_fd;
    int timer_fd;
    int sample_timer_fd;   /* high-rate sampler tick (sampled/tiered); -1 if unused */
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
    bool        quiet;               /* suppress view output (record only) */
    int         pg_major_version;   /* 14, 15, 16, 17, or 18 */
    int         st_query_id_offset; /* 0 = not available */
    int         st_activity_offset; /* st_activity_raw in PgBackendStatus, 0 = N/A */
    enum pgwt_format format;         /* output format (TUI/TEXT/JSON/CSV) */
    int         count;               /* max intervals, 0 = unlimited */
    int         tick;                /* current interval number */
    uint64_t    query_id_filter;     /* filter query_event to one query, 0 = no filter */
    enum pgwt_sort_mode sort_mode;   /* sort mode for active view */
    bool        replay_mode;             /* true when running --replay */
    bool        daemon_mode;             /* true when running --daemon */

    /* Capture tier (A2). mode picks the provider; default PGWT_MODE_FULL
     * (today's watchpoint behavior). sample_rate_hz is the sampled tier's
     * fixed rate (1..1000, default 10). */
    enum pgwt_mode mode;
    int         sample_rate_hz;
    int         escalation_budget_s;     /* tiered: full-fidelity s / rolling hour */
    const struct pgwt_capture_provider *provider; /* active provider vtable */
    struct pgwt_sampler *sampler;        /* sampled-tier state, NULL if unused */
    struct pgwt_escalation escalation;   /* tiered escalation engine (D5/A4) */
    uint32_t    lightweight_mode;        /* 1 = BPF accumulator only (no ringbuf) */
    uint32_t    skip_query_id;          /* 1 = skip query_id reads in BPF */
    uint32_t    skip_usdt;             /* 1 = skip USDT query lifecycle probes */
    enum pgwt_exit_reason exit_reason;   /* why the event loop exited */
    char        pgdata[512];             /* stored for restart detection */

    /* Trace file recording */
    const char *trace_dir;                  /* NULL = disabled */
    int         trace_retention;            /* hours, default 24 */
    const char *trace_group;                /* group for trace files, default "dba" */
    struct pgwt_event_writer *event_writer;     /* NULL if disabled */
    struct pgwt_summary_writer *summary_writer; /* NULL if disabled */
    struct pgwt_query_text_capture *query_text_capture; /* NULL if disabled */
    struct pgwt_backend_meta_writer *backend_meta;       /* NULL if disabled */

    /* Time windows */
#define PGWT_MAX_WINDOWS 3
    int         num_windows;                    /* 0 = no windowing */
    int         windows[PGWT_MAX_WINDOWS];      /* window sizes in seconds */
    struct pgwt_ring ring;                       /* snapshot ring buffer */

    /* Event stream */
    struct pgwt_accumulator *event_accum;   /* heap-allocated, cumulative from events */

    /* Control socket (D4) — created when trace_dir is set */
    struct pgwt_control *control;           /* NULL if disabled/unavailable */
    struct pgwt_counters counters;          /* self-observability (D6) */

    /* State */
    struct pgwt_backend_table backends;
    struct pgwt_accumulator accum;
    volatile bool running;
    uint64_t start_ts;

    /* Placed at end of struct to survive field overflow corruption */
    char       *pg_binary_saved;        /* heap-allocated postgres binary path for USDT */
};

/* True when watchpoints should be attached RIGHT NOW.
 *   FULL    — always (the original watchpoint behavior).
 *   SAMPLED — never (registry only, the sampler reads memory directly).
 *   TIERED  — only while escalated: the sampler runs always-on and full
 *             fidelity is armed for bounded windows. De-escalated, tiered
 *             behaves exactly like sampled (no traps on PG).
 * Used by the fork/exit lifecycle to decide whether a new backend gets a
 * bootstrap watchpoint, and by the daemon to gate watchpoint-only setup. */
static inline bool pgwt_mode_uses_watchpoints(const struct pgwt_daemon *d)
{
    if (d->mode == PGWT_MODE_SAMPLED)
        return false;
    if (d->mode == PGWT_MODE_TIERED)
        return d->escalation.active;
    return true;   /* PGWT_MODE_FULL */
}

/* True when the mode runs the always-on userspace sampler (sampled + tiered).
 * Distinct from pgwt_mode_uses_watchpoints(): in tiered mode BOTH can be true
 * at once (sampler always on, watchpoints on during an escalation window). */
static inline bool pgwt_mode_uses_sampler(const struct pgwt_daemon *d)
{
    return d->mode == PGWT_MODE_SAMPLED || d->mode == PGWT_MODE_TIERED;
}

/* Initialize daemon: load BPF, attach tracepoints, scan backends. */
int pgwt_daemon_init(struct pgwt_daemon *d);

/* Run the main event loop. Returns on signal or duration timeout. */
int pgwt_daemon_run(struct pgwt_daemon *d);

/* Clean up all resources. */
void pgwt_daemon_cleanup(struct pgwt_daemon *d);

#endif /* PGWT_DAEMON_H */
