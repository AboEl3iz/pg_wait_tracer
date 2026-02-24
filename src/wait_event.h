/* wait_event.h — Wait event decode: ID → human-readable name */
#ifndef PGWT_WAIT_EVENT_H
#define PGWT_WAIT_EVENT_H

#include <stdint.h>
#include <stddef.h>

/* Initialize event name tables for the given PG major version.
 * Must be called before any event name lookup functions.
 * Falls back to PG18 tables for unknown versions. */
void pgwt_init_event_names(int pg_major);

/* Returns class name: "IO", "LWLock", "Lock", "CPU", etc. */
const char *pgwt_class_name(uint32_t wait_event_info);

/* Returns event name within class: "DataFileRead", "WALInsert", etc.
 * For event=0 (CPU), returns "CPU". */
const char *pgwt_event_name(uint32_t wait_event_info);

/* Writes "class:event" (e.g. "IO:DataFileRead") to buf.
 * For event=0, writes "CPU*" (asterisk: not all CPU time is instrumented). */
void pgwt_event_full_name(uint32_t wait_event_info, char *buf, size_t bufsz);

/* Returns true if this event class represents idle waits
 * (Activity class — should be excluded from DB Time). */
int pgwt_is_idle_event(uint32_t wait_event_info);

#endif /* PGWT_WAIT_EVENT_H */
