/* backend_meta.c — Write backend metadata to backends.jsonl */
#include "backend_meta.h"
#include <string.h>
#include <stdio.h>

static void json_escape_fp(FILE *fp, const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  fputs("\\\"", fp); break;
        case '\\': fputs("\\\\", fp); break;
        case '\n': fputs("\\n", fp);  break;
        case '\r': fputs("\\r", fp);  break;
        case '\t': fputs("\\t", fp);  break;
        default:
            if (c < 0x20)
                fprintf(fp, "\\u%04x", c);
            else
                fputc(c, fp);
        }
    }
}

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

    fprintf(bm->fp, "{\"pid\":%d,\"type\":\"", pid);
    json_escape_fp(bm->fp, type);
    fputc('"', bm->fp);
    if (meta->usename[0]) {
        fputs(",\"user\":\"", bm->fp);
        json_escape_fp(bm->fp, meta->usename);
        fputc('"', bm->fp);
    }
    if (meta->datname[0]) {
        fputs(",\"db\":\"", bm->fp);
        json_escape_fp(bm->fp, meta->datname);
        fputc('"', bm->fp);
    }
    if (meta->client_addr[0]) {
        fputs(",\"addr\":\"", bm->fp);
        json_escape_fp(bm->fp, meta->client_addr);
        fputc('"', bm->fp);
    }
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
