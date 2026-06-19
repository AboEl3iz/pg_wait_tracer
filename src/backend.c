/* backend.c — Backend lifecycle: scan, fork, init, exit handling */
#include "backend.h"
#include "daemon.h"
#include "perf_event.h"
#include "discovery.h"
#include "cmdline.h"
#include "backend_meta.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <stdlib.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "pg_wait_tracer.skel.h"

void pgwt_backend_init(struct pgwt_backend_table *bt)
{
    memset(bt, 0, sizeof(*bt));
    for (int i = 0; i < MAX_BACKENDS; i++) {
        bt->entries[i].wp_fd = -1;
        bt->entries[i].bootstrap_fd = -1;
    }
}

struct pgwt_backend *pgwt_find_backend(struct pgwt_backend_table *bt, pid_t pid)
{
    for (int i = 0; i < bt->count; i++) {
        if (bt->entries[i].pid == pid)
            return &bt->entries[i];
    }
    return NULL;
}

static struct pgwt_backend *alloc_backend(struct pgwt_backend_table *bt, pid_t pid)
{
    /* Reuse dead slot */
    for (int i = 0; i < bt->count; i++) {
        if (!bt->entries[i].is_alive && bt->entries[i].pid == 0) {
            memset(&bt->entries[i], 0, sizeof(bt->entries[i]));
            bt->entries[i].wp_fd = -1;
            bt->entries[i].bootstrap_fd = -1;
            bt->entries[i].pid = pid;
            bt->entries[i].is_alive = true;
            return &bt->entries[i];
        }
    }
    /* Append */
    if (bt->count >= MAX_BACKENDS) {
        fprintf(stderr, "WARN: max backends (%d) reached, cannot track PID %d\n",
                MAX_BACKENDS, pid);
        return NULL;
    }
    struct pgwt_backend *be = &bt->entries[bt->count++];
    memset(be, 0, sizeof(*be));
    be->wp_fd = -1;
    be->bootstrap_fd = -1;
    be->pid = pid;
    be->is_alive = true;
    return be;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Resolve a backend's wait_event_info address using the version-appropriate
 * path. PG17+: my_wait_ptr_addr is the my_wait_event_info global (a uint32*
 * that already points at the field). PG<17 (use_myproc): my_wait_ptr_addr is
 * the MyProc PGPROC* global — deref it and add pgproc_wait_offset. Returns 0
 * if the backend has not set MyProc / the pointer yet. */
uint64_t pgwt_resolve_backend_wait_addr(struct pgwt_daemon *d, pid_t pid)
{
    if (d->use_myproc)
        return pgwt_resolve_wait_addr_via_myproc(pid, d->my_wait_ptr_addr,
                                                 d->pgproc_wait_offset);
    return pgwt_read_pointer(pid, d->my_wait_ptr_addr);
}

/* Pre-seed the BPF state_map for a backend whose wait_event_info address is
 * already resolved (be->wp_addr). Reads the backend's CURRENT wait_event_info
 * and query_id and writes them as the initial state, so the first watchpoint
 * fire ACCUMULATES the in-progress wait instead of discarding it (otherwise a
 * backend already blocked at attach time — e.g. Lock:relation — loses its
 * opening interval). Idempotent via BPF_NOEXIST. */
static void preseed_state_map(struct pgwt_daemon *d, pid_t pid, uint64_t addr,
                              uint64_t attach_ts)
{
    char mem_path[64];
    snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", pid);
    int mem_fd = open(mem_path, O_RDONLY);
    if (mem_fd < 0)
        return;

    uint32_t current_wei = 0;
    if (pread(mem_fd, &current_wei, sizeof(current_wei), addr) ==
        sizeof(current_wei)) {
        /* Read current query_id from MyBEEntry->st_query_id */
        uint64_t current_qid = 0;
        if (d->my_be_entry_addr && d->st_query_id_offset > 0) {
            uint64_t be_ptr = 0;
            if (pread(mem_fd, &be_ptr, sizeof(be_ptr),
                      d->my_be_entry_addr) == sizeof(be_ptr)
                && be_ptr
                && pread(mem_fd, &current_qid,
                         sizeof(current_qid),
                         be_ptr + d->st_query_id_offset)
                   != sizeof(current_qid))
                current_qid = 0;
        }

        int state_fd = bpf_map__fd(d->skel->maps.state_map);
        struct pgwt_pid_state init_state = {
            .last_event = current_wei,
            .last_ts = attach_ts,
            .last_query_id = current_qid,
            .wait_event_addr = addr,
        };
        uint32_t pid_key = pid;
        /* BPF_ANY (not NOEXIST): on first full-mode attach this creates the
         * entry; on tiered RE-escalation it REFRESHES a reset/sampled entry to
         * the backend's CURRENT wait state, so an in-progress wait at escalate
         * time is captured instead of being lost behind a stale last_event. */
        bpf_map_update_elem(state_fd, &pid_key, &init_state, BPF_ANY);
    }
    close(mem_fd);
}

int pgwt_attach_backend_watchpoint(struct pgwt_daemon *d,
                                   struct pgwt_backend *be)
{
    if (be->wp_addr == 0 || be->wp_fd >= 0)
        return be->wp_fd >= 0 ? 0 : -1;

    int wp_prog_fd = bpf_program__fd(d->skel->progs.on_watchpoint);
    be->wp_fd = pgwt_open_watchpoint(be->pid, be->wp_addr, wp_prog_fd);
    if (be->wp_fd < 0) {
        /* Process likely exited before attach — caller decides on cleanup. */
        d->counters.wp_attach_failures_total++;
        return -1;
    }

    if (be->attach_ts == 0)
        be->attach_ts = now_ns();
    preseed_state_map(d, be->pid, be->wp_addr, be->attach_ts);
    return 0;
}

int pgwt_scan_existing_backends(struct pgwt_daemon *d)
{
    DIR *proc = opendir("/proc");
    if (!proc) {
        perror("opendir /proc");
        return -1;
    }

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(proc)) != NULL) {
        pid_t pid = atoi(ent->d_name);
        if (pid <= 0)
            continue;

        /* Read /proc/<pid>/stat to get ppid (field 4) */
        char stat_path[64];
        snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
        FILE *f = fopen(stat_path, "r");
        if (!f) continue;

        /* Format: pid (comm) state ppid ...
         * comm can contain spaces and parens, so find last ')' */
        char stat_line[512];
        if (!fgets(stat_line, sizeof(stat_line), f)) { fclose(f); continue; }
        fclose(f);
        char *last_paren = strrchr(stat_line, ')');
        if (!last_paren) continue;
        char state;
        int ppid;
        if (sscanf(last_paren + 1, " %c %d", &state, &ppid) != 2)
            continue;

        if (ppid != d->postmaster_pid)
            continue;

        /* This is a postgres child. Resolve its wait_event_info address
         * (PG17+ my_wait_event_info global; PG<17 MyProc + offset). */
        uint64_t ptr = pgwt_resolve_backend_wait_addr(d, pid);
        if (ptr == 0) {
            /* Not yet initialized — treat as fresh fork */
            if (d->verbose)
                fprintf(stderr, "INFO: PID %d not yet initialized, attaching bootstrap\n", pid);
            pgwt_handle_fork(d, pid);
            continue;
        }

        /* Runtime validation (PG<17 MyProc path): the offset is global, so a
         * single sane reading proves it. Check the first backend we resolve;
         * if its wait_event_info value's class byte is garbage, the offset is
         * wrong (custom build / mis-detected layout) — refuse to attach rather
         * than trace nonsense. PG17+ skips this (my_wait_event_info already
         * points exactly at the field). */
        if (d->use_myproc && !d->wait_offset_validated) {
            int v = pgwt_validate_wait_addr(pid, ptr);
            if (v == 0) {
                fprintf(stderr,
                        "FATAL: resolved wait_event_info for PID %d holds a value "
                        "with an invalid wait-event class byte.\n"
                        "  offsetof(PGPROC, wait_event_info)=%d is likely wrong for "
                        "this PG%d build — refusing to attach.\n",
                        pid, d->pgproc_wait_offset, d->pg_major_version);
                closedir(proc);
                return -1;
            }
            if (v == 1)
                d->wait_offset_validated = true;
            /* v < 0: transient read error — try the next backend. */
        }

        /* Already initialized — record the address. */
        struct pgwt_backend *be = alloc_backend(&d->backends, pid);
        if (!be) continue;

        be->wp_addr = ptr;

        /* Sampled mode: register the backend (address + metadata) but arm
         * NO watchpoint. The sampler reads wp_addr directly each tick. Seed
         * a state_map entry so the query_id uprobe can populate it (nothing
         * else creates state_map entries in sampled mode). */
        if (!pgwt_mode_uses_watchpoints(d)) {
            be->attach_ts = now_ns();
            pgwt_parse_cmdline(pid, &be->meta);
            if (d->backend_meta)
                pgwt_bm_write(d->backend_meta, pid, &be->meta);

            int state_fd = bpf_map__fd(d->skel->maps.state_map);
            struct pgwt_pid_state init_state = {
                .last_ts = be->attach_ts,
                .wait_event_addr = ptr,
            };
            uint32_t pid_key = pid;
            bpf_map_update_elem(state_fd, &pid_key, &init_state, BPF_NOEXIST);

            if (d->verbose)
                fprintf(stderr, "INFO: tracking PID %d (%s), sampled at 0x%lx\n",
                        pid, pgwt_backend_type_name(be->meta.backend_type),
                        (unsigned long)ptr);
            count++;
            continue;
        }

        /* Full mode — attach real watchpoint directly. The shared helper
         * opens the watchpoint and pre-seeds the BPF state_map with the
         * backend's current wait state (so an in-progress wait at attach
         * time is not lost). The same helper is reused on escalation. */
        be->attach_ts = now_ns();
        if (pgwt_attach_backend_watchpoint(d, be) != 0) {
            /* Silently skip — process likely exited before attach */
            be->is_alive = false;
            be->pid = 0;
            continue;
        }

        pgwt_parse_cmdline(pid, &be->meta);
        if (d->backend_meta)
            pgwt_bm_write(d->backend_meta, pid, &be->meta);

        if (d->verbose)
            fprintf(stderr, "INFO: attached to PID %d (%s), watchpoint at 0x%lx\n",
                    pid, pgwt_backend_type_name(be->meta.backend_type),
                    (unsigned long)ptr);
        count++;
    }
    closedir(proc);
    return count;
}

int pgwt_handle_fork(struct pgwt_daemon *d, pid_t child_pid)
{
    /* Check for duplicate */
    if (pgwt_find_backend(&d->backends, child_pid))
        return 0;

    struct pgwt_backend *be = alloc_backend(&d->backends, child_pid);
    if (!be) return -1;

    /* Sampled mode: no bootstrap watchpoint. Register the backend now; the
     * sampler lazily resolves wp_addr once the backend sets the pointer. */
    if (!pgwt_mode_uses_watchpoints(d)) {
        if (d->verbose)
            fprintf(stderr, "INFO: fork detected PID %d (sampled, no watchpoint)\n",
                    child_pid);
        return 0;
    }

    /* Attach bootstrap watchpoint on my_wait_event_info pointer address */
    int bootstrap_prog_fd = bpf_program__fd(d->skel->progs.on_bootstrap);
    be->bootstrap_fd = pgwt_open_watchpoint(child_pid, d->my_wait_ptr_addr,
                                             bootstrap_prog_fd);
    if (be->bootstrap_fd < 0) {
        /* Race: child may have already exited or initialized.
         * Try direct init path (version-aware address resolution). */
        uint64_t ptr = pgwt_resolve_backend_wait_addr(d, child_pid);
        if (ptr != 0) {
            return pgwt_handle_init(d, child_pid, ptr);
        }
        /* Silently skip — process exited before bootstrap attach */
        d->counters.wp_attach_failures_total++;
        be->is_alive = false;
        be->pid = 0;
        return -1;
    }

    if (d->verbose)
        fprintf(stderr, "INFO: fork detected PID %d, bootstrap watchpoint attached\n",
                child_pid);
    return 0;
}

int pgwt_handle_init(struct pgwt_daemon *d, pid_t pid, uint64_t addr)
{
    struct pgwt_backend *be = pgwt_find_backend(&d->backends, pid);
    if (!be) {
        /* Unknown PID — might have been forked before daemon started */
        be = alloc_backend(&d->backends, pid);
        if (!be) return -1;
    }

    /* Close bootstrap watchpoint if any */
    if (be->bootstrap_fd >= 0) {
        pgwt_close_watchpoint(be->bootstrap_fd);
        be->bootstrap_fd = -1;
    }

    /* Close previous watchpoint if re-initializing */
    if (be->wp_fd >= 0) {
        pgwt_close_watchpoint(be->wp_fd);
        be->wp_fd = -1;
    }

    /* Runtime offset validation (PG<17 MyProc path), once. Covers the case
     * where PG was idle at daemon start (scan validated nothing) and the
     * first backend arrives via bootstrap→init. A garbage class byte means
     * the offset is wrong — refuse rather than trace nonsense. */
    if (d->use_myproc && !d->wait_offset_validated) {
        int v = pgwt_validate_wait_addr(pid, addr);
        if (v == 0) {
            fprintf(stderr,
                    "FATAL: resolved wait_event_info for PID %d holds a value "
                    "with an invalid wait-event class byte.\n"
                    "  offsetof(PGPROC, wait_event_info)=%d is likely wrong for "
                    "this PG%d build — refusing to attach.\n",
                    pid, d->pgproc_wait_offset, d->pg_major_version);
            return -1;
        }
        if (v == 1)
            d->wait_offset_validated = true;
    }

    /* Attach real watchpoint on PGPROC->wait_event_info */
    be->wp_addr = addr;
    int wp_prog_fd = bpf_program__fd(d->skel->progs.on_watchpoint);
    be->wp_fd = pgwt_open_watchpoint(pid, addr, wp_prog_fd);
    if (be->wp_fd < 0) {
        /* Silently skip — process likely exited before attach */
        d->counters.wp_attach_failures_total++;
        be->is_alive = false;
        be->pid = 0;
        return -1;
    }

    be->attach_ts = now_ns();
    pgwt_parse_cmdline(pid, &be->meta);
    if (d->backend_meta)
        pgwt_bm_write(d->backend_meta, pid, &be->meta);

    if (d->verbose)
        fprintf(stderr, "INFO: PID %d initialized (%s), real watchpoint at 0x%lx\n",
                pid, pgwt_backend_type_name(be->meta.backend_type),
                (unsigned long)addr);
    return 0;
}

int pgwt_handle_exit(struct pgwt_daemon *d, pid_t pid)
{
    struct pgwt_backend *be = pgwt_find_backend(&d->backends, pid);
    if (!be) return 0;

    /* Close watchpoint fds */
    pgwt_close_watchpoint(be->wp_fd);
    be->wp_fd = -1;
    pgwt_close_watchpoint(be->bootstrap_fd);
    be->bootstrap_fd = -1;

    /* Delete BPF map entries for this PID */
    int state_fd = bpf_map__fd(d->skel->maps.state_map);
    uint32_t key = pid;
    bpf_map_delete_elem(state_fd, &key);

    be->is_alive = false;

    if (d->verbose)
        fprintf(stderr, "INFO: PID %d exited (%s)\n",
                pid, pgwt_backend_type_name(be->meta.backend_type));

    /* Reset PID so the slot can be reused by alloc_backend() */
    be->pid = 0;
    return 0;
}

void pgwt_close_all_backends(struct pgwt_backend_table *bt)
{
    for (int i = 0; i < bt->count; i++) {
        pgwt_close_watchpoint(bt->entries[i].wp_fd);
        bt->entries[i].wp_fd = -1;
        pgwt_close_watchpoint(bt->entries[i].bootstrap_fd);
        bt->entries[i].bootstrap_fd = -1;
    }
}
