/* wait_event.h — Wait event decode: ID → human-readable name */
#ifndef PGWT_WAIT_EVENT_H
#define PGWT_WAIT_EVENT_H

#include <stdint.h>
#include <stddef.h>

/* Initialize event name tables for the given PG major version.
 * Must be called before any event name lookup functions.
 * Falls back to PG18 tables for unknown versions. */
void pgwt_init_event_names(int pg_major);

/* Load dynamic event names from a running PostgreSQL instance.
 * Runs: psql -U <user> -p <port> -d postgres -tAF'|' -c "SELECT ..."
 * On PG17+, queries pg_wait_events for all event names.
 * Returns 0 on success, -1 on failure (falls back to hardcoded tables). */
int pgwt_load_event_names_from_pg(const char *pg_bindir, int pg_port,
                                  const char *pg_user);

/* Load dynamic event names from an in-memory buffer of "Type|Name\n"
 * lines (the pg_wait_events query output format). Same id-mapping logic
 * as pgwt_load_event_names_from_pg, but without a live PG — used by unit
 * tests. Returns 0 on success, -1 on failure. */
int pgwt_load_event_names_from_buffer(const char *data);

/* Write current event name mapping to a JSON sidecar file.
 * Path: <trace_dir>/wait_event_names.json
 * Returns 0 on success, -1 on failure. */
int pgwt_write_names_json(const char *trace_dir);

/* Load event name mapping from a JSON sidecar file.
 * Path: <trace_dir>/wait_event_names.json
 * Returns 0 on success (overrides hardcoded tables), -1 if not found. */
int pgwt_load_names_json(const char *trace_dir);

/* Returns class name: "IO", "LWLock", "Lock", "CPU", etc. */
const char *pgwt_class_name(uint32_t wait_event_info);

/* Returns event name within class: "DataFileRead", "WALInsert", etc.
 * For event=0 (CPU), returns "CPU". */
const char *pgwt_event_name(uint32_t wait_event_info);

/* Writes "class:event" (e.g. "IO:DataFileRead") to buf.
 * For event=0, writes "CPU*" (asterisk: not all CPU time is instrumented). */
void pgwt_event_full_name(uint32_t wait_event_info, char *buf, size_t bufsz);

/* LOAD accounting: returns true if this event must be EXCLUDED from
 * DB Time / AAS / active load. True for Activity-class events AND for
 * Client:ClientRead (idle between commands — like Oracle's "SQL*Net
 * message from client"). Use this anywhere you are deciding how much
 * LOAD an event represents (DB Time, idle bucket, AAS, active sessions,
 * % of DB Time, interference, etc.). */
int pgwt_is_idle_event(uint32_t wait_event_info);

/* VISIBILITY: returns true if this event must NOT be displayed in event
 * lists / graphs / breakdowns. True for Activity-class events ONLY.
 * Note: this deliberately does NOT include Client:ClientRead — that
 * event is excluded from load (pgwt_is_idle_event) but must remain
 * VISIBLE in event lists, timelines, transition graphs, histograms,
 * and class drill-downs. Use this anywhere you are deciding whether an
 * event ROW/series/marker should APPEAR to the user. */
int pgwt_is_hidden_event(uint32_t wait_event_info);

#endif /* PGWT_WAIT_EVENT_H */
