#include "fake_transport.h"
#include <stdio.h>
#include <string.h>

static int ft_perform(void *ctx, const htp_request_t *q, htp_response_t *r) {
    fake_transport_t *ft = ctx;
    int i = ft->req_count++;
    snprintf(ft->req[i].method, 8, "%s", q->method);
    snprintf(ft->req[i].path, 256, "%s", q->path);
    snprintf(ft->req[i].content_type, 40, "%s", q->content_type ? q->content_type : "");
    ft->req[i].header_count = q->header_count;
    for (int h = 0; h < q->header_count; h++)
        snprintf(ft->req[i].headers[h], 160, "%s: %s", q->headers[h].name, q->headers[h].value);
    ft->req[i].body_len = q->body_len;
    if (q->body && q->body_len < sizeof ft->req[i].body)
        { memcpy(ft->req[i].body, q->body, q->body_len); ft->req[i].body[q->body_len] = 0; }
    snprintf(ft->req[i].body_file, 128, "%s", q->body_file ? q->body_file : "");
    snprintf(ft->req[i].sink_file, 128, "%s", q->sink_file ? q->sink_file : "");
    ft->req[i].timeout_ms = q->timeout_ms;

    if (ft->resp_next >= ft->resp_count) { r->transport_err = 1; return -1; }
    int j = ft->resp_next++;
    r->transport_err = ft->resp[j].transport_err;
    r->status = ft->resp[j].status;
    r->body = ft->resp[j].body;
    r->body_len = strlen(ft->resp[j].body);
    /* sink_file: the real transport writes the body to disk; the fake does too */
    if (q->sink_file && !r->transport_err && r->status == 200) {
        FILE *f = fopen(q->sink_file, "wb");
        if (f) { fwrite(r->body, 1, r->body_len, f); fclose(f); }
    }
    return r->transport_err ? -1 : 0;
}

void ft_init(fake_transport_t *ft, htp_transport_t *out) {
    memset(ft, 0, sizeof *ft);
    out->perform = ft_perform; out->ctx = ft;
}

void ft_push(fake_transport_t *ft, int terr, int status, const char *body) {
    int j = ft->resp_count++;
    ft->resp[j].transport_err = terr; ft->resp[j].status = status;
    snprintf(ft->resp[j].body, sizeof ft->resp[j].body, "%s", body ? body : "");
}

void ft_push_fixture(fake_transport_t *ft, int status, const char *name) {
    char path[512]; snprintf(path, sizeof path, "%s/%s", FIXDIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "missing fixture %s\n", path); ft_push(ft, 1, 0, ""); return; }
    int j = ft->resp_count++;
    ft->resp[j].transport_err = 0; ft->resp[j].status = status;
    size_t n = fread(ft->resp[j].body, 1, sizeof ft->resp[j].body - 1, f);
    ft->resp[j].body[n] = 0; fclose(f);
}

const char *ft_find_header(fake_transport_t *ft, int i, const char *name) {
    size_t nl = strlen(name);
    for (int h = 0; h < ft->req[i].header_count; h++)
        if (!strncmp(ft->req[i].headers[h], name, nl) && ft->req[i].headers[h][nl] == ':')
            return ft->req[i].headers[h] + nl + 2;
    return NULL;
}
