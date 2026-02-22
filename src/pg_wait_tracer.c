/* pg_wait_tracer.c — Main entry point: argument parsing, discovery, startup */
#include "daemon.h"
#include "discovery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <signal.h>

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --pid <POSTMASTER_PID> [OPTIONS]\n"
        "       %s --pgdata <PGDATA_DIR>   [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  -p, --pid <PID>       Postmaster PID\n"
        "  -D, --pgdata <DIR>    PGDATA directory (reads postmaster.pid)\n"
        "  -i, --interval <SEC>  Refresh interval in seconds (default: 5)\n"
        "  -d, --duration <SEC>  Run for N seconds then exit (default: unlimited)\n"
        "  -V, --view <VIEW>     Output view: time_model (default), system_event,\n"
        "                        session_event, histogram, query_event\n"
        "  -e, --event <NAME>    Event filter for histogram view (e.g. IO:DataFileRead)\n"
        "  -P, --pid-filter <PID> Show detail for specific backend PID (session_event)\n"
        "  -v, --verbose         Verbose output to stderr\n"
        "  -h, --help            Show this help\n"
        "\n"
        "Examples:\n"
        "  sudo %s --pid 12345\n"
        "  sudo %s --pgdata /var/lib/postgresql/18/main --view system_event\n"
        "  sudo %s --pid 12345 --view histogram --event IO:DataFileRead\n"
        "  sudo %s --pid 12345 --view session_event --pid-filter 12400\n",
        prog, prog, prog, prog, prog, prog);
}

static enum pgwt_view parse_view(const char *s)
{
    if (strcmp(s, "time_model") == 0)     return PGWT_VIEW_TIME_MODEL;
    if (strcmp(s, "system_event") == 0)   return PGWT_VIEW_SYSTEM_EVENT;
    if (strcmp(s, "session_event") == 0)  return PGWT_VIEW_SESSION_EVENT;
    if (strcmp(s, "histogram") == 0)      return PGWT_VIEW_HISTOGRAM;
    if (strcmp(s, "query_event") == 0)    return PGWT_VIEW_QUERY_EVENT;
    fprintf(stderr, "ERROR: unknown view '%s'\n", s);
    exit(1);
}

static struct option long_opts[] = {
    {"pid",        required_argument, NULL, 'p'},
    {"pgdata",     required_argument, NULL, 'D'},
    {"interval",   required_argument, NULL, 'i'},
    {"duration",   required_argument, NULL, 'd'},
    {"view",       required_argument, NULL, 'V'},
    {"event",      required_argument, NULL, 'e'},
    {"pid-filter", required_argument, NULL, 'P'},
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
    int opt;

    while ((opt = getopt_long(argc, argv, "p:D:i:d:V:e:P:vh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'p': pm_pid = atoi(optarg); break;
        case 'D': pgdata = optarg; break;
        case 'i': d->interval = atoi(optarg); break;
        case 'd': d->duration = atoi(optarg); break;
        case 'V': d->view = parse_view(optarg); break;
        case 'e': d->event_filter = optarg; break;
        case 'P': d->pid_filter = atoi(optarg); break;
        case 'v': d->verbose = true; break;
        case 'h': usage(argv[0]); free(d); return 0;
        default:  usage(argv[0]); free(d); return 1;
        }
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
        fprintf(stderr, "FATAL: must specify --pid or --pgdata\n\n");
        usage(argv[0]);
        free(d);
        return 1;
    }

    /* Verify postmaster is alive */
    if (kill(pm_pid, 0) != 0) {
        fprintf(stderr, "FATAL: postmaster PID %d not found (not running?)\n", pm_pid);
        free(d);
        return 1;
    }
    d->postmaster_pid = pm_pid;

    /* Validate options */
    if (d->interval < 1) {
        fprintf(stderr, "FATAL: interval must be >= 1 second\n");
        free(d);
        return 1;
    }
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

    /* Discover my_wait_event_info address */
    char binary[256];
    if (pgwt_find_pg_binary(pm_pid, binary, sizeof(binary)) != 0) {
        fprintf(stderr, "FATAL: cannot resolve postgres binary for PID %d\n", pm_pid);
        free(d);
        return 1;
    }
    if (d->verbose)
        fprintf(stderr, "INFO: postgres binary: %s\n", binary);

    uint64_t sym_offset = pgwt_find_symbol_offset(binary, "my_wait_event_info");
    if (sym_offset == 0) {
        fprintf(stderr, "FATAL: symbol 'my_wait_event_info' not found in %s\n", binary);
        free(d);
        return 1;
    }
    if (d->verbose)
        fprintf(stderr, "INFO: my_wait_event_info offset: 0x%lx\n", sym_offset);

    /* Extract basename for /proc/pid/maps matching */
    const char *base = strrchr(binary, '/');
    base = base ? base + 1 : binary;

    uint64_t load_base = pgwt_find_load_base(pm_pid, base);
    if (load_base == 0) {
        fprintf(stderr, "FATAL: cannot find load base for '%s' in PID %d maps\n",
                base, pm_pid);
        free(d);
        return 1;
    }

    d->my_wait_ptr_addr = load_base + sym_offset;
    if (d->verbose)
        fprintf(stderr, "INFO: my_wait_event_info VA: 0x%lx (base=0x%lx + offset=0x%lx)\n",
                d->my_wait_ptr_addr, load_base, sym_offset);

    /* Verify pointer is readable */
    uint64_t ptr_val = pgwt_read_pointer(pm_pid, d->my_wait_ptr_addr);
    if (d->verbose)
        fprintf(stderr, "INFO: my_wait_event_info value (postmaster): 0x%lx\n", ptr_val);

    /* Discover MyBEEntry address (for query_id attribution) */
    uint64_t be_sym_offset = pgwt_find_symbol_offset(binary, "MyBEEntry");
    if (be_sym_offset == 0) {
        if (d->view == PGWT_VIEW_QUERY_EVENT) {
            fprintf(stderr, "FATAL: symbol 'MyBEEntry' not found — query_event view unavailable\n");
            free(d);
            return 1;
        }
        fprintf(stderr, "WARN: symbol 'MyBEEntry' not found — query_event view disabled\n");
    } else {
        d->my_be_entry_addr = load_base + be_sym_offset;
        if (d->verbose)
            fprintf(stderr, "INFO: MyBEEntry VA: 0x%lx\n",
                    (unsigned long)d->my_be_entry_addr);
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
