#include "htp_client.h"
#include "util.h"
#include "cJSON.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define HTP_TIMEOUT_MS       15000
#define HTP_UPLOAD_TIMEOUT_MS 60000   /* 3.8 MB over weak Wi-Fi */

/* Appends printf-formatted text at buf[*pos] within capacity cap, advancing *pos
 * by exactly the number of bytes written (never past cap - 1, and never leaving
 * *pos >= cap). Returns 0 on success. Returns -1 if the formatted text would not
 * fit; on failure *pos is left unchanged and buf's content beyond the previous
 * *pos is unspecified (snprintf may have written a truncated fragment there, but
 * it is never read because callers abort the whole request on -1). */
static int append_fmt(char *buf, size_t cap, size_t *pos, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static int append_fmt(char *buf, size_t cap, size_t *pos, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf + *pos, cap - *pos, fmt, ap);
    va_end(ap);
    if (ret < 0 || (size_t)ret >= cap - *pos) return -1;
    *pos += (size_t)ret;
    return 0;
}

void htp_client_init(htp_client_t *c, htp_transport_t *t, const char *token) {
    c->transport = t; c->token = token; c->battery_pct = -1;
}

static int status_to_err(const htp_response_t *r) {
    if (r->transport_err) return HTP_ERR_NETWORK;
    if (r->status == 401) return HTP_ERR_AUTH;
    if (r->status >= 500) return HTP_ERR_SERVER;
    if (r->status >= 400) return HTP_ERR_CLIENT;
    return HTP_OK;
}

/* Adds Authorization and X-Battery, performs, maps errors.
 * On HTP_OK, *out_json holds the parsed body (caller must cJSON_Delete),
 * unless req->sink_file was set, in which case out_json is untouched. */
static int perform(htp_client_t *c, htp_request_t *req, cJSON **out_json) {
    static char battery[12];   /* fits any int %d (incl. sign) + NUL; -Wformat-truncation on the IDF cross-compiler needs the full range provable, not just the 0-100 domain */
    char auth[192];
    snprintf(auth, sizeof auth, "Bearer %s", c->token);
    req->headers[req->header_count++] = (htp_header_t){ "Authorization", auth };
    if (c->battery_pct >= 0) {
        snprintf(battery, sizeof battery, "%d", c->battery_pct);
        req->headers[req->header_count++] = (htp_header_t){ "X-Battery", battery };
    }
    if (req->timeout_ms <= 0) req->timeout_ms = HTP_TIMEOUT_MS;

    htp_response_t resp = {0};
    c->transport->perform(c->transport->ctx, req, &resp);
    int err = status_to_err(&resp);
    if (err != HTP_OK) return err;
    if (req->sink_file) return HTP_OK;
    cJSON *j = cJSON_ParseWithLength(resp.body, resp.body_len);
    if (!j) return HTP_ERR_PROTO;
    *out_json = j;
    return HTP_OK;
}

static void get_str(cJSON *obj, const char *key, char *dst, size_t cap) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    str_copy(dst, cap, cJSON_IsString(v) ? v->valuestring : "");
}

static long long get_num(cJSON *obj, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? (long long)v->valuedouble : 0;
}

static int get_bool(cJSON *obj, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsTrue(v) ? 1 : 0;
}

static htp_capture_state_t parse_state(const char *s) {
    if (!strcmp(s, "received"))     return HTP_ST_RECEIVED;
    if (!strcmp(s, "transcribing")) return HTP_ST_TRANSCRIBING;
    if (!strcmp(s, "processing"))   return HTP_ST_PROCESSING;
    if (!strcmp(s, "done"))         return HTP_ST_DONE;
    if (!strcmp(s, "reply_ready"))  return HTP_ST_REPLY_READY;
    if (!strcmp(s, "failed"))       return HTP_ST_FAILED;
    return HTP_ST_UNKNOWN;
}

int htp_upload_capture(htp_client_t *c, const htp_upload_params_t *p) {
    char recorded[24];
    htp_request_t req = { .method = "POST", .path = "/htp/v1/captures",
        .content_type = "audio/wav", .body_file = p->wav_path,
        .timeout_ms = HTP_UPLOAD_TIMEOUT_MS };
    req.headers[req.header_count++] = (htp_header_t){ "X-Capture-Id", p->capture_id };
    req.headers[req.header_count++] = (htp_header_t){ "X-Capture-Mode", "auto" };
    if (p->recorded_at > 0) {
        snprintf(recorded, sizeof recorded, "%lld", p->recorded_at);
        req.headers[req.header_count++] = (htp_header_t){ "X-Recorded-At", recorded };
    }
    if (p->conversation_id && p->conversation_id[0])
        req.headers[req.header_count++] = (htp_header_t){ "X-Conversation-Id", p->conversation_id };

    cJSON *j = NULL;
    int err = perform(c, &req, &j);
    if (err != HTP_OK) return err;
    char id[64]; get_str(j, "id", id, sizeof id);
    int ok = !strcmp(id, p->capture_id);
    cJSON_Delete(j);
    return ok ? HTP_OK : HTP_ERR_PROTO;
}

int htp_poll_captures(htp_client_t *c, const char *const ids[], int n,
                      htp_capture_status_t out[], int max_out, long long *server_time) {
    char path[2560];   /* 32 ids x 64 chars + separators must fit */
    size_t pos = 0;
    if (append_fmt(path, sizeof path, &pos, "/htp/v1/captures?ids=") != 0)
        return HTP_ERR_CLIENT;
    for (int i = 0; i < n; i++)
        if (append_fmt(path, sizeof path, &pos, "%s%s", i ? "," : "", ids[i]) != 0)
            return HTP_ERR_CLIENT;
    htp_request_t req = { .method = "GET", .path = path };
    cJSON *j = NULL;
    int err = perform(c, &req, &j);
    if (err != HTP_OK) return err;
    cJSON *st = cJSON_GetObjectItemCaseSensitive(j, "server_time");
    if (server_time) *server_time = cJSON_IsNumber(st) ? (long long)st->valuedouble : 0;
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(j, "captures");
    int count = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (count >= max_out) break;
        htp_capture_status_t *s = &out[count++];
        char state[24];
        get_str(it, "id", s->id, sizeof s->id);
        get_str(it, "state", state, sizeof state);
        s->state = parse_state(state);
        get_str(it, "transcript", s->transcript, sizeof s->transcript);
        get_str(it, "conversation_id", s->conversation_id, sizeof s->conversation_id);
        get_str(it, "error", s->error, sizeof s->error);
    }
    cJSON_Delete(j);
    return count;
}

int htp_download_reply(htp_client_t *c, const char *capture_id, const char *dest_path) {
    char path[160];
    snprintf(path, sizeof path, "/htp/v1/captures/%s/reply.wav", capture_id);
    htp_request_t req = { .method = "GET", .path = path, .sink_file = dest_path };
    cJSON *j = NULL;
    return perform(c, &req, &j);
}

int htp_get_dashboard(htp_client_t *c, const char *rev, htp_dashboard_t *out) {
    char path[128];
    if (rev && rev[0])
        snprintf(path, sizeof path, "/htp/v1/dashboard?rev=%s", rev);
    else
        snprintf(path, sizeof path, "/htp/v1/dashboard");
    htp_request_t req = { .method = "GET", .path = path };
    cJSON *j = NULL;
    int err = perform(c, &req, &j);
    if (err != HTP_OK) return err;

    out->unchanged = get_bool(j, "unchanged");
    get_str(j, "rev", out->rev, sizeof out->rev);
    get_str(j, "title", out->title, sizeof out->title);
    out->server_time = get_num(j, "server_time");
    out->sync_interval = (int)get_num(j, "sync_interval");

    cJSON *items = cJSON_GetObjectItemCaseSensitive(j, "items");
    int count = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, items) {
        if (count >= 32) break;
        htp_dash_item_t *d = &out->items[count++];
        get_str(it, "id", d->id, sizeof d->id);
        get_str(it, "text", d->text, sizeof d->text);
        d->done = get_bool(it, "done");
        get_str(it, "style", d->style, sizeof d->style);
    }
    out->item_count = count;

    cJSON_Delete(j);
    return HTP_OK;
}

int htp_complete_item(htp_client_t *c, const char *item_id, char rev_out[24]) {
    char body[128];
    size_t pos = 0;
    if (append_fmt(body, sizeof body, &pos, "{\"item_id\":\"%s\"}", item_id) != 0)
        return HTP_ERR_CLIENT;
    htp_request_t req = { .method = "POST", .path = "/htp/v1/complete",
        .content_type = "application/json",
        .body = (const uint8_t *)body, .body_len = pos };
    cJSON *j = NULL;
    int err = perform(c, &req, &j);
    if (err != HTP_OK) return err;
    int ok = get_bool(j, "ok");
    if (ok) get_str(j, "rev", rev_out, 24);
    cJSON_Delete(j);
    return ok ? HTP_OK : HTP_ERR_PROTO;
}

int htp_get_notifications(htp_client_t *c, htp_notifications_t *out) {
    htp_request_t req = { .method = "GET", .path = "/htp/v1/notifications" };
    cJSON *j = NULL;
    int err = perform(c, &req, &j);
    if (err != HTP_OK) return err;
    out->server_time = get_num(j, "server_time");
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(j, "notifications");
    int count = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (count >= 16) break;
        htp_notification_t *nn = &out->items[count++];
        get_str(it, "id", nn->id, sizeof nn->id);
        get_str(it, "text", nn->text, sizeof nn->text);
        char priority[16];
        get_str(it, "priority", priority, sizeof priority);
        nn->urgent = !strcmp(priority, "urgent");
        nn->created = get_num(it, "created");
    }
    out->count = count;
    cJSON_Delete(j);
    return HTP_OK;
}

int htp_ack_notifications(htp_client_t *c, const char *const ids[], int n) {
    char body[1024];
    size_t pos = 0;
    if (append_fmt(body, sizeof body, &pos, "{\"ids\":[") != 0) return HTP_ERR_CLIENT;
    for (int i = 0; i < n; i++)
        if (append_fmt(body, sizeof body, &pos, "%s\"%s\"", i ? "," : "", ids[i]) != 0)
            return HTP_ERR_CLIENT;
    if (append_fmt(body, sizeof body, &pos, "]}") != 0) return HTP_ERR_CLIENT;
    htp_request_t req = { .method = "POST", .path = "/htp/v1/notifications/ack",
        .content_type = "application/json",
        .body = (const uint8_t *)body, .body_len = pos };
    cJSON *j = NULL;
    int err = perform(c, &req, &j);
    if (err != HTP_OK) return err;
    int ok = get_bool(j, "ok");
    cJSON_Delete(j);
    return ok ? HTP_OK : HTP_ERR_PROTO;
}
