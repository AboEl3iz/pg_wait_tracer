/* backend_meta.h — Write backend metadata to backends.jsonl for web UI */
#ifndef PGWT_BACKEND_META_H
#define PGWT_BACKEND_META_H

#include "cmdline.h"
#include <stdio.h>
#include <sys/types.h>

struct pgwt_backend_meta_writer {
    char  trace_dir[256];
    FILE *fp;
};

/* Open/create backends.jsonl in trace_dir. Returns 0 on success. */
int pgwt_bm_init(struct pgwt_backend_meta_writer *bm, const char *trace_dir);

/* Write one backend entry. Safe to call multiple times for same PID
 * (server deduplicates on load). */
void pgwt_bm_write(struct pgwt_backend_meta_writer *bm,
                    pid_t pid, const struct pgwt_metadata *meta);

/* Close the file. */
void pgwt_bm_close(struct pgwt_backend_meta_writer *bm);

#endif /* PGWT_BACKEND_META_H */
