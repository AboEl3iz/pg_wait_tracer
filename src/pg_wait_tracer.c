/* pg_wait_tracer.c — Main entry point: argument parsing, discovery, startup */
#include "daemon.h"
#include "discovery.h"
#include "replay.h"
#include "wait_event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Target (auto-detect if omitted for single instance):\n"
        "  -p, --pid <PID>       Postmaster PID\n"
        "  -D, --pgdata <DIR>    PGDATA directory (reads postmaster.pid)\n"
        "\n"
        "Output control:\n"
        "  -V, --view <VIEW>     time_model (default), system_event, session_event,\n"
        "                        histogram, query_event, active\n"
        "  -S, --sort <MODE>     Sort for active view: wait_time (default), db_time,\n"
        "                        pid, event\n"
        "  -f, --format <FMT>    tui | text (default: auto-detect TTY)\n"
        "  -i, --interval <SEC>  Refresh interval in seconds (default: 5)\n"
        "  -d, --duration <SEC>  Run for N seconds then exit (default: unlimited)\n"
        "  -n, --count <N>       Print N intervals then exit\n"
        "  -w, --window <W1,W2,W3>  Time windows, e.g. 5s,1m,5m (first = interval)\n"
        "\n"
        "Filters:\n"
        "  -e, --event <NAME>    Event filter (histogram: required; query_event: by event)\n"
        "  -P, --pid-filter <PID> Show detail for specific backend (session_event)\n"
        "  -Q, --query-id <ID>   Filter query_event to one query\n"
        "\n"
        "Recording:\n"
        "  -T, --trace-dir <DIR>      Write raw trace files to DIR\n"
        "  -R, --trace-retention <H>  Keep trace files for H hours (default: 24)\n"
        "      --trace-group <GROUP>  Group for trace file access (default: dba)\n"
        "\n"
        "Replay (offline analysis — no root, no PostgreSQL needed):\n"
        "      --replay               Replay trace files instead of live tracing\n"
        "      --from <TIME>          Start time: ISO 8601, relative (1h, 30m), or 'now'\n"
        "      --to <TIME>            End time (same formats as --from)\n"
        "\n"
        "Daemon:\n"
        "      --daemon               Run as daemon (reconnect on PG restart)\n"
        "\n"
        "Other:\n"
        "  -v, --verbose         Verbose output to stderr\n"
        "  -h, --help            Show this help\n"
        "\n"
        "Examples:\n"
        "  sudo %s                                           # auto-detect instance\n"
        "  sudo %s --pid 12345 --count 1                     # one-shot\n"
        "  sudo %s --view system_event --count 5             # 5 intervals\n"
        "  sudo %s --view histogram --event IO:DataFileRead\n"
        "  sudo %s --window 5s,1m,5m --count 3                 # time windows\n"
        "  sudo %s --count 10 | cat                          # text mode (piped)\n"
        "\n"
        "  # Daemon (reconnects on PG restart):\n"
        "  sudo %s --daemon -T /tmp/traces\n"
        "  sudo %s --daemon --pgdata /var/lib/pgsql/18/data\n"
        "\n"
        "  # Replay (no root needed):\n"
        "  %s --replay -T /tmp/traces --view time_model\n"
        "  %s --replay -T /tmp/traces --from 1h --view system_event\n"
        "  %s --replay -T /tmp/traces --from '2025-02-25T14:00:00' --to '2025-02-25T15:00:00'\n",
        prog, prog, prog, prog, prog, prog, prog,
        prog, prog,
        prog, prog, prog);
}

static enum pgwt_view parse_view(const char *s)
{
    if (strcmp(s, "time_model") == 0)     return PGWT_VIEW_TIME_MODEL;
    if (strcmp(s, "system_event") == 0)   return PGWT_VIEW_SYSTEM_EVENT;
    if (strcmp(s, "session_event") == 0)  return PGWT_VIEW_SESSION_EVENT;
    if (strcmp(s, "histogram") == 0)      return PGWT_VIEW_HISTOGRAM;
    if (strcmp(s, "query_event") == 0)    return PGWT_VIEW_QUERY_EVENT;
    if (strcmp(s, "active") == 0)        return PGWT_VIEW_ACTIVE;
    fprintf(stderr, "ERROR: unknown view '%s'\n", s);
    exit(1);
}

static enum pgwt_sort_mode parse_sort(const char *s)
{
    if (strcmp(s, "wait_time") == 0)  return PGWT_SORT_WAIT_TIME;
    if (strcmp(s, "db_time") == 0)    return PGWT_SORT_DB_TIME;
    if (strcmp(s, "pid") == 0)        return PGWT_SORT_PID;
    if (strcmp(s, "event") == 0)      return PGWT_SORT_EVENT;
    fprintf(stderr, "ERROR: unknown sort mode '%s' (use: wait_time, db_time, pid, event)\n", s);
    exit(1);
}

static int parse_windows(const char *s, int *windows, int *num_windows)
{
    char buf[128];
    int n = 0;

    snprintf(buf, sizeof(buf), "%s", s);

    char *tok = strtok(buf, ",");
    while (tok && n < PGWT_MAX_WINDOWS) {
        char *end;
        long val = strtol(tok, &end, 10);
        if (val <= 0 || end == tok) {
            fprintf(stderr, "ERROR: invalid window value '%s'\n", tok);
            return -1;
        }

        int secs;
        switch (*end) {
        case 's': case '\0': secs = (int)val; break;
        case 'm':            secs = (int)val * 60; break;
        case 'h':            secs = (int)val * 3600; break;
        default:
            fprintf(stderr, "ERROR: invalid window suffix '%c' (use s, m, h)\n", *end);
            return -1;
        }

        if (n > 0 && secs <= windows[n - 1]) {
            fprintf(stderr, "FATAL: windows must be in increasing order (%ds <= %ds)\n",
                    secs, windows[n - 1]);
            return -1;
        }

        windows[n++] = secs;
        tok = strtok(NULL, ",");
    }

    if (n == 0) {
        fprintf(stderr, "ERROR: --window requires at least one value\n");
        return -1;
    }

    *num_windows = n;
    return 0;
}

static enum pgwt_format parse_format(const char *s)
{
    if (strcmp(s, "tui") == 0)   return PGWT_FMT_TUI;
    if (strcmp(s, "text") == 0)  return PGWT_FMT_TEXT;
    if (strcmp(s, "json") == 0)  return PGWT_FMT_JSON;
    if (strcmp(s, "csv") == 0)   return PGWT_FMT_CSV;
    fprintf(stderr, "ERROR: unknown format '%s' (use: tui, text, json, csv)\n", s);
    exit(1);
}

/* Long-only option values (no short form) */
#define OPT_REPLAY 256
#define OPT_FROM   257
#define OPT_TO     258
#define OPT_DAEMON       259
#define OPT_TRACE_GROUP  260

static struct option long_opts[] = {
    {"pid",        required_argument, NULL, 'p'},
    {"pgdata",     required_argument, NULL, 'D'},
    {"interval",   required_argument, NULL, 'i'},
    {"duration",   required_argument, NULL, 'd'},
    {"view",       required_argument, NULL, 'V'},
    {"format",     required_argument, NULL, 'f'},
    {"count",      required_argument, NULL, 'n'},
    {"window",     required_argument, NULL, 'w'},
    {"event",      required_argument, NULL, 'e'},
    {"pid-filter", required_argument, NULL, 'P'},
    {"query-id",   required_argument, NULL, 'Q'},
    {"sort",            required_argument, NULL, 'S'},
    {"trace-dir",       required_argument, NULL, 'T'},
    {"trace-retention", required_argument, NULL, 'R'},
    {"replay",          no_argument,       NULL, OPT_REPLAY},
    {"from",            required_argument, NULL, OPT_FROM},
    {"to",              required_argument, NULL, OPT_TO},
    {"daemon",          no_argument,       NULL, OPT_DAEMON},
    {"trace-group",     required_argument, NULL, OPT_TRACE_GROUP},
    {"verbose",         no_argument,       NULL, 'v'},
    {"help",            no_argument,       NULL, 'h'},
    {NULL, 0, NULL, 0},
};

int main(int argc, char **argv)
{
    /* Heap-allocate: pgwt_daemon is ~27 MB due to accumulator arrays */
    struct pgwt_daemon *d = calloc(1, sizeof(*d));
    if (!d) {
        fprintf(stderr, "FATAL: cannot allocate daemon state\n");
        return 1;
    }
    d->epoll_fd  = -1;
    d->timer_fd  = -1;
    d->signal_fd = -1;
    d->interval  = 5;
    d->view      = PGWT_VIEW_TIME_MODEL;

    pid_t pm_pid = 0;
    const char *pgdata = NULL;
    bool format_set = false;
    bool replay_mode = false;
    bool daemon_mode = false;
    const char *from_str = NULL;
    const char *to_str = NULL;
    int opt;

    while ((opt = getopt_long(argc, argv, "p:D:i:d:V:f:n:w:e:P:Q:S:T:R:vh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'p': pm_pid = atoi(optarg); break;
        case 'D': pgdata = optarg; break;
        case 'i': d->interval = atoi(optarg); break;
        case 'd': d->duration = atoi(optarg); break;
        case 'V': d->view = parse_view(optarg); break;
        case 'f': d->format = parse_format(optarg); format_set = true; break;
        case 'n': d->count = atoi(optarg); break;
        case 'w':
            if (parse_windows(optarg, d->windows, &d->num_windows) != 0) {
                free(d);
                return 1;
            }
            break;
        case 'e': d->event_filter = optarg; break;
        case 'P': d->pid_filter = atoi(optarg); break;
        case 'Q': d->query_id_filter = strtoull(optarg, NULL, 10); break;
        case 'S': d->sort_mode = parse_sort(optarg); break;
        case 'T': d->trace_dir = optarg; break;
        case 'R': d->trace_retention = atoi(optarg); break;
        case OPT_REPLAY: replay_mode = true; break;
        case OPT_FROM:   from_str = optarg; break;
        case OPT_TO:     to_str = optarg; break;
        case OPT_DAEMON: daemon_mode = true; break;
        case OPT_TRACE_GROUP: d->trace_group = optarg; break;
        case 'v': d->verbose = true; break;
        case 'h': usage(argv[0]); free(d); return 0;
        default:  usage(argv[0]); free(d); return 1;
        }
    }

    /* Default trace group to "dba" when trace recording is enabled */
    if (d->trace_dir && !d->trace_group)
        d->trace_group = "dba";

    /* TTY auto-detect: tui for terminal, text for pipe */
    if (!format_set)
        d->format = isatty(STDOUT_FILENO) ? PGWT_FMT_TUI : PGWT_FMT_TEXT;

    /* Replay mode: bypass all BPF/PostgreSQL discovery */
    if (replay_mode) {
        d->format = PGWT_FMT_TEXT;  /* replay always text mode */
        int rc = pgwt_run_replay(d, from_str, to_str);
        free(d);
        return rc;
    }

    /* Validate options (before PG discovery) */
    if (d->interval < 1) {
        fprintf(stderr, "FATAL: interval must be >= 1 second\n");
        free(d);
        return 1;
    }
    if (d->num_windows > 0 && d->windows[0] != d->interval) {
        fprintf(stderr, "FATAL: first window (%ds) must equal --interval (%ds)\n",
                d->windows[0], d->interval);
        free(d);
        return 1;
    }
    if (daemon_mode && (d->count > 0 || d->duration > 0)) {
        fprintf(stderr, "FATAL: --daemon cannot be used with --count or --duration\n");
        free(d);
        return 1;
    }

    /* Validate view-specific options */
    if (d->view == PGWT_VIEW_HISTOGRAM && (!d->event_filter || !d->event_filter[0])) {
        fprintf(stderr, "FATAL: histogram view requires --event <NAME>\n");
        free(d);
        return 1;
    }

    /* Check root */
    if (geteuid() != 0) {
        fprintf(stderr, "FATAL: must run as root (hardware watchpoints require CAP_SYS_ADMIN)\n");
        free(d);
        return 1;
    }

    d->daemon_mode = daemon_mode;

    /* Set up discovery state: pgdata, pre-set PID, or auto-discover.
     * pgwt_discover() uses: d->pgdata (if set) > d->postmaster_pid (if set) > auto. */
    if (pgdata)
        snprintf(d->pgdata, sizeof(d->pgdata), "%s", pgdata);
    if (pm_pid > 0)
        d->postmaster_pid = pm_pid;

    /* In daemon mode with --pid but no --pgdata, infer PGDATA for restart detection */
    if (daemon_mode && pm_pid > 0 && !pgdata) {
        char inferred[512];
        if (pgwt_infer_pgdata(pm_pid, inferred, sizeof(inferred)) == 0) {
            snprintf(d->pgdata, sizeof(d->pgdata), "%s", inferred);
            if (d->verbose)
                fprintf(stderr, "INFO: inferred PGDATA: %s\n", d->pgdata);
        } else {
            fprintf(stderr, "WARN: cannot infer PGDATA from PID %d — "
                    "restart detection may not work\n", pm_pid);
        }
    }

    /* First discovery */
    if (pgwt_discover(d) != 0) {
        if (!pgdata && pm_pid == 0)
            fprintf(stderr, "\nUse --pid <PID> or --pgdata <DIR>\n");
        free(d);
        return 1;
    }

    /* ── Daemon mode: supervision loop ─────────────────────── */
    if (daemon_mode) {
        int rc = 0;

        /* Block signals for sigtimedwait in wait phase.
         * pgwt_daemon_init() will also call sigprocmask (idempotent). */
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTERM);
        sigprocmask(SIG_BLOCK, &mask, NULL);

        for (;;) {
            if (pgwt_daemon_init(d) != 0) {
                rc = 1;
                break;
            }

            pgwt_daemon_run(d);
            pgwt_daemon_cleanup(d);

            if (d->exit_reason != PGWT_EXIT_PG_DEAD)
                break;

            /* Wait for PG restart */
            fprintf(stderr, "pg_wait_tracer: waiting for PostgreSQL to restart...\n");
            bool found = false;
            while (!found) {
                struct timespec ts = { .tv_sec = 5 };
                int sig = sigtimedwait(&mask, NULL, &ts);
                if (sig > 0) {
                    fprintf(stderr, "\npg_wait_tracer: shutting down\n");
                    goto done;
                }
                /* sigtimedwait returns -1/EAGAIN on timeout — try discover */

                /* Clear pre-set PID so discover uses pgdata or auto */
                d->postmaster_pid = 0;

                if (pgwt_discover(d) == 0) {
                    fprintf(stderr, "pg_wait_tracer: PostgreSQL restarted (PID %d), "
                            "re-attaching\n", d->postmaster_pid);
                    found = true;
                }
            }

            /* Reset per-cycle state */
            d->tick = 0;
            d->exit_reason = PGWT_EXIT_NORMAL;
        }

done:
        free(d);
        return rc;
    }

    /* ── Single-shot mode ──────────────────────────────────── */
    if (pgwt_daemon_init(d) != 0) {
        free(d);
        return 1;
    }

    pgwt_daemon_run(d);
    pgwt_daemon_cleanup(d);
    free(d);
    return 0;
}
