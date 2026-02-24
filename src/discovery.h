/* discovery.h — Postmaster PID, ELF symbol offset, /proc helpers */
#ifndef PGWT_DISCOVERY_H
#define PGWT_DISCOVERY_H

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>

/* Find postmaster PID from pgdata/postmaster.pid. Returns 0 on error. */
pid_t pgwt_find_postmaster_pid(const char *pgdata);

/* Resolve absolute path to postgres binary via /proc/<pid>/exe. */
int pgwt_find_pg_binary(pid_t pid, char *buf, size_t bufsz);

/* Find symbol value in ELF binary (using libelf).
 * Returns the raw st_value from the ELF symbol table. */
uint64_t pgwt_find_symbol_offset(const char *binary, const char *symbol);

/* Find the load base address of the binary in the target process. */
uint64_t pgwt_find_load_base(pid_t pid, const char *binary_basename);

/* Resolve a symbol to its runtime virtual address in the target process.
 * Handles both PIE (ET_DYN) and non-PIE (ET_EXEC) binaries correctly. */
uint64_t pgwt_resolve_symbol(const char *binary, const char *symbol,
                             pid_t pid, const char *binary_basename);

/* Read an 8-byte pointer from /proc/<pid>/mem at addr. Returns 0 on error. */
uint64_t pgwt_read_pointer(pid_t pid, uint64_t addr);

/* Detect PostgreSQL major version from the postgres binary.
 * Runs "<binary> --version" and parses "postgres (PostgreSQL) XX.Y".
 * Returns major version (e.g. 14, 15, 16, 17, 18) or 0 on error. */
int pgwt_detect_pg_version(const char *pg_binary);

/* Detect st_query_id offset in PgBackendStatus.
 * Tries: 1) DWARF debug info, 2) known offset table.
 * Returns offset in bytes, or 0 if unavailable. */
int pgwt_detect_query_id_offset(const char *pg_binary, int pg_major);

/* Auto-discover a running PostgreSQL postmaster.
 * Scans /proc for postgres processes, filters children.
 * Returns PID if exactly one postmaster found, 0 otherwise.
 * On multiple instances, prints a list to stderr. */
pid_t pgwt_auto_discover_postmaster(bool verbose);

#endif /* PGWT_DISCOVERY_H */
