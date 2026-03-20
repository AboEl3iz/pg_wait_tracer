/* map_reader.c — Read BPF state_map for open intervals, accumulator helpers */
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

struct pgwt_pid_accum *pgwt_get_or_create_pid(struct pgwt_accumulator *acc, uint32_t pid)
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

struct pgwt_event_stats *pgwt_get_or_create_event(struct pgwt_pid_accum *pa, uint32_t we)
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

struct pgwt_event_stats *pgwt_get_or_create_system_event(struct pgwt_accumulator *acc,
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

struct pgwt_query_event_stats *pgwt_get_or_create_query_event(
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

void pgwt_update_time_model(struct pgwt_time_model *tm, uint32_t event,
                             uint64_t duration_ns)
{
    if (pgwt_is_idle_event(event)) {
        tm->activity_time_ns += duration_ns;
    } else if (event == 0) {
        tm->cpu_time_ns += duration_ns;
        tm->db_time_ns += duration_ns;
    } else {
        int cls = WE_CLASS(event);
        switch (cls) {
        case PG_WAIT_IO:        tm->io_time_ns += duration_ns; break;
        case PG_WAIT_LWLOCK:    tm->lwlock_time_ns += duration_ns; break;
        case PG_WAIT_LOCK:      tm->lock_time_ns += duration_ns; break;
        case PG_WAIT_BUFFERPIN: tm->bufferpin_time_ns += duration_ns; break;
        case PG_WAIT_CLIENT:    tm->client_time_ns += duration_ns; break;
        case PG_WAIT_IPC:       tm->ipc_time_ns += duration_ns; break;
        case PG_WAIT_TIMEOUT:   tm->timeout_time_ns += duration_ns; break;
        case PG_WAIT_EXTENSION: tm->extension_time_ns += duration_ns; break;
        case PG_WAIT_ACTIVITY:  tm->activity_time_ns += duration_ns; break;
        }
        tm->db_time_ns += duration_ns;
    }
}

uint32_t pgwt_duration_to_bucket(uint64_t ns)
{
    uint64_t us = ns / 1000;
    if (us < 1)     return 0;
    if (us < 2)     return 1;
    if (us < 4)     return 2;
    if (us < 8)     return 3;
    if (us < 16)    return 4;
    if (us < 32)    return 5;
    if (us < 64)    return 6;
    if (us < 128)   return 7;
    if (us < 256)   return 8;
    if (us < 512)   return 9;
    if (us < 1024)  return 10;
    if (us < 2048)  return 11;
    if (us < 4096)  return 12;
    if (us < 8192)  return 13;
    if (us < 16384) return 14;
    return 15;
}

void pgwt_read_state_map(struct pgwt_daemon *d)
{
    int state_fd = bpf_map__fd(d->skel->maps.state_map);
    uint32_t skey = 0, snext;
    struct pgwt_pid_state sval;
    uint64_t now = now_ns();

    while (bpf_map_get_next_key(state_fd, &skey, &snext) == 0) {
        if (bpf_map_lookup_elem(state_fd, &snext, &sval) == 0) {
            uint64_t open_ns = now - sval.last_ts;
            uint32_t we = sval.last_event;

            /* Store current state for active sessions view */
            struct pgwt_pid_accum *pa_cur = pgwt_get_or_create_pid(&d->accum, snext);
            if (pa_cur) {
                pa_cur->current_event = we;
                pa_cur->current_wait_ns = open_ns;
            }

            /* Check if d->accum already has closed-interval data for this
             * (PID, event). If so, skip the open interval to avoid double-counting. */
            bool has_closed_data = false;
            if (pa_cur) {
                for (int i = 0; i < pa_cur->num_events; i++) {
                    if (pa_cur->events[i].wait_event == we &&
                        pa_cur->events[i].count > 0) {
                        has_closed_data = true;
                        break;
                    }
                }
            }

            if (open_ns > 0 && !has_closed_data) {
                /* Per-PID accumulation */
                struct pgwt_pid_accum *pa = pgwt_get_or_create_pid(&d->accum, snext);
                if (pa) {
                    struct pgwt_event_stats *es = pgwt_get_or_create_event(pa, we);
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
                struct pgwt_event_stats *se = pgwt_get_or_create_system_event(&d->accum, we);
                if (se) {
                    se->count += 1;
                    se->total_ns += open_ns;
                    if (open_ns < se->min_ns) se->min_ns = open_ns;
                    if (open_ns > se->max_ns) se->max_ns = open_ns;
                }

                /* Time model by class */
                pgwt_update_time_model(&d->accum.tm, we, open_ns);

                /* Query-level open interval */
                if (sval.last_query_id != 0) {
                    struct pgwt_query_event_stats *qe =
                        pgwt_get_or_create_query_event(&d->accum, sval.last_query_id, we);
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
}

/* Read BPF accum_map (lightweight mode) and merge into event_accum.
 * Deletes entries after reading so next interval starts fresh. */
void pgwt_read_accum_map(struct pgwt_daemon *d)
{
    int accum_fd = bpf_map__fd(d->skel->maps.accum_map);
    struct pgwt_accumulator *acc = d->event_accum;

    uint32_t key = 0, next_key = 0;
    struct pgwt_accum_val val;

    while (bpf_map_get_next_key(accum_fd, &key, &next_key) == 0) {
        if (bpf_map_lookup_elem(accum_fd, &next_key, &val) == 0) {
            uint32_t we = next_key;
            uint64_t dur = val.total_ns;

            /* Time model by class */
            pgwt_update_time_model(&acc->tm, we, dur);

            /* System-wide event stats */
            struct pgwt_event_stats *se = pgwt_get_or_create_system_event(acc, we);
            if (se) {
                se->count += val.count;
                se->total_ns += dur;
            }

            /* Delete after reading */
            bpf_map_delete_elem(accum_fd, &next_key);
        }
        key = next_key;
    }
}

