/* output.c — CLI output formatters for the 4 diagnostic views */
#include "output.h"
#include "daemon.h"
#include "map_reader.h"
#include "wait_event.h"
#include "backend.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define LINE "════════════════════════════════════════════════════════════════════════════════"
#define DASH "────────────────────────────────────────────────────────────────────────────────"

static double ns_to_ms(uint64_t ns) { return (double)ns / 1e6; }
static double ns_to_us(uint64_t ns) { return (double)ns / 1e3; }

/* ── Multi-window helpers ───────────────────────────────── */

static void format_window_label(int secs, char *buf, size_t bufsz)
{
    if (secs >= 3600 && secs % 3600 == 0)
        snprintf(buf, bufsz, "Last %dh", secs / 3600);
    else if (secs >= 60 && secs % 60 == 0)
        snprintf(buf, bufsz, "Last %dm", secs / 60);
    else
        snprintf(buf, bufsz, "Last %ds", secs);
}

static uint64_t find_snap_event_ns(const struct pgwt_snapshot *snap,
                                   uint32_t we)
{
    for (int i = 0; i < snap->num_events; i++)
        if (snap->events[i].wait_event == we)
            return snap->events[i].total_ns;
    return 0;
}

static uint64_t tm_class_ns(const struct pgwt_time_model *tm,
                            uint32_t class_id)
{
    switch (class_id) {
    case PG_WAIT_IO:        return tm->io_time_ns;
    case PG_WAIT_LWLOCK:    return tm->lwlock_time_ns;
    case PG_WAIT_LOCK:      return tm->lock_time_ns;
    case PG_WAIT_CLIENT:    return tm->client_time_ns;
    case PG_WAIT_IPC:       return tm->ipc_time_ns;
    case PG_WAIT_BUFFERPIN: return tm->bufferpin_time_ns;
    case PG_WAIT_TIMEOUT:   return tm->timeout_time_ns;
    case PG_WAIT_EXTENSION: return tm->extension_time_ns;
    default:                return 0;
    }
}

static int count_active_backends(struct pgwt_daemon *d)
{
    int n = 0;
    for (int i = 0; i < d->backends.count; i++)
        if (d->backends.entries[i].is_alive)
            n++;
    return n;
}

/* Print view separator: TUI clears screen, text prints timestamp */
static void print_view_start(struct pgwt_daemon *d)
{
    if (d->format == PGWT_FMT_TUI) {
        printf("\033[2J\033[H");
    } else if (d->format == PGWT_FMT_TEXT) {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        printf("\n--- %04d-%02d-%02d %02d:%02d:%02d ---\n",
               t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
               t->tm_hour, t->tm_min, t->tm_sec);
    }
}

void pgwt_print_header(struct pgwt_daemon *d)
{
    fprintf(stderr, "pg_wait_tracer v0.1 — postmaster PID %d\n",
            d->postmaster_pid);
    fprintf(stderr, "Tracing %d backends | %ds interval | Ctrl-C to stop\n\n",
            count_active_backends(d), d->interval);
}

/* ── Time Model View ─────────────────────────────────────── */

/* Multi-window time_model: side-by-side columns per window */
static void print_time_model_multi(struct pgwt_daemon *d)
{
    int nw = d->num_windows;
    struct pgwt_snapshot deltas[PGWT_MAX_WINDOWS];
    int valid[PGWT_MAX_WINDOWS] = {0};

    for (int w = 0; w < nw; w++) {
        int ticks = d->windows[w] / d->interval;
        valid[w] = (pgwt_ring_delta(&d->ring, ticks, &deltas[w]) == 0);
    }

    if (!valid[0]) {
        printf("  (waiting for data)\n\n");
        return;
    }

    /* Column headers */
    printf("  %-32s", "Stat Name");
    for (int w = 0; w < nw; w++) {
        char label[16];
        format_window_label(d->windows[w], label, sizeof(label));
        printf(" %9s %7s", label, "% DB");
    }
    printf("\n");

    /* Separator */
    printf("  ");
    for (int i = 0; i < 32; i++) putchar('-');
    for (int w = 0; w < nw; w++) {
        putchar(' ');
        for (int i = 0; i < 9; i++) putchar('-');
        putchar(' ');
        for (int i = 0; i < 7; i++) putchar('-');
    }
    printf("\n");

    /* DB Time */
    printf("  %-32s", "DB Time");
    for (int w = 0; w < nw; w++) {
        if (!valid[w]) { printf(" %9s %7s", "-", "-"); continue; }
        printf(" %9.1f %6.1f%%", ns_to_ms(deltas[w].tm.db_time_ns), 100.0);
    }
    printf("\n");

    /* CPU* */
    printf("    %-30s", "CPU*");
    for (int w = 0; w < nw; w++) {
        if (!valid[w]) { printf(" %9s %7s", "-", "-"); continue; }
        uint64_t db = deltas[w].tm.db_time_ns;
        printf(" %9.1f %6.1f%%", ns_to_ms(deltas[w].tm.cpu_time_ns),
               db ? 100.0 * deltas[w].tm.cpu_time_ns / db : 0);
    }
    printf("\n");

    /* Wait classes sorted by first window */
    struct { const char *name; uint32_t class_id; uint64_t sort_ns; } classes[] = {
        {"IO",        PG_WAIT_IO,        deltas[0].tm.io_time_ns},
        {"LWLock",    PG_WAIT_LWLOCK,    deltas[0].tm.lwlock_time_ns},
        {"Lock",      PG_WAIT_LOCK,      deltas[0].tm.lock_time_ns},
        {"Client",    PG_WAIT_CLIENT,    deltas[0].tm.client_time_ns},
        {"IPC",       PG_WAIT_IPC,       deltas[0].tm.ipc_time_ns},
        {"BufferPin", PG_WAIT_BUFFERPIN, deltas[0].tm.bufferpin_time_ns},
        {"Timeout",   PG_WAIT_TIMEOUT,   deltas[0].tm.timeout_time_ns},
        {"Extension", PG_WAIT_EXTENSION, deltas[0].tm.extension_time_ns},
    };
    int nc = sizeof(classes) / sizeof(classes[0]);

    for (int i = 0; i < nc - 1; i++)
        for (int j = i + 1; j < nc; j++)
            if (classes[j].sort_ns > classes[i].sort_ns) {
                typeof(classes[0]) tmp = classes[i];
                classes[i] = classes[j];
                classes[j] = tmp;
            }

#define MAX_SUB_EVENTS_M 3
#define MIN_DB_PCT_M     1.0

    uint64_t db0 = deltas[0].tm.db_time_ns;

    for (int i = 0; i < nc; i++) {
        if (classes[i].sort_ns == 0) continue;

        /* Class row */
        printf("    %-30s", classes[i].name);
        for (int w = 0; w < nw; w++) {
            if (!valid[w]) { printf(" %9s %7s", "-", "-"); continue; }
            uint64_t db = deltas[w].tm.db_time_ns;
            uint64_t cls_ns = tm_class_ns(&deltas[w].tm, classes[i].class_id);
            printf(" %9.1f %6.1f%%", ns_to_ms(cls_ns),
                   db ? 100.0 * cls_ns / db : 0);
        }
        printf("\n");

        /* Top sub-events from first window */
        struct { uint32_t we; uint64_t total_ns; } top[MAX_SUB_EVENTS_M];
        int ntop = 0;

        for (int j = 0; j < deltas[0].num_events; j++) {
            struct pgwt_snap_event *ev = &deltas[0].events[j];
            if (ev->count == 0) continue;
            if (WE_CLASS(ev->wait_event) != classes[i].class_id) continue;
            if (db0 == 0 || 100.0 * ev->total_ns / db0 < MIN_DB_PCT_M)
                continue;

            int pos = ntop;
            for (int k = 0; k < ntop; k++) {
                if (ev->total_ns > top[k].total_ns) { pos = k; break; }
            }
            if (pos < MAX_SUB_EVENTS_M) {
                int tail = ntop < MAX_SUB_EVENTS_M ? ntop : MAX_SUB_EVENTS_M - 1;
                for (int k = tail; k > pos; k--)
                    top[k] = top[k - 1];
                top[pos].we = ev->wait_event;
                top[pos].total_ns = ev->total_ns;
                if (ntop < MAX_SUB_EVENTS_M)
                    ntop++;
            }
        }

        for (int t = 0; t < ntop; t++) {
            char name[64];
            pgwt_event_full_name(top[t].we, name, sizeof(name));
            printf("      %-28s", name);
            for (int w = 0; w < nw; w++) {
                if (!valid[w]) { printf(" %9s %7s", "-", "-"); continue; }
                uint64_t db = deltas[w].tm.db_time_ns;
                uint64_t ev_ns = find_snap_event_ns(&deltas[w], top[t].we);
                printf(" %9.1f %6.1f%%", ns_to_ms(ev_ns),
                       db ? 100.0 * ev_ns / db : 0);
            }
            printf("\n");
        }
    }

#undef MAX_SUB_EVENTS_M
#undef MIN_DB_PCT_M

    /* Activity/Idle */
    if (deltas[0].tm.activity_time_ns > 0) {
        printf("\n  %-32s", "(Activity/Idle)");
        for (int w = 0; w < nw; w++) {
            if (!valid[w]) { printf(" %9s %7s", "-", "-"); continue; }
            printf(" %9.1f %7s", ns_to_ms(deltas[w].tm.activity_time_ns), "-");
        }
        printf("\n");
    }

    printf("\n");
}

void pgwt_print_time_model(struct pgwt_daemon *d)
{
    struct pgwt_accumulator *acc = &d->accum;
    struct pgwt_time_model *tm = &acc->tm;

    print_view_start(d);
    printf("%s\n", LINE);
    printf("pg_wait_tracer — Time Model    Backends: %d    Interval: %ds\n",
           count_active_backends(d), d->interval);
    printf("%s\n\n", LINE);

    if (d->num_windows > 0 && d->ring.slots) {
        print_time_model_multi(d);
        return;
    }

    uint64_t db = tm->db_time_ns;
    if (db == 0) {
        printf("  (no data yet — waiting for events)\n\n");
        return;
    }

    printf("  %-36s %12s %10s\n", "Stat Name", "Time (ms)", "% DB Time");
    printf("  %-36s %12s %10s\n", DASH + 44, DASH + 68, DASH + 70);

    printf("  %-36s %12.1f %9.1f%%\n", "DB Time", ns_to_ms(db), 100.0);
    printf("    %-34s %12.1f %9.1f%%\n", "CPU*", ns_to_ms(tm->cpu_time_ns),
           db ? 100.0 * tm->cpu_time_ns / db : 0);

    /* Wait classes, sorted by size */
    struct { const char *name; uint64_t ns; uint32_t class_id; } classes[] = {
        {"IO",        tm->io_time_ns,        PG_WAIT_IO},
        {"LWLock",    tm->lwlock_time_ns,    PG_WAIT_LWLOCK},
        {"Lock",      tm->lock_time_ns,      PG_WAIT_LOCK},
        {"Client",    tm->client_time_ns,    PG_WAIT_CLIENT},
        {"IPC",       tm->ipc_time_ns,       PG_WAIT_IPC},
        {"BufferPin", tm->bufferpin_time_ns, PG_WAIT_BUFFERPIN},
        {"Timeout",   tm->timeout_time_ns,   PG_WAIT_TIMEOUT},
        {"Extension", tm->extension_time_ns, PG_WAIT_EXTENSION},
    };
    int nclasses = sizeof(classes) / sizeof(classes[0]);

    /* Sort by ns descending */
    for (int i = 0; i < nclasses - 1; i++)
        for (int j = i + 1; j < nclasses; j++)
            if (classes[j].ns > classes[i].ns) {
                typeof(classes[0]) tmp = classes[i];
                classes[i] = classes[j];
                classes[j] = tmp;
            }

#define MAX_SUB_EVENTS 3
#define MIN_DB_PCT     1.0

    for (int i = 0; i < nclasses; i++) {
        if (classes[i].ns == 0) continue;
        printf("    %-34s %12.1f %9.1f%%\n", classes[i].name,
               ns_to_ms(classes[i].ns), 100.0 * classes[i].ns / db);

        /* Find top sub-events for this class */
        struct { uint32_t we; uint64_t total_ns; } top[MAX_SUB_EVENTS];
        int ntop = 0;

        for (int j = 0; j < acc->num_system_events; j++) {
            struct pgwt_event_stats *ev = &acc->system_events[j];
            if (ev->count == 0) continue;
            if (WE_CLASS(ev->wait_event) != classes[i].class_id) continue;
            if (100.0 * ev->total_ns / db < MIN_DB_PCT) continue;

            /* Insert into top[], sorted desc by total_ns, cap at MAX_SUB_EVENTS */
            int pos = ntop;
            for (int k = 0; k < ntop; k++) {
                if (ev->total_ns > top[k].total_ns) {
                    pos = k;
                    break;
                }
            }
            if (pos < MAX_SUB_EVENTS) {
                int tail = ntop < MAX_SUB_EVENTS ? ntop : MAX_SUB_EVENTS - 1;
                for (int k = tail; k > pos; k--)
                    top[k] = top[k - 1];
                top[pos].we = ev->wait_event;
                top[pos].total_ns = ev->total_ns;
                if (ntop < MAX_SUB_EVENTS)
                    ntop++;
            }
        }

        for (int t = 0; t < ntop; t++) {
            char name[64];
            pgwt_event_full_name(top[t].we, name, sizeof(name));
            printf("      %-32s %12.1f %9.1f%%\n", name,
                   ns_to_ms(top[t].total_ns), 100.0 * top[t].total_ns / db);
        }
    }

#undef MAX_SUB_EVENTS
#undef MIN_DB_PCT

    if (tm->activity_time_ns > 0) {
        printf("\n  %-36s %12.1f %10s\n", "(Activity/Idle — excluded from DB Time)",
               ns_to_ms(tm->activity_time_ns), "—");
    }

    printf("\n");
}

/* ── System Event View ───────────────────────────────────── */

/* qsort comparator: sort events by total_ns descending */
static int cmp_event_total(const void *a, const void *b)
{
    const struct pgwt_event_stats *ea = a;
    const struct pgwt_event_stats *eb = b;
    if (eb->total_ns > ea->total_ns) return 1;
    if (eb->total_ns < ea->total_ns) return -1;
    return 0;
}

/* qsort comparator for snapshot events (used in multi-window mode) */
static int cmp_snap_event_total(const void *a, const void *b)
{
    const struct pgwt_snap_event *ea = a;
    const struct pgwt_snap_event *eb = b;
    if (eb->total_ns > ea->total_ns) return 1;
    if (eb->total_ns < ea->total_ns) return -1;
    return 0;
}

/* Multi-window system_event: vertically stacked sections per window */
static void print_system_event_multi(struct pgwt_daemon *d)
{
    int nw = d->num_windows;
    struct pgwt_snapshot deltas[PGWT_MAX_WINDOWS];
    int valid[PGWT_MAX_WINDOWS] = {0};

    for (int w = 0; w < nw; w++) {
        int ticks = d->windows[w] / d->interval;
        valid[w] = (pgwt_ring_delta(&d->ring, ticks, &deltas[w]) == 0);
    }

    for (int w = 0; w < nw; w++) {
        /* Section header: ---- Last 5s ---- */
        char label[16];
        format_window_label(d->windows[w], label, sizeof(label));
        printf("---- %s ", label);
        int used = 5 + (int)strlen(label) + 1;
        for (int i = used; i < 78; i++) putchar('-');
        printf("\n");

        if (!valid[w]) {
            printf("  (waiting for data)\n\n");
            continue;
        }

        /* Sort delta events */
        qsort(deltas[w].events, deltas[w].num_events,
              sizeof(deltas[w].events[0]), cmp_snap_event_total);

        uint64_t db = deltas[w].tm.db_time_ns;

        /* Column headers (no Max column — not available in deltas) */
        printf("  %-26s %12s %14s %10s %9s\n",
               "Wait Event", "Total Waits", "Total (ms)", "Avg (us)", "% DB");
        printf("  ");
        for (int i = 0; i < 26; i++) putchar('-');
        printf(" ");
        for (int i = 0; i < 12; i++) putchar('-');
        printf(" ");
        for (int i = 0; i < 14; i++) putchar('-');
        printf(" ");
        for (int i = 0; i < 10; i++) putchar('-');
        printf(" ");
        for (int i = 0; i < 9; i++) putchar('-');
        printf("\n");

        int shown = 0;
        for (int i = 0; i < deltas[w].num_events && shown < 20; i++) {
            struct pgwt_snap_event *ev = &deltas[w].events[i];
            if (ev->count == 0) continue;
            if (pgwt_is_idle_event(ev->wait_event)) continue;

            char name[64];
            pgwt_event_full_name(ev->wait_event, name, sizeof(name));

            double avg_us = ev->count ?
                ns_to_us(ev->total_ns) / ev->count : 0;

            printf("  %-26s %12lu %14.1f %10.1f %8.1f%%\n",
                   name,
                   (unsigned long)ev->count,
                   ns_to_ms(ev->total_ns),
                   avg_us,
                   db ? 100.0 * ev->total_ns / db : 0);
            shown++;
        }
        printf("\n");
    }
}

void pgwt_print_system_event(struct pgwt_daemon *d)
{
    struct pgwt_accumulator *acc = &d->accum;

    print_view_start(d);
    printf("%s\n", LINE);
    printf("pg_wait_tracer — System Events    Backends: %d    Interval: %ds\n",
           count_active_backends(d), d->interval);
    printf("%s\n\n", LINE);

    if (d->num_windows > 0 && d->ring.slots) {
        print_system_event_multi(d);
        return;
    }

    if (acc->num_system_events == 0) {
        printf("  (no data yet)\n\n");
        return;
    }

    /* Copy and sort */
    struct pgwt_event_stats sorted[4096];
    int n = acc->num_system_events;
    memcpy(sorted, acc->system_events, n * sizeof(sorted[0]));
    qsort(sorted, n, sizeof(sorted[0]), cmp_event_total);

    uint64_t db = acc->tm.db_time_ns;

    printf("  %-26s %12s %14s %10s %12s %9s\n",
           "Wait Event", "Total Waits", "Total (ms)", "Avg (us)", "Max (us)", "% DB");
    printf("  %-26s %12s %14s %10s %12s %9s\n",
           DASH + 54, DASH + 68, DASH + 66, DASH + 70, DASH + 68, DASH + 71);

    int shown = 0;
    for (int i = 0; i < n && shown < 20; i++) {
        if (sorted[i].count == 0) continue;
        if (pgwt_is_idle_event(sorted[i].wait_event)) continue;

        char name[64];
        pgwt_event_full_name(sorted[i].wait_event, name, sizeof(name));

        double avg_us = sorted[i].count ?
            ns_to_us(sorted[i].total_ns) / sorted[i].count : 0;

        printf("  %-26s %12lu %14.1f %10.1f %12.1f %8.1f%%\n",
               name,
               (unsigned long)sorted[i].count,
               ns_to_ms(sorted[i].total_ns),
               avg_us,
               ns_to_us(sorted[i].max_ns),
               db ? 100.0 * sorted[i].total_ns / db : 0);
        shown++;
    }
    printf("\n");
}

/* ── Session Event View ──────────────────────────────────── */

void pgwt_print_session_event(struct pgwt_daemon *d)
{
    struct pgwt_accumulator *acc = &d->accum;

    print_view_start(d);
    printf("%s\n", LINE);
    printf("pg_wait_tracer — Session Summary    Backends: %d\n",
           count_active_backends(d));
    printf("%s\n\n", LINE);

    printf("  %-7s %-14s %-10s %-10s %12s %6s %6s  %-20s\n",
           "PID", "Type", "User", "DB", "DB Time(ms)", "CPU%", "Wait%", "Top Wait");
    printf("  %-7s %-14s %-10s %-10s %12s %6s %6s  %-20s\n",
           DASH + 73, DASH + 66, DASH + 70, DASH + 70, DASH + 68, DASH + 74, DASH + 74, DASH + 60);

    for (int i = 0; i < d->backends.count; i++) {
        struct pgwt_backend *be = &d->backends.entries[i];
        if (!be->is_alive || be->pid == 0) continue;

        struct pgwt_pid_accum *pa = pgwt_find_pid_accum(acc, be->pid);
        if (!pa || pa->db_time_ns == 0) {
            printf("  %-7d %-14s %-10s %-10s %12s %6s %6s  %-20s\n",
                   be->pid,
                   pgwt_backend_type_name(be->meta.backend_type),
                   be->meta.usename[0] ? be->meta.usename : "-",
                   be->meta.datname[0] ? be->meta.datname : "-",
                   "0.0", "-", "-", "-");
            continue;
        }

        double cpu_pct = pa->db_time_ns ?
            100.0 * pa->cpu_time_ns / pa->db_time_ns : 0;
        double wait_pct = pa->db_time_ns ?
            100.0 * pa->wait_time_ns / pa->db_time_ns : 0;

        /* Find top wait event (by total_ns, excluding CPU) */
        const char *top_wait = "-";
        uint64_t top_ns = 0;
        char top_name[64] = {};
        for (int j = 0; j < pa->num_events; j++) {
            if (pa->events[j].wait_event == 0) continue;
            if (pgwt_is_idle_event(pa->events[j].wait_event)) continue;
            if (pa->events[j].total_ns > top_ns) {
                top_ns = pa->events[j].total_ns;
                pgwt_event_full_name(pa->events[j].wait_event,
                                     top_name, sizeof(top_name));
                top_wait = top_name;
            }
        }

        printf("  %-7d %-14s %-10s %-10s %12.1f %5.1f%% %5.1f%%  %-20s\n",
               be->pid,
               pgwt_backend_type_name(be->meta.backend_type),
               be->meta.usename[0] ? be->meta.usename : "-",
               be->meta.datname[0] ? be->meta.datname : "-",
               ns_to_ms(pa->db_time_ns),
               cpu_pct, wait_pct, top_wait);
    }
    printf("\n");

    /* If --pid-filter is set, show detailed breakdown for that PID */
    if (d->pid_filter > 0) {
        struct pgwt_pid_accum *pa = pgwt_find_pid_accum(acc, d->pid_filter);
        if (!pa) return;

        printf("  Detail for PID %d:\n\n", d->pid_filter);
        printf("  %-26s %12s %14s %10s %12s %9s\n",
               "Wait Event", "Total Waits", "Total (ms)", "Avg (us)", "Max (us)", "% DB");
        printf("  %-26s %12s %14s %10s %12s %9s\n",
               DASH + 54, DASH + 68, DASH + 66, DASH + 70, DASH + 68, DASH + 71);

        /* Sort events by total_ns desc */
        struct pgwt_event_stats sorted[MAX_EVENTS_PER_PID];
        memcpy(sorted, pa->events, pa->num_events * sizeof(sorted[0]));
        qsort(sorted, pa->num_events, sizeof(sorted[0]), cmp_event_total);

        for (int j = 0; j < pa->num_events; j++) {
            if (sorted[j].count == 0) continue;
            char name[64];
            pgwt_event_full_name(sorted[j].wait_event, name, sizeof(name));
            double avg_us = sorted[j].count ?
                ns_to_us(sorted[j].total_ns) / sorted[j].count : 0;
            printf("  %-26s %12lu %14.1f %10.1f %12.1f %8.1f%%\n",
                   name,
                   (unsigned long)sorted[j].count,
                   ns_to_ms(sorted[j].total_ns),
                   avg_us,
                   ns_to_us(sorted[j].max_ns),
                   pa->db_time_ns ? 100.0 * sorted[j].total_ns / pa->db_time_ns : 0);
        }
        printf("\n");
    }
}

/* ── Histogram View ──────────────────────────────────────── */

static const char *bucket_labels[] = {
    "     <1",  "  1-  2",  "  2-  4",  "  4-  8",
    "  8- 16",  " 16- 32",  " 32- 64",  " 64-128",
    "128-256",  "256-512",  "512-1K ",  " 1K- 2K",
    " 2K- 4K",  " 4K- 8K",  " 8K-16K",  ">=16K  ",
};

void pgwt_print_histogram(struct pgwt_daemon *d)
{
    print_view_start(d);
    printf("%s\n", LINE);
    printf("pg_wait_tracer — Event Histogram\n");
    printf("%s\n\n", LINE);

    if (!d->event_filter || !d->event_filter[0]) {
        printf("  Use --event \"IO:DataFileRead\" to select an event.\n\n");
        return;
    }

    /* Find the matching system event */
    struct pgwt_event_stats *target = NULL;
    for (int i = 0; i < d->accum.num_system_events; i++) {
        char name[64];
        pgwt_event_full_name(d->accum.system_events[i].wait_event, name, sizeof(name));
        if (strcmp(name, d->event_filter) == 0) {
            target = &d->accum.system_events[i];
            break;
        }
    }

    if (!target) {
        printf("  Event '%s' not found (no data yet?).\n\n", d->event_filter);
        return;
    }

    printf("  Event: %s | Total Waits: %lu | Total: %.1f ms\n\n",
           d->event_filter, (unsigned long)target->count,
           ns_to_ms(target->total_ns));

    printf("  %-10s %12s %10s %12s  %s\n",
           "Bucket(us)", "Waits", "% Waits", "Cumulative", "");
    printf("  %-10s %12s %10s %12s\n",
           DASH + 70, DASH + 68, DASH + 70, DASH + 68);

    uint64_t cum = 0;
    for (int b = 0; b < HISTOGRAM_BUCKETS; b++) {
        uint64_t w = target->histogram[b];
        cum += w;
        double pct = target->count ? 100.0 * w / target->count : 0;
        double cum_pct = target->count ? 100.0 * cum / target->count : 0;

        /* ASCII bar: each char = ~2% */
        int bar_len = (int)(pct / 2);
        if (bar_len > 40) bar_len = 40;
        char bar[41];
        memset(bar, '#', bar_len);
        bar[bar_len] = '\0';

        printf("  %s %12lu %9.1f%% %11.1f%%  %s\n",
               bucket_labels[b], (unsigned long)w, pct, cum_pct, bar);
    }
    printf("\n");
}

/* ── Query Event View ────────────────────────────────────── */

/* qsort comparator: sort query events by total_ns descending */
static int cmp_query_event_total(const void *a, const void *b)
{
    const struct pgwt_query_event_stats *qa = a;
    const struct pgwt_query_event_stats *qb = b;
    if (qb->total_ns > qa->total_ns) return 1;
    if (qb->total_ns < qa->total_ns) return -1;
    return 0;
}

/* qsort comparator for snapshot query events (multi-window mode) */
static int cmp_snap_query_event_total(const void *a, const void *b)
{
    const struct pgwt_snap_query_event *ea = a;
    const struct pgwt_snap_query_event *eb = b;
    if (eb->total_ns > ea->total_ns) return 1;
    if (eb->total_ns < ea->total_ns) return -1;
    return 0;
}

/* Multi-window query_event: vertically stacked sections per window */
static void print_query_event_multi(struct pgwt_daemon *d)
{
    int nw = d->num_windows;
    struct pgwt_snapshot deltas[PGWT_MAX_WINDOWS];
    int valid[PGWT_MAX_WINDOWS] = {0};

    int mode_b = (d->event_filter && d->event_filter[0]);
    int mode_c = (d->query_id_filter != 0);

    for (int w = 0; w < nw; w++) {
        int ticks = d->windows[w] / d->interval;
        valid[w] = (pgwt_ring_delta(&d->ring, ticks, &deltas[w]) == 0);
    }

    for (int w = 0; w < nw; w++) {
        char label[16];
        format_window_label(d->windows[w], label, sizeof(label));
        printf("---- %s ", label);
        int used = 5 + (int)strlen(label) + 1;
        for (int i = used; i < 78; i++) putchar('-');
        printf("\n");

        if (!valid[w]) {
            printf("  (waiting for data)\n\n");
            continue;
        }

        /* Sort delta query events */
        qsort(deltas[w].query_events, deltas[w].num_query_events,
              sizeof(deltas[w].query_events[0]), cmp_snap_query_event_total);

        uint64_t db = deltas[w].tm.db_time_ns;

        /* Compute denominator for % Event / % Query */
        uint64_t denom_ns = 0;
        if (mode_b) {
            for (int i = 0; i < deltas[w].num_query_events; i++) {
                char name[64];
                pgwt_event_full_name(deltas[w].query_events[i].wait_event,
                                     name, sizeof(name));
                if (strcmp(name, d->event_filter) == 0)
                    denom_ns += deltas[w].query_events[i].total_ns;
            }
        } else if (mode_c) {
            for (int i = 0; i < deltas[w].num_query_events; i++)
                if (deltas[w].query_events[i].query_id == d->query_id_filter)
                    denom_ns += deltas[w].query_events[i].total_ns;
        }

        /* Column headers */
        if (mode_c) {
            printf("  %-26s %12s %14s %10s %9s %9s\n",
                   "Wait Event", "Waits", "Total (ms)",
                   "Avg (us)", "% Query", "% DB");
            printf("  ");
            for (int i = 0; i < 26; i++) putchar('-');
            printf(" ");
            for (int i = 0; i < 12; i++) putchar('-');
            printf(" ");
            for (int i = 0; i < 14; i++) putchar('-');
            printf(" ");
            for (int i = 0; i < 10; i++) putchar('-');
            printf(" ");
            for (int i = 0; i < 9; i++) putchar('-');
            printf(" ");
            for (int i = 0; i < 9; i++) putchar('-');
            printf("\n");
        } else if (mode_b) {
            printf("  %-20s %12s %14s %10s %9s %9s\n",
                   "query_id", "Waits", "Total (ms)",
                   "Avg (us)", "% Event", "% DB");
            printf("  ");
            for (int i = 0; i < 20; i++) putchar('-');
            printf(" ");
            for (int i = 0; i < 12; i++) putchar('-');
            printf(" ");
            for (int i = 0; i < 14; i++) putchar('-');
            printf(" ");
            for (int i = 0; i < 10; i++) putchar('-');
            printf(" ");
            for (int i = 0; i < 9; i++) putchar('-');
            printf(" ");
            for (int i = 0; i < 9; i++) putchar('-');
            printf("\n");
        } else {
            printf("  %-20s %-26s %12s %14s %10s %9s\n",
                   "query_id", "Wait Event", "Waits", "Total (ms)",
                   "Avg (us)", "% DB");
            printf("  ");
            for (int i = 0; i < 20; i++) putchar('-');
            printf(" ");
            for (int i = 0; i < 26; i++) putchar('-');
            printf(" ");
            for (int i = 0; i < 12; i++) putchar('-');
            printf(" ");
            for (int i = 0; i < 14; i++) putchar('-');
            printf(" ");
            for (int i = 0; i < 10; i++) putchar('-');
            printf(" ");
            for (int i = 0; i < 9; i++) putchar('-');
            printf("\n");
        }

        int shown = 0;
        for (int i = 0; i < deltas[w].num_query_events && shown < 30; i++) {
            struct pgwt_snap_query_event *qe = &deltas[w].query_events[i];
            if (qe->count == 0) continue;
            if (pgwt_is_idle_event(qe->wait_event)) continue;

            char name[64];
            pgwt_event_full_name(qe->wait_event, name, sizeof(name));

            if (mode_b && strcmp(name, d->event_filter) != 0) continue;
            if (mode_c && qe->query_id != d->query_id_filter) continue;

            double avg_us = qe->count ?
                ns_to_us(qe->total_ns) / qe->count : 0;

            if (mode_c) {
                printf("  %-26s %12lu %14.1f %10.1f %8.1f%% %8.1f%%\n",
                       name,
                       (unsigned long)qe->count,
                       ns_to_ms(qe->total_ns),
                       avg_us,
                       denom_ns ? 100.0 * qe->total_ns / denom_ns : 0,
                       db ? 100.0 * qe->total_ns / db : 0);
            } else if (mode_b) {
                printf("  %20ld %12lu %14.1f %10.1f %8.1f%% %8.1f%%\n",
                       (int64_t)qe->query_id,
                       (unsigned long)qe->count,
                       ns_to_ms(qe->total_ns),
                       avg_us,
                       denom_ns ? 100.0 * qe->total_ns / denom_ns : 0,
                       db ? 100.0 * qe->total_ns / db : 0);
            } else {
                printf("  %20ld %-26s %12lu %14.1f %10.1f %8.1f%%\n",
                       (int64_t)qe->query_id,
                       name,
                       (unsigned long)qe->count,
                       ns_to_ms(qe->total_ns),
                       avg_us,
                       db ? 100.0 * qe->total_ns / db : 0);
            }
            shown++;
        }
        printf("\n");
    }
}

void pgwt_print_query_event(struct pgwt_daemon *d)
{
    struct pgwt_accumulator *acc = &d->accum;

    int mode_b = (d->event_filter && d->event_filter[0]);
    int mode_c = (d->query_id_filter != 0);

    print_view_start(d);
    printf("%s\n", LINE);
    if (mode_b)
        printf("pg_wait_tracer — Top Queries for %s    Backends: %d    Interval: %ds\n",
               d->event_filter, count_active_backends(d), d->interval);
    else if (mode_c)
        printf("pg_wait_tracer — Wait Profile for query_id %ld    Backends: %d    Interval: %ds\n",
               (int64_t)d->query_id_filter, count_active_backends(d), d->interval);
    else
        printf("pg_wait_tracer — Query Events    Backends: %d    Interval: %ds\n",
               count_active_backends(d), d->interval);
    printf("%s\n\n", LINE);

    if (d->num_windows > 0 && d->ring.slots) {
        print_query_event_multi(d);
        return;
    }

    if (acc->num_query_events == 0) {
        printf("  (no query data yet — ensure compute_query_id = on/auto)\n\n");
        return;
    }

    /* Copy and sort */
    struct pgwt_query_event_stats sorted[MAX_QUERY_EVENTS];
    int n = acc->num_query_events;
    if (n > MAX_QUERY_EVENTS) n = MAX_QUERY_EVENTS;
    memcpy(sorted, acc->query_events, n * sizeof(sorted[0]));
    qsort(sorted, n, sizeof(sorted[0]), cmp_query_event_total);

    uint64_t db = acc->tm.db_time_ns;

    /* Compute denominator for % Event (Mode B) or % Query (Mode C) */
    uint64_t denom_ns = 0;
    if (mode_b) {
        for (int i = 0; i < n; i++) {
            char name[64];
            pgwt_event_full_name(sorted[i].wait_event, name, sizeof(name));
            if (strcmp(name, d->event_filter) == 0)
                denom_ns += sorted[i].total_ns;
        }
    } else if (mode_c) {
        for (int i = 0; i < n; i++)
            if (sorted[i].query_id == d->query_id_filter)
                denom_ns += sorted[i].total_ns;
    }

    /* Column headers */
    if (mode_c) {
        printf("  %-26s %12s %14s %10s %12s %9s %9s\n",
               "Wait Event", "Waits", "Total (ms)",
               "Avg (us)", "Max (us)", "% Query", "% DB");
        printf("  %-26s %12s %14s %10s %12s %9s %9s\n",
               DASH + 54, DASH + 68, DASH + 66,
               DASH + 70, DASH + 68, DASH + 71, DASH + 71);
    } else if (mode_b) {
        printf("  %-20s %12s %14s %10s %12s %9s %9s\n",
               "query_id", "Waits", "Total (ms)",
               "Avg (us)", "Max (us)", "% Event", "% DB");
        printf("  %-20s %12s %14s %10s %12s %9s %9s\n",
               DASH + 60, DASH + 68, DASH + 66,
               DASH + 70, DASH + 68, DASH + 71, DASH + 71);
    } else {
        printf("  %-20s %-26s %12s %14s %10s %12s %9s\n",
               "query_id", "Wait Event", "Total Waits", "Total (ms)",
               "Avg (us)", "Max (us)", "% DB");
        printf("  %-20s %-26s %12s %14s %10s %12s %9s\n",
               DASH + 60, DASH + 54, DASH + 68, DASH + 66,
               DASH + 70, DASH + 68, DASH + 71);
    }

    int shown = 0;
    for (int i = 0; i < n && shown < 30; i++) {
        if (sorted[i].count == 0) continue;
        if (pgwt_is_idle_event(sorted[i].wait_event)) continue;

        char name[64];
        pgwt_event_full_name(sorted[i].wait_event, name, sizeof(name));

        /* Apply mode filter */
        if (mode_b && strcmp(name, d->event_filter) != 0) continue;
        if (mode_c && sorted[i].query_id != d->query_id_filter) continue;

        double avg_us = sorted[i].count ?
            ns_to_us(sorted[i].total_ns) / sorted[i].count : 0;

        if (mode_c) {
            printf("  %-26s %12lu %14.1f %10.1f %12.1f %8.1f%% %8.1f%%\n",
                   name,
                   (unsigned long)sorted[i].count,
                   ns_to_ms(sorted[i].total_ns),
                   avg_us,
                   ns_to_us(sorted[i].max_ns),
                   denom_ns ? 100.0 * sorted[i].total_ns / denom_ns : 0,
                   db ? 100.0 * sorted[i].total_ns / db : 0);
        } else if (mode_b) {
            printf("  %20ld %12lu %14.1f %10.1f %12.1f %8.1f%% %8.1f%%\n",
                   (int64_t)sorted[i].query_id,
                   (unsigned long)sorted[i].count,
                   ns_to_ms(sorted[i].total_ns),
                   avg_us,
                   ns_to_us(sorted[i].max_ns),
                   denom_ns ? 100.0 * sorted[i].total_ns / denom_ns : 0,
                   db ? 100.0 * sorted[i].total_ns / db : 0);
        } else {
            printf("  %20ld %-26s %12lu %14.1f %10.1f %12.1f %8.1f%%\n",
                   (int64_t)sorted[i].query_id,
                   name,
                   (unsigned long)sorted[i].count,
                   ns_to_ms(sorted[i].total_ns),
                   avg_us,
                   ns_to_us(sorted[i].max_ns),
                   db ? 100.0 * sorted[i].total_ns / db : 0);
        }
        shown++;
    }
    printf("\n");
}
