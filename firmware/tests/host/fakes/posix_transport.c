#include "posix_transport.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int read_all(int fd, char *buf, size_t cap) {
    size_t got = 0; ssize_t n;
    while (got < cap - 1 && (n = read(fd, buf + got, cap - 1 - got)) > 0) got += (size_t)n;
    buf[got] = 0; return (int)got;
}

static int pt_perform(void *ctx, const htp_request_t *q, htp_response_t *r) {
    posix_transport_t *pt = ctx;
    memset(r, 0, sizeof *r);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { r->transport_err = 1; return -1; }
    struct timeval tv;
    tv.tv_sec = q->timeout_ms / 1000;
    tv.tv_usec = (q->timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)pt->port);
    if (inet_pton(AF_INET, pt->host, &addr.sin_addr) != 1) {
        close(fd); r->transport_err = 1; return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd); r->transport_err = 1; return -1;
    }
    /* --- request head --- */
    char head[2048]; int hl = 0;
    /* body from file? load it (uploads are ≤ 3.8 MB; tests use tiny WAVs) */
    char *fbody = NULL; size_t fblen = 0;
    if (q->body_file) {
        FILE *f = fopen(q->body_file, "rb");
        if (!f) { close(fd); r->transport_err = 1; return -1; }
        fseek(f, 0, SEEK_END); fblen = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
        fbody = malloc(fblen ? fblen : 1);
        fread(fbody, 1, fblen, f); fclose(f);
    }
    size_t blen = q->body_file ? fblen : q->body_len;
    hl += snprintf(head + hl, sizeof head - hl, "%s %s HTTP/1.1\r\nHost: %s\r\n",
                   q->method, q->path, pt->host);
    for (int i = 0; i < q->header_count; i++)
        hl += snprintf(head + hl, sizeof head - hl, "%s: %s\r\n",
                       q->headers[i].name, q->headers[i].value);
    if (q->content_type)
        hl += snprintf(head + hl, sizeof head - hl, "Content-Type: %s\r\n", q->content_type);
    hl += snprintf(head + hl, sizeof head - hl,
                   "Content-Length: %zu\r\nConnection: close\r\n\r\n", blen);
    write(fd, head, (size_t)hl);
    if (blen) write(fd, q->body_file ? fbody : (const char *)q->body, blen);
    free(fbody);

    /* --- response --- */
    int total = read_all(fd, pt->body, sizeof pt->body);
    close(fd);
    if (total <= 0 || sscanf(pt->body, "HTTP/1.%*c %d", &r->status) != 1) {
        r->transport_err = 1; return -1;
    }
    char *sep = strstr(pt->body, "\r\n\r\n");
    if (!sep) { r->transport_err = 1; return -1; }
    r->body = sep + 4;
    r->body_len = (size_t)(total - (sep + 4 - pt->body));
    if (q->sink_file && r->status == 200) {
        FILE *f = fopen(q->sink_file, "wb");
        if (f) { fwrite(r->body, 1, r->body_len, f); fclose(f); }
    }
    return 0;
}

void posix_transport_init(posix_transport_t *pt, htp_transport_t *out,
                          const char *host, int port) {
    snprintf(pt->host, sizeof pt->host, "%s", host);
    pt->port = port;
    out->perform = pt_perform; out->ctx = pt;
}
