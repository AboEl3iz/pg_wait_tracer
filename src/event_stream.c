/* event_stream.c — BPF ringbuf event consumer + in-memory accumulation
 *
 * Processes raw trace events from event_ringbuf, accumulates into
 * d->event_accum. At timer tick, event_accum is copied to d->accum
 * for display (pgwt_accum_copy_used). */
#include "event_stream.h"
#include "daemon.h"
#include "map_reader.h"
#include "wait_event.h"

#include <string.h>

/* Log2 histogram bucket for a duration in nanoseconds.
 * Mirrors duration_to_bucket() in BPF program. */
static uint32_t duration_to_bucket(uint64_t ns)
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

int pgwt_handle_trace_event(void *ctx, void *data, size_t data_sz)
{
    struct pgwt_daemon *d = ctx;
    struct pgwt_trace_event *evt = data;
    struct pgwt_accumulator *acc = d->event_accum;

    (void)data_sz;

    uint32_t we = evt->old_event;
    uint64_t dur = evt->duration_ns;

    /* Per-PID accumulation */
    struct pgwt_pid_accum *pa = pgwt_get_or_create_pid(acc, evt->pid);
    if (pa) {
        struct pgwt_event_stats *es = pgwt_get_or_create_event(pa, we);
        if (es) {
            es->count++;
            es->total_ns += dur;
            if (dur < es->min_ns) es->min_ns = dur;
            if (dur > es->max_ns) es->max_ns = dur;
            uint32_t bucket = duration_to_bucket(dur);
            if (bucket < HISTOGRAM_BUCKETS)
                es->histogram[bucket]++;
        }

        if (we == 0) {
            pa->cpu_time_ns += dur;
        } else if (!pgwt_is_idle_event(we)) {
            pa->wait_time_ns += dur;
        }
        if (!pgwt_is_idle_event(we))
            pa->db_time_ns += dur;
    }

    /* System-wide accumulation */
    struct pgwt_event_stats *se = pgwt_get_or_create_system_event(acc, we);
    if (se) {
        se->count++;
        se->total_ns += dur;
        if (dur < se->min_ns) se->min_ns = dur;
        if (dur > se->max_ns) se->max_ns = dur;
        uint32_t bucket = duration_to_bucket(dur);
        if (bucket < HISTOGRAM_BUCKETS)
            se->histogram[bucket]++;
    }

    /* Time model by class */
    pgwt_update_time_model(&acc->tm, we, dur);

    /* Query events */
    if (evt->query_id != 0) {
        struct pgwt_query_event_stats *qe =
            pgwt_get_or_create_query_event(acc, evt->query_id, we);
        if (qe) {
            qe->count++;
            qe->total_ns += dur;
            if (dur < qe->min_ns) qe->min_ns = dur;
            if (dur > qe->max_ns) qe->max_ns = dur;
        }
    }

    return 0;
}

void pgwt_accum_copy_used(struct pgwt_accumulator *dst,
                           const struct pgwt_accumulator *src)
{
    /* Time model */
    dst->tm = src->tm;

    /* Per-PID data: copy only populated entries */
    dst->num_pids = src->num_pids;
    for (int i = 0; i < src->num_pids; i++) {
        const struct pgwt_pid_accum *sp = &src->pids[i];
        struct pgwt_pid_accum *dp = &dst->pids[i];

        dp->pid = sp->pid;
        dp->active = sp->active;
        dp->num_events = sp->num_events;
        dp->db_time_ns = sp->db_time_ns;
        dp->cpu_time_ns = sp->cpu_time_ns;
        dp->wait_time_ns = sp->wait_time_ns;
        dp->current_event = 0;      /* filled by pgwt_read_state_map */
        dp->current_wait_ns = 0;
        memcpy(dp->events, sp->events,
               sp->num_events * sizeof(struct pgwt_event_stats));
    }

    /* System-wide events */
    dst->num_system_events = src->num_system_events;
    memcpy(dst->system_events, src->system_events,
           src->num_system_events * sizeof(struct pgwt_event_stats));

    /* Query events */
    dst->num_query_events = src->num_query_events;
    memcpy(dst->query_events, src->query_events,
           src->num_query_events * sizeof(struct pgwt_query_event_stats));
}
