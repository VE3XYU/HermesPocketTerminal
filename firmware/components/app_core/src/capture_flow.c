#include "capture_flow.h"
#include "htp_backoff.h"
#include "rec_index.h"
#include "util.h"
#include <string.h>

static void status(capture_ctx_t *cx, const char *line) {
    if (cx->on_status) cx->on_status(cx->ui_ctx, line);
}

static int upload_once(capture_ctx_t *cx, sidecar_t *sc) {
    char wav[96]; sidecar_wav_path(wav, sc->id);
    htp_upload_params_t p = { .capture_id = sc->id, .wav_path = wav,
        .recorded_at = sc->recorded_at,
        .conversation_id = sc->conversation_id[0] ? sc->conversation_id : NULL };
    return htp_upload_capture(cx->client, &p);
}

/* 3 attempts, backoff between; HTP_OK / HTP_ERR_AUTH / HTP_ERR_CLIENT end early */
static int upload_with_retry(capture_ctx_t *cx, sidecar_t *sc) {
    htp_backoff_t b; htp_backoff_init(&b, 1000, 30000);
    int err = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt) cx->clock->sleep_ms(cx->clock->ctx, htp_backoff_next(&b));
        err = upload_once(cx, sc);
        if (err == HTP_OK || err == HTP_ERR_AUTH || err == HTP_ERR_CLIENT) return err;
    }
    return err;
}

capture_outcome_t capture_run(capture_ctx_t *cx, sidecar_t *sc) {
    if (strcmp(sc->state, "not_uploaded") == 0) {
        status(cx, "Uploading...");
        int err = upload_with_retry(cx, sc);
        if (err == HTP_ERR_AUTH) { status(cx, "Auth error"); return CAPTURE_AUTH_ERROR; }
        if (err != HTP_OK && err != HTP_ERR_CLIENT) {
            status(cx, "Saved, will upload later");
            return CAPTURE_OFFLINE;
        }
        if (err == HTP_ERR_CLIENT) { status(cx, "Upload rejected"); return CAPTURE_FAILED; }
        str_copy(sc->state, sizeof sc->state, "uploaded");
        sc->uploaded_at = cx->clock->epoch_s(cx->clock->ctx);
        sidecar_save(cx->storage, sc);
        status(cx, "Uploaded");
    }

    unsigned start = cx->clock->mono_ms(cx->clock->ctx);
    const char *ids[1] = { sc->id };
    while (cx->clock->mono_ms(cx->clock->ctx) - start < cx->poll_window_ms) {
        htp_capture_status_t stat; long long t;
        int n = htp_poll_captures(cx->client, ids, 1, &stat, 1, &t);
        if (n == HTP_ERR_AUTH) return CAPTURE_AUTH_ERROR;
        if (n == 1) {
            if (stat.transcript[0])
                str_copy(sc->transcript, sizeof sc->transcript, stat.transcript);
            if (stat.conversation_id[0])
                str_copy(sc->conversation_id, sizeof sc->conversation_id, stat.conversation_id);
            if (stat.state == HTP_ST_DONE) {
                str_copy(sc->state, sizeof sc->state, "done");
                sidecar_save(cx->storage, sc);
                status(cx, "Noted");
                return CAPTURE_DONE;
            }
            if (stat.state == HTP_ST_REPLY_READY) {
                if (htp_download_reply(cx->client, sc->id, cx->reply_path) == HTP_OK) {
                    str_copy(sc->state, sizeof sc->state, "reply_ready");
                    sidecar_save(cx->storage, sc);
                    return CAPTURE_REPLY_READY;
                } /* download failed: keep polling, window still bounds us */
            }
            if (stat.state == HTP_ST_FAILED) {
                str_copy(sc->state, sizeof sc->state, "failed");
                str_copy(sc->error, sizeof sc->error, stat.error);
                sidecar_save(cx->storage, sc);
                status(cx, "Failed");
                return CAPTURE_FAILED;
            }
            if (stat.state == HTP_ST_UNKNOWN) {
                /* bridge lost it: mark for re-upload, bail to retry path */
                str_copy(sc->state, sizeof sc->state, "not_uploaded");
                sidecar_save(cx->storage, sc);
                return CAPTURE_OFFLINE;
            }
        }
        cx->clock->sleep_ms(cx->clock->ctx, cx->poll_interval_ms);
    }
    sidecar_save(cx->storage, sc);
    return CAPTURE_TIMEOUT;
}

/* Retry-scan batch buffers: static, not stack -- the id list (2 KB) plus
 * a sidecar_t (1.2 KB) made capture_retry_pending a 3.3 KB frame, first
 * on the stack in every sync cycle (C5 stack ruling; see rec_index.c).
 * Single main task, fully re-populated per call. */
static char s_retry_ids[32][64];
static sidecar_t s_retry_sc;

int capture_retry_pending(capture_ctx_t *cx) {
    int n = rec_index_list(cx->storage, s_retry_ids, 32);
    int confirmed = 0;
    for (int i = 0; i < n; i++) {
        sidecar_t *sc = &s_retry_sc;
        if (sidecar_load(cx->storage, s_retry_ids[i], sc) != 0) continue;
        if (strcmp(sc->state, "not_uploaded") != 0) continue;
        if (upload_once(cx, sc) == HTP_OK) {
            str_copy(sc->state, sizeof sc->state, "uploaded");
            sc->uploaded_at = cx->clock->epoch_s(cx->clock->ctx);
            sidecar_save(cx->storage, sc);
            confirmed++;
        }
    }
    return confirmed;
}
