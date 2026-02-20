/* perf_event.h — perf_event_open wrapper for hardware watchpoints */
#ifndef PGWT_PERF_EVENT_H
#define PGWT_PERF_EVENT_H

#include <sys/types.h>
#include <stdint.h>

/* Open a hardware write-watchpoint on addr for pid, attach bpf_prog_fd.
 * Returns perf_event fd (>= 0) on success, -1 on error (errno set). */
int pgwt_open_watchpoint(pid_t pid, uint64_t addr, int bpf_prog_fd);

/* Close a watchpoint fd (auto-detaches the hardware breakpoint). */
void pgwt_close_watchpoint(int perf_fd);

#endif /* PGWT_PERF_EVENT_H */
