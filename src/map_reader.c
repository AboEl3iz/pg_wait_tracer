/* map_reader.c — Read BPF PERCPU_HASH maps, sum per-CPU, accumulate */
#include "map_reader.h"
#include "daemon.h"
#include "wait_event.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <time.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "pg_wait_tracer.skel.h"

/* Must match bpf_ktime_get_ns() clock source (CLOCK_MONOTONIC). */
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

void pgwt_accum_init(struct pgwt_accumulator *acc)
{
    memset(acc, 0, sizeof(*acc));
}

struct pgwt_pid_accum *pgwt_find_pid_accum(struct pgwt_accumulator *acc, uint32_t pid)
{
    for (int i = 0; i < acc->num_pids; i++) {
        if (acc->pids[i].pid == pid && acc->pids[i].active)
            return &acc->pids[i];
    }
    return NULL;
}

static struct pgwt_pid_accum *get_or_create_pid(struct pgwt_accumulator *acc, uint32_t pid)
{
    struct pgwt_pid_accum *pa = pgwt_find_pid_accum(acc, pid);
    if (pa) return pa;

    if (acc->num_pids >= MAX_BACKENDS)
        return NULL;
    pa = &acc->pids[acc->num_pids++];
    memset(pa, 0, sizeof(*pa));
    pa->pid = pid;
    pa->active = true;
    return pa;
}

static struct pgwt_event_stats *get_or_create_event(struct pgwt_pid_accum *pa, uint32_t we)
{
    for (int i = 0; i < pa->num_events; i++) {
        if (pa->events[i].wait_event == we)
            return &pa->events[i];
    }
    if (pa->num_events >= MAX_EVENTS_PER_PID)
        return NULL;
    struct pgwt_event_stats *es = &pa->events[pa->num_events++];
    memset(es, 0, sizeof(*es));
    es->wait_event = we;
    es->min_ns = UINT64_MAX;
    return es;
}

struct pgwt_event_stats *pgwt_find_system_event(struct pgwt_accumulator *acc,
                                                 uint32_t wait_event)
{
    for (int i = 0; i < acc->num_system_events; i++) {
        if (acc->system_events[i].wait_event == wait_event)
            return &acc->system_events[i];
    }
    return NULL;
}

static struct pgwt_event_stats *get_or_create_system_event(struct pgwt_accumulator *acc,
                                                            uint32_t we)
{
    struct pgwt_event_stats *se = pgwt_find_system_event(acc, we);
    if (se) return se;

    if (acc->num_system_events >= 4096)
        return NULL;
    se = &acc->system_events[acc->num_system_events++];
    memset(se, 0, sizeof(*se));
    se->wait_event = we;
    se->min_ns = UINT64_MAX;
    return se;
}

static struct pgwt_query_event_stats *get_or_create_query_event(
    struct pgwt_accumulator *acc, uint64_t query_id, uint32_t we)
{
    for (int i = 0; i < acc->num_query_events; i++) {
        if (acc->query_events[i].query_id == query_id &&
            acc->query_events[i].wait_event == we)
            return &acc->query_events[i];
    }
    if (acc->num_query_events >= MAX_QUERY_EVENTS)
        return NULL;
    struct pgwt_query_event_stats *qe = &acc->query_events[acc->num_query_events++];
    memset(qe, 0, sizeof(*qe));
    qe->query_id = query_id;
    qe->wait_event = we;
    qe->min_ns = UINT64_MAX;
    return qe;
}

int pgwt_read_maps(struct pgwt_daemon *d)
{
    int stats_fd = bpf_map__fd(d->skel->maps.wait_stats);
    int nr_cpus = libbpf_num_possible_cpus();
    if (nr_cpus < 0) return -1;

    /* Save previous time model for delta computation */
    d->accum.prev_tm = d->accum.tm;

    /* Reset accumulators (rebuild from BPF maps each tick) */
    for (int i = 0; i < d->accum.num_pids; i++) {
        d->accum.pids[i].num_events = 0;
        d->accum.pids[i].db_time_ns = 0;
        d->accum.pids[i].cpu_time_ns = 0;
        d->accum.pids[i].wait_time_ns = 0;
        d->accum.pids[i].current_event = 0;
        d->accum.pids[i].current_wait_ns = 0;
    }
    d->accum.num_system_events = 0;
    d->accum.num_query_events = 0;
    memset(&d->accum.tm, 0, sizeof(d->accum.tm));

    /* Iterate all entries in wait_stats PERCPU_HASH */
    struct pgwt_agg_key key = {}, next_key;
    struct pgwt_agg_value values[MAX_CPUS];

    while (bpf_map_get_next_key(stats_fd, &key, &next_key) == 0) {
        if (bpf_map_lookup_elem(stats_fd, &next_key, values) != 0) {
            key = next_key;
            continue;
        }

        /* Sum across all CPUs */
        uint64_t count = 0, total = 0, min_v = UINT64_MAX, max_v = 0;
        uint64_t hist[HISTOGRAM_BUCKETS] = {};

        for (int cpu = 0; cpu < nr_cpus && cpu < MAX_CPUS; cpu++) {
            count += values[cpu].count;
            total += values[cpu].total_ns;
            if (values[cpu].count > 0 && values[cpu].min_ns < min_v)
                min_v = values[cpu].min_ns;
            if (values[cpu].max_ns > max_v)
                max_v = values[cpu].max_ns;
            for (int b = 0; b < HISTOGRAM_BUCKETS; b++)
                hist[b] += values[cpu].histogram[b];
        }
        if (min_v == UINT64_MAX) min_v = 0;
        if (count == 0) { key = next_key; continue; }

        uint32_t pid = next_key.pid;
        uint32_t we  = next_key.wait_event;

        /* Per-PID accumulation */
        struct pgwt_pid_accum *pa = get_or_create_pid(&d->accum, pid);
        if (pa) {
            struct pgwt_event_stats *es = get_or_create_event(pa, we);
            if (es) {
                es->count = count;
                es->total_ns = total;
                es->min_ns = min_v;
                es->max_ns = max_v;
                memcpy(es->histogram, hist, sizeof(hist));
            }

            if (we == 0) {
                pa->cpu_time_ns += total;
            } else if (!pgwt_is_idle_event(we)) {
                pa->wait_time_ns += total;
            }
            if (!pgwt_is_idle_event(we))
                pa->db_time_ns += total;
        }

        /* System-wide accumulation */
        struct pgwt_event_stats *se = get_or_create_system_event(&d->accum, we);
        if (se) {
            se->count += count;
            se->total_ns += total;
            if (min_v < se->min_ns) se->min_ns = min_v;
            if (max_v > se->max_ns) se->max_ns = max_v;
            for (int b = 0; b < HISTOGRAM_BUCKETS; b++)
                se->histogram[b] += hist[b];
        }

        /* Time model by class */
        struct pgwt_time_model *tm = &d->accum.tm;
        if (we == 0) {
            tm->cpu_time_ns += total;
        } else {
            int cls = WE_CLASS(we);
            switch (cls) {
            case PG_WAIT_IO:        tm->io_time_ns += total; break;
            case PG_WAIT_LWLOCK:    tm->lwlock_time_ns += total; break;
            case PG_WAIT_LOCK:      tm->lock_time_ns += total; break;
            case PG_WAIT_BUFFERPIN: tm->bufferpin_time_ns += total; break;
            case PG_WAIT_CLIENT:    tm->client_time_ns += total; break;
            case PG_WAIT_IPC:       tm->ipc_time_ns += total; break;
            case PG_WAIT_TIMEOUT:   tm->timeout_time_ns += total; break;
            case PG_WAIT_EXTENSION: tm->extension_time_ns += total; break;
            case PG_WAIT_ACTIVITY:  tm->activity_time_ns += total; break;
            }
        }
        if (!pgwt_is_idle_event(we))
            tm->db_time_ns += total;

        key = next_key;
    }

    /* Account for open intervals: backends currently sitting in a wait
     * state have accumulated no data via on_watchpoint if there were no
     * state transitions. Read state_map to capture ongoing waits.
     *
     * Only add open intervals for (PID, event) combinations that have
     * no data in wait_stats yet. This avoids double-counting for
     * backends that ARE producing watchpoint fires (their accumulated
     * closed intervals in wait_stats are already accurate). */
    int state_fd = bpf_map__fd(d->skel->maps.state_map);
    uint32_t skey = 0, snext;
    struct pgwt_pid_state sval;
    uint64_t now = now_ns();

    while (bpf_map_get_next_key(state_fd, &skey, &snext) == 0) {
        if (bpf_map_lookup_elem(state_fd, &snext, &sval) == 0) {
            uint64_t open_ns = now - sval.last_ts;
            uint32_t we = sval.last_event;

            /* Store current state for active sessions view */
            {
                struct pgwt_pid_accum *pa_cur = get_or_create_pid(&d->accum, snext);
                if (pa_cur) {
                    pa_cur->current_event = we;
                    pa_cur->current_wait_ns = open_ns;
                }
            }

            /* Skip if wait_stats already has data for this (PID, event) */
            struct pgwt_agg_key check_key = { .pid = snext, .wait_event = we };
            struct pgwt_agg_value check_vals[MAX_CPUS];
            bool has_closed_data = false;
            if (bpf_map_lookup_elem(stats_fd, &check_key, check_vals) == 0) {
                for (int cpu = 0; cpu < nr_cpus && cpu < MAX_CPUS; cpu++) {
                    if (check_vals[cpu].count > 0) {
                        has_closed_data = true;
                        break;
                    }
                }
            }

            if (open_ns > 0 && !has_closed_data) {
                /* Per-PID accumulation */
                struct pgwt_pid_accum *pa = get_or_create_pid(&d->accum, snext);
                if (pa) {
                    struct pgwt_event_stats *es = get_or_create_event(pa, we);
                    if (es) {
                        es->count += 1;
                        es->total_ns += open_ns;
                        if (open_ns < es->min_ns) es->min_ns = open_ns;
                        if (open_ns > es->max_ns) es->max_ns = open_ns;
                    }
                    if (we == 0) {
                        pa->cpu_time_ns += open_ns;
                    } else if (!pgwt_is_idle_event(we)) {
                        pa->wait_time_ns += open_ns;
                    }
                    if (!pgwt_is_idle_event(we))
                        pa->db_time_ns += open_ns;
                }

                /* System-wide accumulation */
                struct pgwt_event_stats *se = get_or_create_system_event(&d->accum, we);
                if (se) {
                    se->count += 1;
                    se->total_ns += open_ns;
                    if (open_ns < se->min_ns) se->min_ns = open_ns;
                    if (open_ns > se->max_ns) se->max_ns = open_ns;
                }

                /* Time model by class */
                struct pgwt_time_model *tm = &d->accum.tm;
                if (we == 0) {
                    tm->cpu_time_ns += open_ns;
                } else {
                    int cls = WE_CLASS(we);
                    switch (cls) {
                    case PG_WAIT_IO:        tm->io_time_ns += open_ns; break;
                    case PG_WAIT_LWLOCK:    tm->lwlock_time_ns += open_ns; break;
                    case PG_WAIT_LOCK:      tm->lock_time_ns += open_ns; break;
                    case PG_WAIT_BUFFERPIN: tm->bufferpin_time_ns += open_ns; break;
                    case PG_WAIT_CLIENT:    tm->client_time_ns += open_ns; break;
                    case PG_WAIT_IPC:       tm->ipc_time_ns += open_ns; break;
                    case PG_WAIT_TIMEOUT:   tm->timeout_time_ns += open_ns; break;
                    case PG_WAIT_EXTENSION: tm->extension_time_ns += open_ns; break;
                    case PG_WAIT_ACTIVITY:  tm->activity_time_ns += open_ns; break;
                    }
                }
                if (!pgwt_is_idle_event(we))
                    tm->db_time_ns += open_ns;

                /* Query-level open interval */
                if (sval.last_query_id != 0) {
                    struct pgwt_query_event_stats *qe =
                        get_or_create_query_event(&d->accum, sval.last_query_id, we);
                    if (qe) {
                        qe->count += 1;
                        qe->total_ns += open_ns;
                        if (open_ns < qe->min_ns) qe->min_ns = open_ns;
                        if (open_ns > qe->max_ns) qe->max_ns = open_ns;
                    }
                }
            }
        }
        skey = snext;
    }

    /* ── Read query_wait_stats PERCPU_HASH ────────────────────── */
    int qstats_fd = bpf_map__fd(d->skel->maps.query_wait_stats);
    struct pgwt_query_agg_key qkey = {}, qnext;
    struct pgwt_agg_value qvalues[MAX_CPUS];

    while (bpf_map_get_next_key(qstats_fd, &qkey, &qnext) == 0) {
        if (bpf_map_lookup_elem(qstats_fd, &qnext, qvalues) != 0) {
            qkey = qnext;
            continue;
        }

        /* Sum across all CPUs */
        uint64_t count = 0, total = 0, min_v = UINT64_MAX, max_v = 0;

        for (int cpu = 0; cpu < nr_cpus && cpu < MAX_CPUS; cpu++) {
            count += qvalues[cpu].count;
            total += qvalues[cpu].total_ns;
            if (qvalues[cpu].count > 0 && qvalues[cpu].min_ns < min_v)
                min_v = qvalues[cpu].min_ns;
            if (qvalues[cpu].max_ns > max_v)
                max_v = qvalues[cpu].max_ns;
        }
        if (min_v == UINT64_MAX) min_v = 0;
        if (count == 0) { qkey = qnext; continue; }

        struct pgwt_query_event_stats *qe =
            get_or_create_query_event(&d->accum, qnext.query_id, qnext.wait_event);
        if (qe) {
            qe->count += count;
            qe->total_ns += total;
            if (min_v < qe->min_ns) qe->min_ns = min_v;
            if (max_v > qe->max_ns) qe->max_ns = max_v;
        }

        qkey = qnext;
    }

    return 0;
}
