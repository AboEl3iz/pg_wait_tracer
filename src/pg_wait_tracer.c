/* pg_wait_tracer.c — Main entry point: argument parsing, discovery, startup */
#include "daemon.h"
#include "discovery.h"
#include "wait_event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <signal.h>

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
        "  sudo %s --count 10 | cat                          # text mode (piped)\n",
        prog, prog, prog, prog, prog, prog, prog);
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
    {"sort",       required_argument, NULL, 'S'},
    {"verbose",    no_argument,       NULL, 'v'},
    {"help",       no_argument,       NULL, 'h'},
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
    int opt;

    while ((opt = getopt_long(argc, argv, "p:D:i:d:V:f:n:w:e:P:Q:S:vh", long_opts, NULL)) != -1) {
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
        case 'v': d->verbose = true; break;
        case 'h': usage(argv[0]); free(d); return 0;
        default:  usage(argv[0]); free(d); return 1;
        }
    }

    /* TTY auto-detect: tui for terminal, text for pipe */
    if (!format_set)
        d->format = isatty(STDOUT_FILENO) ? PGWT_FMT_TUI : PGWT_FMT_TEXT;

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

    /* Resolve postmaster PID */
    if (pm_pid == 0 && pgdata) {
        pm_pid = pgwt_find_postmaster_pid(pgdata);
        if (pm_pid == 0) {
            fprintf(stderr, "FATAL: cannot read postmaster PID from %s/postmaster.pid\n",
                    pgdata);
            free(d);
            return 1;
        }
    }
    if (pm_pid == 0) {
        /* Auto-discover: find single running PostgreSQL instance */
        pm_pid = pgwt_auto_discover_postmaster(d->verbose);
        if (pm_pid == 0) {
            fprintf(stderr, "\nUse --pid <PID> or --pgdata <DIR>\n");
            free(d);
            return 1;
        }
    }

    /* Verify postmaster is alive */
    if (kill(pm_pid, 0) != 0) {
        fprintf(stderr, "FATAL: postmaster PID %d not found (not running?)\n", pm_pid);
        free(d);
        return 1;
    }
    d->postmaster_pid = pm_pid;

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

    /* Discover postgres binary and version */
    char binary[256];
    if (pgwt_find_pg_binary(pm_pid, binary, sizeof(binary)) != 0) {
        fprintf(stderr, "FATAL: cannot resolve postgres binary for PID %d\n", pm_pid);
        free(d);
        return 1;
    }
    if (d->verbose)
        fprintf(stderr, "INFO: postgres binary: %s\n", binary);

    /* Detect PostgreSQL major version */
    d->pg_major_version = pgwt_detect_pg_version(binary);
    if (d->pg_major_version == 0) {
        fprintf(stderr, "WARN: cannot detect PostgreSQL version, assuming PG18\n");
        d->pg_major_version = 18;
    } else if (d->verbose) {
        fprintf(stderr, "INFO: detected PostgreSQL %d\n", d->pg_major_version);
    }

    /* Initialize version-aware event name tables */
    pgwt_init_event_names(d->pg_major_version);

    /* Extract basename for /proc/pid/maps matching */
    const char *base = strrchr(binary, '/');
    base = base ? base + 1 : binary;

    d->my_wait_ptr_addr = pgwt_resolve_symbol(binary, "my_wait_event_info",
                                               pm_pid, base);
    if (d->my_wait_ptr_addr == 0) {
        fprintf(stderr, "FATAL: cannot resolve 'my_wait_event_info' in %s (PID %d)\n",
                binary, pm_pid);
        free(d);
        return 1;
    }
    if (d->verbose)
        fprintf(stderr, "INFO: my_wait_event_info VA: 0x%lx\n", d->my_wait_ptr_addr);

    /* Verify pointer is readable */
    uint64_t ptr_val = pgwt_read_pointer(pm_pid, d->my_wait_ptr_addr);
    if (d->verbose)
        fprintf(stderr, "INFO: my_wait_event_info value (postmaster): 0x%lx\n", ptr_val);

    /* Discover MyBEEntry address (for query_id attribution) */
    d->my_be_entry_addr = pgwt_resolve_symbol(binary, "MyBEEntry",
                                               pm_pid, base);
    if (d->my_be_entry_addr == 0) {
        if (d->view == PGWT_VIEW_QUERY_EVENT) {
            fprintf(stderr, "FATAL: symbol 'MyBEEntry' not found — query_event view unavailable\n");
            free(d);
            return 1;
        }
        fprintf(stderr, "WARN: symbol 'MyBEEntry' not found — query_event view disabled\n");
    } else if (d->verbose) {
        fprintf(stderr, "INFO: MyBEEntry VA: 0x%lx\n",
                (unsigned long)d->my_be_entry_addr);
    }

    /* Detect st_query_id offset for query_event view */
    d->st_query_id_offset = pgwt_detect_query_id_offset(binary, d->pg_major_version);
    if (d->st_query_id_offset > 0) {
        if (d->verbose)
            fprintf(stderr, "INFO: st_query_id offset: %d (PG%d)\n",
                    d->st_query_id_offset, d->pg_major_version);
    } else {
        if (d->view == PGWT_VIEW_QUERY_EVENT) {
            fprintf(stderr, "FATAL: st_query_id offset not found for PG%d — "
                    "query_event view unavailable\n"
                    "  Hint: install postgresql-%d-dbgsym for DWARF-based detection\n",
                    d->pg_major_version, d->pg_major_version);
            free(d);
            return 1;
        }
        if (d->verbose)
            fprintf(stderr, "INFO: st_query_id offset not available — "
                    "query_event view disabled\n");
    }

    /* Init daemon: load BPF, attach tracepoints, scan backends */
    if (pgwt_daemon_init(d) != 0) {
        free(d);
        return 1;
    }

    /* Run event loop */
    pgwt_daemon_run(d);

    /* Cleanup */
    pgwt_daemon_cleanup(d);
    free(d);
    return 0;
}
