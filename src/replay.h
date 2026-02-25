/* replay.h — Offline replay of trace files into existing views */
#ifndef PGWT_REPLAY_H
#define PGWT_REPLAY_H

struct pgwt_daemon;

/* Run replay mode: scan trace files, decode blocks, accumulate, display.
 * from_str / to_str may be NULL (= no bound). Returns 0 on success. */
int pgwt_run_replay(struct pgwt_daemon *d, const char *from_str,
                    const char *to_str);

#endif /* PGWT_REPLAY_H */
