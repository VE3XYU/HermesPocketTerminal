#include "sidecar.h"
#include "util.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void get_str(cJSON *obj, const char *key, char *dst, size_t cap) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    str_copy(dst, cap, cJSON_IsString(v) ? v->valuestring : "");
}

static long long get_num(cJSON *obj, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? (long long)v->valuedouble : 0;
}

void sidecar_init(sidecar_t *sc, const char *capture_id) {
    memset(sc, 0, sizeof *sc);
    str_copy(sc->id, sizeof sc->id, capture_id);
    str_copy(sc->state, sizeof sc->state, "not_uploaded");
}

int sidecar_save(port_storage_t *st, const sidecar_t *sc) {
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return -1;

    cJSON_AddStringToObject(obj, "id", sc->id);
    cJSON_AddStringToObject(obj, "state", sc->state);
    cJSON_AddStringToObject(obj, "transcript", sc->transcript);
    cJSON_AddStringToObject(obj, "conversation_id", sc->conversation_id);
    cJSON_AddStringToObject(obj, "error", sc->error);
    cJSON_AddNumberToObject(obj, "recorded_at", (double)sc->recorded_at);
    cJSON_AddNumberToObject(obj, "uploaded_at", (double)sc->uploaded_at);

    char *json_str = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!json_str) return -1;

    char path[96];
    sidecar_path(path, sc->id);
    size_t len = strlen(json_str);
    int ret = st->write(st->ctx, path, (const uint8_t *)json_str, len);

    free(json_str);
    return ret;
}

/* JSON read buffer for sidecar_load(): static, not stack -- a 4 KB local
 * put a 4.2 KB frame on every load call, stacked under the sync and
 * render flows (C5 stack ruling; see rec_index.c). Single main task,
 * fully re-read per call. */
static char s_json_buf[4096];

int sidecar_load(port_storage_t *st, const char *capture_id, sidecar_t *out) {
    memset(out, 0, sizeof *out);

    char path[96];
    sidecar_path(path, capture_id);

    char *const buf = s_json_buf;
    size_t len = 0;
    if (st->read(st->ctx, path, buf, sizeof s_json_buf - 1, &len) != 0)
        return -1;

    buf[len] = 0;
    cJSON *obj = cJSON_Parse(buf);
    if (!obj) return -1;

    get_str(obj, "id", out->id, sizeof out->id);
    get_str(obj, "state", out->state, sizeof out->state);
    get_str(obj, "transcript", out->transcript, sizeof out->transcript);
    get_str(obj, "conversation_id", out->conversation_id, sizeof out->conversation_id);
    get_str(obj, "error", out->error, sizeof out->error);
    out->recorded_at = get_num(obj, "recorded_at");
    out->uploaded_at = get_num(obj, "uploaded_at");

    cJSON_Delete(obj);
    return 0;
}

void sidecar_path(char out[96], const char *capture_id) {
    snprintf(out, 96, "/rec/%s.json", capture_id);
}

void sidecar_wav_path(char out[96], const char *capture_id) {
    snprintf(out, 96, "/rec/%s.wav", capture_id);
}
