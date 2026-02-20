/* discovery.h — Postmaster PID, ELF symbol offset, /proc helpers */
#ifndef PGWT_DISCOVERY_H
#define PGWT_DISCOVERY_H

#include <sys/types.h>
#include <stdint.h>

/* Find postmaster PID from pgdata/postmaster.pid. Returns 0 on error. */
pid_t pgwt_find_postmaster_pid(const char *pgdata);

/* Resolve absolute path to postgres binary via /proc/<pid>/exe. */
int pgwt_find_pg_binary(pid_t pid, char *buf, size_t bufsz);

/* Find my_wait_event_info symbol offset in ELF binary (using libelf). */
uint64_t pgwt_find_symbol_offset(const char *binary, const char *symbol);

/* Find the load base address of the binary in the target process. */
uint64_t pgwt_find_load_base(pid_t pid, const char *binary_basename);

/* Read an 8-byte pointer from /proc/<pid>/mem at addr. Returns 0 on error. */
uint64_t pgwt_read_pointer(pid_t pid, uint64_t addr);

#endif /* PGWT_DISCOVERY_H */
