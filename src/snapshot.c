/* snapshot.c — Compact snapshots and ring buffer for time-windowed delta */
#include "snapshot.h"
#include "map_reader.h"

#include <stdlib.h>
#include <string.h>

int pgwt_ring_init(struct pgwt_ring *ring, int capacity)
{
    ring->slots = calloc(capacity, sizeof(struct pgwt_snapshot));
    if (!ring->slots)
        return -1;
    ring->capacity = capacity;
    ring->head = 0;
    ring->count = 0;
    return 0;
}

void pgwt_ring_free(struct pgwt_ring *ring)
{
    free(ring->slots);
    ring->slots = NULL;
    ring->capacity = 0;
    ring->head = 0;
    ring->count = 0;
}

void pgwt_ring_push(struct pgwt_ring *ring, const struct pgwt_accumulator *acc)
{
    struct pgwt_snapshot *snap = &ring->slots[ring->head % ring->capacity];

    /* Time model: copy as-is */
    snap->tm = acc->tm;

    /* System events: compact (skip zero-count entries) */
    int ne = 0;
    for (int i = 0; i < acc->num_system_events && ne < MAX_SNAP_EVENTS; i++) {
        const struct pgwt_event_stats *se = &acc->system_events[i];
        if (se->count == 0)
            continue;
        struct pgwt_snap_event *dst = &snap->events[ne++];
        dst->wait_event = se->wait_event;
        dst->count = se->count;
        dst->total_ns = se->total_ns;
        memcpy(dst->histogram, se->histogram, sizeof(dst->histogram));
    }
    snap->num_events = ne;

    /* Query events: compact */
    int nq = 0;
    for (int i = 0; i < acc->num_query_events && nq < MAX_SNAP_QUERIES; i++) {
        const struct pgwt_query_event_stats *qe = &acc->query_events[i];
        if (qe->count == 0)
            continue;
        struct pgwt_snap_query_event *dst = &snap->query_events[nq++];
        dst->query_id = qe->query_id;
        dst->wait_event = qe->wait_event;
        dst->count = qe->count;
        dst->total_ns = qe->total_ns;
    }
    snap->num_query_events = nq;

    ring->head++;
    if (ring->count < ring->capacity)
        ring->count++;
}

const struct pgwt_snapshot *pgwt_ring_latest(const struct pgwt_ring *ring)
{
    if (ring->count == 0)
        return NULL;
    return &ring->slots[(ring->head - 1) % ring->capacity];
}

/* Find event by wait_event in a snapshot. Returns NULL if not found. */
static const struct pgwt_snap_event *
find_snap_event(const struct pgwt_snapshot *snap, uint32_t wait_event)
{
    for (int i = 0; i < snap->num_events; i++) {
        if (snap->events[i].wait_event == wait_event)
            return &snap->events[i];
    }
    return NULL;
}

/* Find query event by (query_id, wait_event). Returns NULL if not found. */
static const struct pgwt_snap_query_event *
find_snap_query_event(const struct pgwt_snapshot *snap,
                      uint64_t query_id, uint32_t wait_event)
{
    for (int i = 0; i < snap->num_query_events; i++) {
        if (snap->query_events[i].query_id == query_id &&
            snap->query_events[i].wait_event == wait_event)
            return &snap->query_events[i];
    }
    return NULL;
}

int pgwt_ring_delta(const struct pgwt_ring *ring, int ticks_ago,
                    struct pgwt_snapshot *out)
{
    if (ticks_ago >= ring->count)
        return -1;

    const struct pgwt_snapshot *curr =
        &ring->slots[(ring->head - 1) % ring->capacity];
    const struct pgwt_snapshot *prev =
        &ring->slots[(ring->head - 1 - ticks_ago) % ring->capacity];

    /* Time model: field-by-field subtraction */
    out->tm.db_time_ns        = curr->tm.db_time_ns        - prev->tm.db_time_ns;
    out->tm.cpu_time_ns       = curr->tm.cpu_time_ns       - prev->tm.cpu_time_ns;
    out->tm.io_time_ns        = curr->tm.io_time_ns        - prev->tm.io_time_ns;
    out->tm.lwlock_time_ns    = curr->tm.lwlock_time_ns    - prev->tm.lwlock_time_ns;
    out->tm.lock_time_ns      = curr->tm.lock_time_ns      - prev->tm.lock_time_ns;
    out->tm.bufferpin_time_ns = curr->tm.bufferpin_time_ns - prev->tm.bufferpin_time_ns;
    out->tm.client_time_ns    = curr->tm.client_time_ns    - prev->tm.client_time_ns;
    out->tm.ipc_time_ns       = curr->tm.ipc_time_ns       - prev->tm.ipc_time_ns;
    out->tm.timeout_time_ns   = curr->tm.timeout_time_ns   - prev->tm.timeout_time_ns;
    out->tm.extension_time_ns = curr->tm.extension_time_ns - prev->tm.extension_time_ns;
    out->tm.activity_time_ns  = curr->tm.activity_time_ns  - prev->tm.activity_time_ns;

    /* System events: for each event in curr, subtract prev if found */
    int ne = 0;
    for (int i = 0; i < curr->num_events && ne < MAX_SNAP_EVENTS; i++) {
        const struct pgwt_snap_event *ce = &curr->events[i];
        const struct pgwt_snap_event *pe =
            find_snap_event(prev, ce->wait_event);

        struct pgwt_snap_event *dst = &out->events[ne++];
        dst->wait_event = ce->wait_event;

        if (pe) {
            dst->count = ce->count - pe->count;
            dst->total_ns = ce->total_ns - pe->total_ns;
            for (int b = 0; b < HISTOGRAM_BUCKETS; b++)
                dst->histogram[b] = ce->histogram[b] - pe->histogram[b];
        } else {
            /* New event since prev snapshot */
            dst->count = ce->count;
            dst->total_ns = ce->total_ns;
            memcpy(dst->histogram, ce->histogram, sizeof(dst->histogram));
        }
    }
    out->num_events = ne;

    /* Query events: same approach */
    int nq = 0;
    for (int i = 0; i < curr->num_query_events && nq < MAX_SNAP_QUERIES; i++) {
        const struct pgwt_snap_query_event *cq = &curr->query_events[i];
        const struct pgwt_snap_query_event *pq =
            find_snap_query_event(prev, cq->query_id, cq->wait_event);

        struct pgwt_snap_query_event *dst = &out->query_events[nq++];
        dst->query_id = cq->query_id;
        dst->wait_event = cq->wait_event;

        if (pq) {
            dst->count = cq->count - pq->count;
            dst->total_ns = cq->total_ns - pq->total_ns;
        } else {
            dst->count = cq->count;
            dst->total_ns = cq->total_ns;
        }
    }
    out->num_query_events = nq;

    return 0;
}
