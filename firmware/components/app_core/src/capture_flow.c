#include "capture_flow.h"
#include "htp_backoff.h"
#include "rec_index.h"
#include "util.h"
#include "wav.h"
#include <string.h>

static void status(capture_ctx_t *cx, const char *line) {
    if (cx->on_status) cx->on_status(cx->ui_ctx, line);
}

/* Design §10's torn-file rule: the recorder writes a ZEROED 44-byte header
 * and patches it only on a clean stop, so a WAV that lost power
 * mid-recording (or was bit-rotted on the card) fails wav_parse_header --
 * and must be REFUSED by upload rather than streamed to the bridge as
 * audio. Only the first 64 bytes are read: every device-recorded file has
 * the canonical 44-byte header, and only device-recorded files are ever
 * uploaded. An UNREADABLE file is not refused here (return 0, fail OPEN):
 * a transient card-busy/I-O read failure must not permanently discard a
 * genuine recording just because the read happened to fail on this one
 * attempt -- that is a storage/transport failure with its own reporting,
 * and the transport surfaces it (and gets to retry) on the upload attempt
 * that follows. On refusal the sidecar is marked
 * failed/bad_wav_header and saved, so the capture leaves the pending
 * queue instead of being refused again on every future sync; the WAV
 * stays on the card. Returns 1 = refused. */
static int wav_refused(capture_ctx_t *cx, sidecar_t *sc) {
    char wav[96];
    sidecar_wav_path(wav, sc->id);
    uint8_t head[64];
    size_t got = 0;
    if (cx->storage->read(cx->storage->ctx, wav, head, sizeof head, &got) != 0)
        return 0;
    wav_info_t inf;
    if (wav_parse_header(head, got, &inf) == 0) return 0;
    str_copy(sc->state, sizeof sc->state, "failed");
    str_copy(sc->error, sizeof sc->error, "bad_wav_header");
    sidecar_save(cx->storage, sc);
    return 1;
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
        if (wav_refused(cx, sc)) {
            /* the on_status line doubles as the serial note (main.c logs
             * every status callback) */
            status(cx, "Recording unreadable - not uploaded");
            return CAPTURE_FAILED;
        }
        status(cx, "Uploading...");
        int err = upload_with_retry(cx, sc);
        if (err == HTP_ERR_AUTH) { status(cx, "Auth error"); return CAPTURE_AUTH_ERROR; }
        if (err != HTP_OK && err != HTP_ERR_CLIENT) {
            status(cx, "Saved, will upload later");
            return CAPTURE_OFFLINE;
        }
        if (err == HTP_ERR_CLIENT) {
            /* the bridge rejected THIS capture (status_to_err maps a real
             * 4xx response here -- htp_upload_capture has no local path
             * that returns HTP_ERR_CLIENT, unlike htp_poll_captures/
             * htp_complete_item/htp_ack_notifications' buffer-build
             * checks), so retrying would only get the same 4xx again.
             * Drain it out of the pending queue the same way wav_refused
             * does above, instead of leaving it not_uploaded forever --
             * without this the whole WAV re-streams on every sync. */
            str_copy(sc->state, sizeof sc->state, "failed");
            str_copy(sc->error, sizeof sc->error, "upload_rejected");
            sidecar_save(cx->storage, sc);
            status(cx, "Upload rejected");
            return CAPTURE_FAILED;
        }
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

/* Final review, Important 1: the retry scan and the pending count used to
 * read only the newest 32 index entries -- but the window is by RECENCY,
 * not by pending state, so capture #33+ of an offline backlog stayed
 * not_uploaded forever while the status header reported 0 pending. Both
 * now page through the whole retained index (rec_index_list_page walks
 * skip = 0, 32, 64... until a short page), reusing the same 32-slot batch
 * statics -- no new memory. The scan is unbounded; the upload WORK per
 * wake is not: RETRY_UPLOAD_BUDGET caps upload attempts (each can cost a
 * full transport timeout) at the old implicit bound, so a huge backlog
 * drains across wakes instead of wedging one against the awake watchdog.
 * Successful uploads leave the pending set, so the next wake's pages reach
 * the next-newest 32 pending -- every retained capture uploads eventually. */
#define RETRY_PAGE          32   /* ids per page = the batch statics' size */
#define RETRY_UPLOAD_BUDGET 32   /* upload attempts per call (per wake) */

int capture_retry_pending(capture_ctx_t *cx) {
    int confirmed = 0, attempts = 0;
    for (int skip = 0; ; skip += RETRY_PAGE) {
        int n = rec_index_list_page(cx->storage, s_retry_ids, RETRY_PAGE, skip);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            sidecar_t *sc = &s_retry_sc;
            if (sidecar_load(cx->storage, s_retry_ids[i], sc) != 0) continue;
            if (strcmp(sc->state, "not_uploaded") != 0) continue;
            if (wav_refused(cx, sc)) continue;   /* torn header: now failed on
                                                    the card, out of the queue */
            if (attempts >= RETRY_UPLOAD_BUDGET) return confirmed;
            attempts++;
            int err = upload_once(cx, sc);
            if (err == HTP_OK) {
                str_copy(sc->state, sizeof sc->state, "uploaded");
                sc->uploaded_at = cx->clock->epoch_s(cx->clock->ctx);
                sidecar_save(cx->storage, sc);
                confirmed++;
            } else if (err == HTP_ERR_CLIENT) {
                /* Same guarantee as capture_run's HTP_ERR_CLIENT branch (see
                 * the trace there): upload_once -> htp_upload_capture can only
                 * return HTP_ERR_CLIENT from a real server 4xx, never a local
                 * fault, so retrying gets the same rejection forever. Reached
                 * when a capture went not_uploaded via a genuine offline/server
                 * failure and is THEN rejected on a later retry (capture_run's
                 * own upload already drains this on the first attempt; this is
                 * the same poison-pill class on the second-chance path). Drain
                 * it the same way instead of re-streaming the WAV every sync. */
                str_copy(sc->state, sizeof sc->state, "failed");
                str_copy(sc->error, sizeof sc->error, "upload_rejected");
                sidecar_save(cx->storage, sc);
            }
            /* HTP_ERR_AUTH / SERVER / NETWORK / PROTO: sc is left untouched,
             * so it stays not_uploaded and is retried on the next sync --
             * unchanged from before this fix. */
        }
        if (n < RETRY_PAGE) break;   /* short page = the index is exhausted */
    }
    return confirmed;
}

int capture_pending_count(port_storage_t *st) {
    int pending = 0;
    for (int skip = 0; ; skip += RETRY_PAGE) {
        /* shares the batch statics */
        int n = rec_index_list_page(st, s_retry_ids, RETRY_PAGE, skip);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            if (sidecar_load(st, s_retry_ids[i], &s_retry_sc) != 0) continue;
            if (strcmp(s_retry_sc.state, "not_uploaded") == 0) pending++;
        }
        if (n < RETRY_PAGE) break;
    }
    return pending;
}
