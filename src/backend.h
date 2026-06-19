/* backend.h — Backend lifecycle manager: watchpoint attach/detach */
#ifndef PGWT_BACKEND_H
#define PGWT_BACKEND_H

#include "pg_wait_tracer.h"
#include "cmdline.h"

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>

struct pgwt_backend {
    pid_t    pid;
    int      wp_fd;            /* real watchpoint perf_event fd (-1 if none) */
    int      bootstrap_fd;     /* bootstrap watchpoint fd (-1 if none) */
    uint64_t wp_addr;          /* PGPROC->wait_event_info address */
    uint64_t attach_ts;        /* monotonic ns when attached */
    bool     is_alive;
    struct pgwt_metadata meta;
};

struct pgwt_backend_table {
    struct pgwt_backend entries[MAX_BACKENDS];
    int count;
};

/* Forward declaration */
struct pgwt_daemon;

/* Initialize the backend table. */
void pgwt_backend_init(struct pgwt_backend_table *bt);

/* Scan existing backends at startup. Attaches real watchpoints directly
 * (skips bootstrap — they are already initialized). */
int pgwt_scan_existing_backends(struct pgwt_daemon *d);

/* Handle a new fork from postmaster. Attaches bootstrap watchpoint. */
int pgwt_handle_fork(struct pgwt_daemon *d, pid_t child_pid);

/* Handle initialization complete (bootstrap fired). Switch to real watchpoint. */
int pgwt_handle_init(struct pgwt_daemon *d, pid_t pid, uint64_t addr);

/* Handle backend exit. Close watchpoint, flush stats, cleanup. */
int pgwt_handle_exit(struct pgwt_daemon *d, pid_t pid);

/* Attach a hardware watchpoint to an already-resolved backend (be->wp_addr
 * must be set) and pre-seed its BPF state_map with the current wait state.
 * Used by the full-mode scan and by the A4 escalation engine. Returns 0 on
 * success, -1 on attach failure (caller decides cleanup). No-op (returns 0)
 * if the backend already has a watchpoint. */
int pgwt_attach_backend_watchpoint(struct pgwt_daemon *d,
                                   struct pgwt_backend *be);

/* Resolve a backend's wait_event_info address using the version-appropriate
 * path (PG17+ my_wait_event_info global vs PG<17 MyProc+offset). Returns 0 if
 * the backend has not yet set the pointer (still in early init). */
uint64_t pgwt_resolve_backend_wait_addr(struct pgwt_daemon *d, pid_t pid);

/* Find backend by PID. Returns NULL if not found. */
struct pgwt_backend *pgwt_find_backend(struct pgwt_backend_table *bt, pid_t pid);

/* Close all watchpoints (cleanup). */
void pgwt_close_all_backends(struct pgwt_backend_table *bt);

#endif /* PGWT_BACKEND_H */
