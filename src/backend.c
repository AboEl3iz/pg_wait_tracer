/* backend.c — Backend lifecycle: scan, fork, init, exit handling */
#include "backend.h"
#include "daemon.h"
#include "perf_event.h"
#include "discovery.h"
#include "cmdline.h"
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

        /* Format: pid (comm) state ppid ... */
        int stat_pid;
        char comm[256];
        char state;
        int ppid;
        if (fscanf(f, "%d %255s %c %d", &stat_pid, comm, &state, &ppid) != 4) {
            fclose(f);
            continue;
        }
        fclose(f);

        if (ppid != d->postmaster_pid)
            continue;

        /* This is a postgres child. Read its my_wait_event_info pointer. */
        uint64_t ptr = pgwt_read_pointer(pid, d->my_wait_ptr_addr);
        if (ptr == 0) {
            /* Not yet initialized — treat as fresh fork */
            if (d->verbose)
                fprintf(stderr, "INFO: PID %d not yet initialized, attaching bootstrap\n", pid);
            pgwt_handle_fork(d, pid);
            continue;
        }

        /* Already initialized — attach real watchpoint directly */
        struct pgwt_backend *be = alloc_backend(&d->backends, pid);
        if (!be) continue;

        be->wp_addr = ptr;
        int wp_prog_fd = bpf_program__fd(d->skel->progs.on_watchpoint);
        be->wp_fd = pgwt_open_watchpoint(pid, ptr, wp_prog_fd);
        if (be->wp_fd < 0) {
            fprintf(stderr, "WARN: cannot attach watchpoint to PID %d\n", pid);
            be->is_alive = false;
            be->pid = 0;
            continue;
        }

        be->attach_ts = now_ns();
        pgwt_parse_cmdline(pid, &be->meta);

        /* Pre-initialize BPF state_map so the first watchpoint fire
         * ACCUMULATES the current wait state instead of just initializing.
         * Without this, backends already in a wait (e.g. Lock:relation)
         * would lose their initial interval because on_watchpoint treats
         * the first fire as state_map initialization. */
        {
            char mem_path[64];
            snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", pid);
            int mem_fd = open(mem_path, O_RDONLY);
            if (mem_fd >= 0) {
                uint32_t current_wei = 0;
                if (pread(mem_fd, &current_wei, sizeof(current_wei), ptr) ==
                    sizeof(current_wei)) {
                    /* Read current query_id from MyBEEntry->st_query_id */
                    uint64_t current_qid = 0;
                    if (d->my_be_entry_addr) {
                        uint64_t be_ptr = 0;
                        if (pread(mem_fd, &be_ptr, sizeof(be_ptr),
                                  d->my_be_entry_addr) == sizeof(be_ptr)
                            && be_ptr
                            && pread(mem_fd, &current_qid,
                                     sizeof(current_qid),
                                     be_ptr + PGWT_ST_QUERY_ID_OFFSET)
                               != sizeof(current_qid))
                            current_qid = 0;
                    }

                    int state_fd = bpf_map__fd(d->skel->maps.state_map);
                    struct pgwt_pid_state init_state = {
                        .last_event = current_wei,
                        .last_ts = be->attach_ts,
                        .last_query_id = current_qid,
                    };
                    uint32_t pid_key = pid;
                    bpf_map_update_elem(state_fd, &pid_key, &init_state,
                                        BPF_NOEXIST);
                }
                close(mem_fd);
            }
        }

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

    /* Attach bootstrap watchpoint on my_wait_event_info pointer address */
    int bootstrap_prog_fd = bpf_program__fd(d->skel->progs.on_bootstrap);
    be->bootstrap_fd = pgwt_open_watchpoint(child_pid, d->my_wait_ptr_addr,
                                             bootstrap_prog_fd);
    if (be->bootstrap_fd < 0) {
        /* Race: child may have already exited or initialized.
         * Try direct init path. */
        uint64_t ptr = pgwt_read_pointer(child_pid, d->my_wait_ptr_addr);
        if (ptr != 0) {
            return pgwt_handle_init(d, child_pid, ptr);
        }
        fprintf(stderr, "WARN: cannot attach bootstrap to PID %d\n", child_pid);
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

    /* Attach real watchpoint on PGPROC->wait_event_info */
    be->wp_addr = addr;
    int wp_prog_fd = bpf_program__fd(d->skel->progs.on_watchpoint);
    be->wp_fd = pgwt_open_watchpoint(pid, addr, wp_prog_fd);
    if (be->wp_fd < 0) {
        fprintf(stderr, "WARN: cannot attach real watchpoint to PID %d\n", pid);
        be->is_alive = false;
        be->pid = 0;
        return -1;
    }

    be->attach_ts = now_ns();
    pgwt_parse_cmdline(pid, &be->meta);

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

    /* Delete wait_stats entries for this PID.
     * We iterate to find all keys with this PID. */
    int stats_fd = bpf_map__fd(d->skel->maps.wait_stats);
    struct pgwt_agg_key skey = {}, next_key;
    while (bpf_map_get_next_key(stats_fd, &skey, &next_key) == 0) {
        if (next_key.pid == (uint32_t)pid) {
            bpf_map_delete_elem(stats_fd, &next_key);
            /* Don't advance — deletion may affect iteration.
             * Restart from the deleted key position. */
            skey = next_key;
            skey.wait_event = 0; /* force re-search from this pid */
            continue;
        }
        skey = next_key;
    }

    be->is_alive = false;

    if (d->verbose)
        fprintf(stderr, "INFO: PID %d exited (%s)\n",
                pid, pgwt_backend_type_name(be->meta.backend_type));
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
