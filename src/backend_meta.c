/* backend_meta.c — Write backend metadata to backends.jsonl */
#include "backend_meta.h"
#include <string.h>
#include <stdio.h>

int pgwt_bm_init(struct pgwt_backend_meta_writer *bm, const char *trace_dir)
{
    memset(bm, 0, sizeof(*bm));
    snprintf(bm->trace_dir, sizeof(bm->trace_dir), "%s", trace_dir);

    char path[512];
    snprintf(path, sizeof(path), "%s/backends.jsonl", trace_dir);
    bm->fp = fopen(path, "a");
    if (!bm->fp) {
        perror("fopen backends.jsonl");
        return -1;
    }
    return 0;
}

void pgwt_bm_write(struct pgwt_backend_meta_writer *bm,
                    pid_t pid, const struct pgwt_metadata *meta)
{
    if (!bm->fp) return;

    const char *type = pgwt_backend_type_name(meta->backend_type);

    fprintf(bm->fp, "{\"pid\":%d,\"type\":\"%s\"", pid, type);
    if (meta->usename[0])
        fprintf(bm->fp, ",\"user\":\"%s\"", meta->usename);
    if (meta->datname[0])
        fprintf(bm->fp, ",\"db\":\"%s\"", meta->datname);
    if (meta->client_addr[0])
        fprintf(bm->fp, ",\"addr\":\"%s\"", meta->client_addr);
    fprintf(bm->fp, "}\n");
    fflush(bm->fp);
}

void pgwt_bm_close(struct pgwt_backend_meta_writer *bm)
{
    if (bm->fp) {
        fclose(bm->fp);
        bm->fp = NULL;
    }
}
