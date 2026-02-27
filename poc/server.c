/*
 * pgwt-server-poc — minimal PoC server for SSH stdin/stdout protocol.
 *
 * Reads JSON-line requests from stdin, writes JSON-line responses to stdout.
 * Compile: gcc -O2 -o pgwt-server-poc server.c
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

int main(void)
{
    char line[4096];
    char host[256] = "unknown";
    gethostname(host, sizeof(host));
    long ncpus = sysconf(_SC_NPROCESSORS_ONLN);

    /* Line-buffered stdout — critical for SSH pipe */
    setvbuf(stdout, NULL, _IOLBF, 0);

    while (fgets(line, sizeof(line), stdin)) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        long long now_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

        if (strstr(line, "\"ping\""))
            printf("{\"cmd\":\"pong\",\"host\":\"%s\",\"cpus\":%ld,"
                   "\"pid\":%d,\"ts\":%lld}\n",
                   host, ncpus, (int)getpid(), now_ms);
        else if (strstr(line, "\"info\""))
            printf("{\"cmd\":\"info\",\"host\":\"%s\",\"cpus\":%ld,"
                   "\"pid\":%d,\"ts\":%lld}\n",
                   host, ncpus, (int)getpid(), now_ms);
        else
            printf("{\"error\":\"unknown command\"}\n");
    }

    return 0;
}
