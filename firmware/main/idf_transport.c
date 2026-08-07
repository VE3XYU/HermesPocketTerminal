/* esp_http_client binding for Task 3's htp_transport_t. See idf_transport.h. */
#include "idf_transport.h"
#include "idf_ports.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_err.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "idf_http";

/* htp_client.h promises resp->body stays valid until the next perform(),
 * and every caller is the single main task, so one static buffer set is
 * both sufficient and the only way to keep 16 KB off the task stack. */
#define RESP_CAP  16384
#define URL_CAP   2816     /* base_url (<=128) + the longest client path:
                              htp_poll_captures builds a 2560-byte query */
#define CHUNK_CAP 4096     /* SD <-> socket streaming granularity */

static char s_resp[RESP_CAP];
static char s_chunk[CHUNK_CAP];
static char s_url[URL_CAP];
static char s_base[128];

static int is_https(const char *url) { return strncmp(url, "https://", 8) == 0; }

static int method_of(const char *m, esp_http_client_method_t *out) {
    if (!m) return -1;
    if (!strcmp(m, "GET"))  { *out = HTTP_METHOD_GET;  return 0; }
    if (!strcmp(m, "POST")) { *out = HTTP_METHOD_POST; return 0; }
    return -1;
}

/* FatFs's f_rename() refuses to replace an existing destination (see the
 * recovery contract in idf_ports.c), so a re-download of the same reply
 * would fail on the rename without this. The .part file is already whole
 * and fclose()'d here, so it -- not the stale destination -- is the copy
 * worth keeping if the retry loses power midway. */
static int rename_replacing(const char *from, const char *to) {
    if (rename(from, to) == 0) return 0;
    remove(to);
    return rename(from, to) == 0 ? 0 : -1;
}

/* Streams req->body_file to the socket in CHUNK_CAP pieces. */
static int send_file_body(esp_http_client_handle_t cl, FILE *f, long size) {
    long sent = 0;
    while (sent < size) {
        size_t want = (size - sent) < (long)CHUNK_CAP ? (size_t)(size - sent) : CHUNK_CAP;
        size_t got = fread(s_chunk, 1, want, f);
        if (got == 0) {
            ESP_LOGE(TAG, "upload body short at %ld/%ld bytes", sent, size);
            return -1;
        }
        int w = esp_http_client_write(cl, s_chunk, (int)got);
        if (w != (int)got) {
            ESP_LOGE(TAG, "upload write failed at %ld/%ld bytes (w=%d)", sent, size, w);
            return -1;
        }
        sent += (long)got;
    }
    return 0;
}

/* Inline body or streamed file body, whichever the request carries. */
static int send_body(esp_http_client_handle_t cl, const htp_request_t *req, FILE *f, long len) {
    if (len <= 0) return 0;
    if (f) return send_file_body(cl, f, len);
    return esp_http_client_write(cl, (const char *)req->body, (int)len) == (int)len ? 0 : -1;
}

/* Streams the response body to "<sink>.part", promoting it to <sink> only
 * once the connection has delivered everything it had. */
static int recv_to_file(esp_http_client_handle_t cl, const char *sink_logical) {
    char final_path[IDF_SD_PATH_MAX];
    char part_path[IDF_SD_PATH_MAX + 8];
    if (idf_ports_sd_path(sink_logical, final_path, sizeof final_path) != 0) return -1;
    if ((size_t)snprintf(part_path, sizeof part_path, "%s.part", final_path) >= sizeof part_path)
        return -1;

    FILE *f = fopen(part_path, "wb");
    if (!f) { ESP_LOGE(TAG, "cannot open %s", part_path); return -1; }

    long total = 0;
    for (;;) {
        int rd = esp_http_client_read(cl, s_chunk, CHUNK_CAP);
        if (rd < 0) { ESP_LOGE(TAG, "download read failed at %ld bytes", total); break; }
        if (rd == 0) {
            if (fclose(f) != 0) { ESP_LOGE(TAG, "close %s failed", part_path); f = NULL; break; }
            if (rename_replacing(part_path, final_path) != 0) {
                ESP_LOGE(TAG, "cannot promote %s", part_path);
                remove(part_path);
                return -1;
            }
            ESP_LOGI(TAG, "downloaded %ld bytes to %s", total, sink_logical);
            return 0;
        }
        if (fwrite(s_chunk, 1, (size_t)rd, f) != (size_t)rd) {
            ESP_LOGE(TAG, "SD write failed at %ld bytes", total);
            break;
        }
        total += rd;
    }
    if (f) fclose(f);
    remove(part_path);   /* a partial reply is worse than no reply */
    return -1;
}

/* Buffers a JSON response into s_resp, always NUL-terminated. */
static size_t recv_to_buffer(esp_http_client_handle_t cl, int *err) {
    size_t total = 0;
    *err = 0;
    while (total < RESP_CAP - 1) {
        int rd = esp_http_client_read(cl, s_resp + total, (int)(RESP_CAP - 1 - total));
        if (rd < 0) { *err = 1; break; }
        if (rd == 0) break;
        total += (size_t)rd;
    }
    if (total >= RESP_CAP - 1)
        ESP_LOGW(TAG, "response exceeded %d bytes and was truncated", RESP_CAP - 1);
    s_resp[total] = 0;
    return total;
}

static int tr_perform(void *ctx, const htp_request_t *req, htp_response_t *resp) {
    (void)ctx;
    memset(resp, 0, sizeof *resp);
    s_resp[0] = 0;
    resp->body = s_resp;   /* never NULL, even on a failure path */
    resp->transport_err = 1;   /* cleared once a status code is in hand */

    esp_http_client_method_t method;
    if (!req->path || method_of(req->method, &method) != 0) {
        ESP_LOGE(TAG, "unsupported request (%s %s)", req->method ? req->method : "(null)",
                 req->path ? req->path : "(null)");
        return -1;
    }
    /* Mandatory per htp_client.h; enforced rather than defaulted so a
     * missing timeout can never turn into an unbounded network wait. */
    if (req->timeout_ms <= 0) {
        ESP_LOGE(TAG, "request without a timeout: %s", req->path);
        return -1;
    }
    if ((size_t)snprintf(s_url, sizeof s_url, "%s%s", s_base, req->path) >= sizeof s_url) {
        ESP_LOGE(TAG, "URL too long for %s", req->path);
        return -1;
    }

    FILE *body_f = NULL;
    long body_len = 0;
    if (req->body_file) {
        char fp[IDF_SD_PATH_MAX];
        if (idf_ports_sd_path(req->body_file, fp, sizeof fp) != 0) return -1;
        body_f = fopen(fp, "rb");
        if (!body_f) { ESP_LOGE(TAG, "cannot open %s", fp); return -1; }
        if (fseek(body_f, 0, SEEK_END) != 0 || (body_len = ftell(body_f)) < 0 ||
            fseek(body_f, 0, SEEK_SET) != 0) {
            ESP_LOGE(TAG, "cannot size %s", fp);
            fclose(body_f);
            return -1;
        }
    } else if (req->body) {
        body_len = (long)req->body_len;
    }

    esp_http_client_config_t cfg = {
        .url = s_url,
        .method = method,
        .timeout_ms = req->timeout_ms,
        /* http:// is deliberately honored as-is for LAN development against
         * the mock bridge (design §5.5); https:// validates against the
         * built-in root bundle. */
        .crt_bundle_attach = is_https(s_url) ? esp_crt_bundle_attach : NULL,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) {
        if (body_f) fclose(body_f);
        return -1;
    }

    for (int i = 0; i < req->header_count; i++)
        esp_http_client_set_header(cl, req->headers[i].name, req->headers[i].value);
    if (req->content_type)
        esp_http_client_set_header(cl, "Content-Type", req->content_type);

    int ok = 0;
    esp_err_t e = esp_http_client_open(cl, (int)body_len);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "%s %s: %s", req->method, req->path, esp_err_to_name(e));
    } else if (send_body(cl, req, body_f, body_len) != 0) {
        ESP_LOGE(TAG, "%s %s: sending the body failed", req->method, req->path);
    } else if (esp_http_client_fetch_headers(cl) < 0) {
        ESP_LOGE(TAG, "%s %s: no response headers", req->method, req->path);
    } else {
        ok = 1;
    }
    if (body_f) fclose(body_f);

    if (ok) {
        resp->status = esp_http_client_get_status_code(cl);
        resp->transport_err = 0;
        /* An error status still carries a JSON {"error": ...} body worth
         * buffering; only a 200 gets written to the sink file. */
        if (req->sink_file && resp->status == 200) {
            if (recv_to_file(cl, req->sink_file) != 0) {
                resp->transport_err = 1;
                resp->status = 0;
            }
        } else {
            int rerr = 0;
            resp->body_len = recv_to_buffer(cl, &rerr);
            if (rerr) {
                ESP_LOGE(TAG, "%s %s: reading the response failed", req->method, req->path);
                resp->transport_err = 1;
                resp->status = 0;
                resp->body_len = 0;
            }
        }
    }

    esp_http_client_close(cl);
    esp_http_client_cleanup(cl);
    return resp->transport_err ? -1 : 0;
}

void idf_transport_init(htp_transport_t *out, const char *base_url) {
    snprintf(s_base, sizeof s_base, "%s", base_url ? base_url : "");
    out->perform = tr_perform;
    out->ctx = NULL;
}
