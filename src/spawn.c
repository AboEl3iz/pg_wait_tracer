/* spawn.c — fork/execvp child-process helper (CAP-7). See spawn.h. */
#define _GNU_SOURCE   /* pipe2 */
#include "spawn.h"

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>

int pgwt_proc_open(struct pgwt_proc *p, char *const argv[])
{
    p->out = NULL;
    p->pid = -1;

    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child: stdout -> pipe; stdin/stderr -> /dev/null (matches the
         * "2>/dev/null" the old popen command lines used). dup2 clears
         * O_CLOEXEC on the duplicated fd, so stdout survives the exec. */
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO)
                close(devnull);
        }
        if (dup2(fds[1], STDOUT_FILENO) < 0)
            _exit(127);
        execvp(argv[0], argv);
        _exit(127);   /* exec failed */
    }

    close(fds[1]);
    FILE *f = fdopen(fds[0], "r");
    if (!f) {
        close(fds[0]);
        /* Reap the child we can no longer talk to. */
        int st;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
            ;
        return -1;
    }
    p->out = f;
    p->pid = pid;
    return 0;
}

int pgwt_proc_close(struct pgwt_proc *p)
{
    if (!p->out)
        return -1;
    fclose(p->out);
    p->out = NULL;

    int status = 0;
    pid_t r;
    do {
        r = waitpid(p->pid, &status, 0);
    } while (r < 0 && errno == EINTR);
    p->pid = -1;
    return (r < 0) ? -1 : status;
}
