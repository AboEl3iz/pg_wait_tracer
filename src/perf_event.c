/* perf_event.c — perf_event_open wrapper for hardware watchpoints */
#include "perf_event.h"

#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

static long sys_perf_event_open(struct perf_event_attr *attr, pid_t pid,
                                int cpu, int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

int pgwt_open_watchpoint(pid_t pid, uint64_t addr, int bpf_prog_fd)
{
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));

    attr.type           = PERF_TYPE_BREAKPOINT;
    attr.size           = sizeof(attr);
    attr.bp_type        = HW_BREAKPOINT_W;
    attr.bp_addr        = addr;
    attr.bp_len         = HW_BREAKPOINT_LEN_4;
    attr.sample_period  = 1;
    attr.sample_type    = PERF_SAMPLE_RAW;
    attr.wakeup_events  = 1;

    int fd = sys_perf_event_open(&attr, pid, -1, -1, PERF_FLAG_FD_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "perf_event_open(pid=%d, addr=0x%lx): %s\n",
                pid, (unsigned long)addr, strerror(errno));
        return -1;
    }

    if (ioctl(fd, PERF_EVENT_IOC_SET_BPF, bpf_prog_fd) != 0) {
        fprintf(stderr, "ioctl SET_BPF(pid=%d): %s\n", pid, strerror(errno));
        close(fd);
        return -1;
    }

    if (ioctl(fd, PERF_EVENT_IOC_ENABLE, 0) != 0) {
        fprintf(stderr, "ioctl ENABLE(pid=%d): %s\n", pid, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

void pgwt_close_watchpoint(int perf_fd)
{
    if (perf_fd >= 0)
        close(perf_fd);
}
