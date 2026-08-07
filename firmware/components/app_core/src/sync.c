#include "sync.h"
#include "capture_flow.h"
#include "rec_index.h"
#include "sidecar.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BACKFILL_CAP    32
#define ACK_KV_CAP      64   /* fake_kv/port_kv value budget for "acked" */
#define ACK_KEEP        8    /* last N acked ids retained in the ring */
#define ACK_ITEM_LEN    24   /* matches htp_notification_t.id */
#define ACK_MAX_ITEMS   (ACK_KEEP + 16)  /* existing ring + one full notif batch */

/* ---- kv "acked" ring: comma-joined ids, most-recent-last, 64-byte budget ---- */

static int id_in_ring(const char *ring, const char *id) {
    size_t idlen = strlen(id);
    const char *p = ring;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t seglen = comma ? (size_t)(comma - p) : strlen(p);
        if (seglen == idlen && strncmp(p, id, idlen) == 0) return 1;
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

static void copy_seg(char *dst, size_t cap, const char *src, size_t len) {
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* Appends new_ids to the existing "acked" ring, keeps only the last ACK_KEEP
 * entries, then drops further oldest entries until the comma-joined result
 * fits the kv value budget. */
static void ring_append(port_kv_t *kv, const char *const new_ids[], int n) {
    char cur[ACK_KV_CAP];
    if (kv->get(kv->ctx, "acked", cur, sizeof cur) != 0) cur[0] = '\0';

    char items[ACK_MAX_ITEMS][ACK_ITEM_LEN];
    int count = 0;

    const char *p = cur;
    while (*p && count < ACK_MAX_ITEMS) {
        const char *comma = strchr(p, ',');
        size_t seglen = comma ? (size_t)(comma - p) : strlen(p);
        if (seglen > 0) copy_seg(items[count++], ACK_ITEM_LEN, p, seglen);
        if (!comma) break;
        p = comma + 1;
    }
    for (int i = 0; i < n && count < ACK_MAX_ITEMS; i++)
        copy_seg(items[count++], ACK_ITEM_LEN, new_ids[i], strlen(new_ids[i]));

    int start = count > ACK_KEEP ? count - ACK_KEEP : 0;
    char out[ACK_KV_CAP];
    for (;;) {
        size_t pos = 0;
        int fits = 1;
        for (int i = start; i < count; i++) {
            size_t l = strlen(items[i]);
            size_t need = l + (pos ? 1 : 0);
            if (pos + need >= sizeof out) { fits = 0; break; }
            if (pos) out[pos++] = ',';
            memcpy(out + pos, items[i], l);
            pos += l;
        }
        out[pos] = '\0';
        if (fits || start >= count) break;
        start++;
    }
    kv->set(kv->ctx, "acked", out);
}

/* ---- step 4: transcript backfill ---- */

/* Batch buffers: static, not stack. An htp_capture_status_t is ~1.1 KB
 * (transcript[1024]) and the id list another 2 KB, so as locals this was a
 * ~40 KB frame -- larger than any task stack the firmware runs sessions on.
 * Plain C file-scope storage keeps app_core target-agnostic (no OS or
 * allocator calls -- the same ruling that made the audio staging buffers
 * static); the cost is reentrancy, which sync_cycle() never had anyway:
 * it is documented single-caller/single-task on the device and the host
 * suite calls it sequentially. Both arrays are fully re-populated before
 * every use (rec_index_list / htp_poll_captures bound the read ranges). */
static char s_idx_ids[BACKFILL_CAP][64];
static htp_capture_status_t s_statuses[BACKFILL_CAP];

static void backfill(sync_ctx_t *cx, int *net_calls, int *net_failures) {
    int idx_n = rec_index_list(cx->storage, s_idx_ids, BACKFILL_CAP);

    const char *bf_ids[BACKFILL_CAP];
    int bf_n = 0;
    for (int i = 0; i < idx_n; i++) {
        sidecar_t sc;
        if (sidecar_load(cx->storage, s_idx_ids[i], &sc) != 0) continue;
        if (strcmp(sc.state, "uploaded") == 0) bf_ids[bf_n++] = s_idx_ids[i];
    }
    if (bf_n == 0) return;

    htp_capture_status_t *statuses = s_statuses;
    long long server_time = 0;
    int n = htp_poll_captures(cx->client, bf_ids, bf_n, statuses, BACKFILL_CAP, &server_time);
    (*net_calls)++;
    if (n == HTP_ERR_NETWORK) { (*net_failures)++; return; }
    if (n < 0) return;

    for (int i = 0; i < n; i++) {
        sidecar_t sc;
        if (sidecar_load(cx->storage, statuses[i].id, &sc) != 0) continue;
        if (statuses[i].transcript[0])
            str_copy(sc.transcript, sizeof sc.transcript, statuses[i].transcript);
        if (statuses[i].conversation_id[0])
            str_copy(sc.conversation_id, sizeof sc.conversation_id, statuses[i].conversation_id);
        switch (statuses[i].state) {
            case HTP_ST_DONE:        str_copy(sc.state, sizeof sc.state, "done"); break;
            case HTP_ST_REPLY_READY: str_copy(sc.state, sizeof sc.state, "reply_ready"); break;
            case HTP_ST_FAILED:
                str_copy(sc.state, sizeof sc.state, "failed");
                str_copy(sc.error, sizeof sc.error, statuses[i].error);
                break;
            case HTP_ST_UNKNOWN:     str_copy(sc.state, sizeof sc.state, "not_uploaded"); break;
            default: break; /* received / transcribing / processing: still in flight */
        }
        sidecar_save(cx->storage, &sc);
    }
}

/* The response models are static for the same stack-size reason as the
 * backfill batch above: htp_notifications_t is ~3.8 KB and htp_dashboard_t
 * ~3.7 KB, and both sat in sync_cycle()'s own frame. Same non-reentrancy
 * trade, same single-task usage; both are memset before use. */
static htp_notifications_t s_notifs;
static htp_dashboard_t s_dash;

int sync_cycle(sync_ctx_t *cx, sync_report_t *out) {
    memset(out, 0, sizeof *out);
    int net_calls = 0, net_failures = 0;

    /* 1. retry uploads */
    out->uploads_retried = capture_retry_pending(cx->capture);

    /* 2. notifications */
    memset(&s_notifs, 0, sizeof s_notifs);
    int notif_err = htp_get_notifications(cx->client, &s_notifs);
    net_calls++;
    if (notif_err == HTP_ERR_NETWORK) net_failures++;
    int notif_ok = (notif_err == HTP_OK);
    int render_ok = 0;

    if (notif_ok) {
        out->notifs_fetched = s_notifs.count;

        char ring[ACK_KV_CAP];
        if (cx->kv->get(cx->kv->ctx, "acked", ring, sizeof ring) != 0) ring[0] = '\0';
        for (int i = 0; i < s_notifs.count; i++) {
            if (s_notifs.items[i].urgent && !id_in_ring(ring, s_notifs.items[i].id)) {
                if (cx->chime) cx->chime(cx->ui_ctx);
                break; /* chime once per cycle */
            }
        }
        if (cx->render_notifications)
            render_ok = (cx->render_notifications(cx->ui_ctx, &s_notifs) == 0);
    }

    /* 3. dashboard */
    char rev[24];
    if (cx->kv->get(cx->kv->ctx, "dash_rev", rev, sizeof rev) != 0) rev[0] = '\0';
    memset(&s_dash, 0, sizeof s_dash);
    int dash_err = htp_get_dashboard(cx->client, rev, &s_dash);
    net_calls++;
    if (dash_err == HTP_ERR_NETWORK) net_failures++;

    if (dash_err == HTP_OK) {
        if (!s_dash.unchanged) {
            if (cx->render_dashboard) cx->render_dashboard(cx->ui_ctx, &s_dash);
            cx->kv->set(cx->kv->ctx, "dash_rev", s_dash.rev);
            out->dashboard_changed = 1;
        }
        char sbuf[16];
        snprintf(sbuf, sizeof sbuf, "%d", s_dash.sync_interval);
        cx->kv->set(cx->kv->ctx, "sync_s", sbuf);
        out->sync_interval_s = s_dash.sync_interval;

        long long diff = s_dash.server_time - cx->clock->epoch_s(cx->clock->ctx);
        if (llabs(diff) > 2 && cx->set_rtc) cx->set_rtc(cx->rtc_ctx, s_dash.server_time);
    }

    /* 4. transcript backfill */
    backfill(cx, &net_calls, &net_failures);

    /* 5. ack — only what render_notifications actually got onto the panel */
    if (notif_ok && render_ok && s_notifs.count > 0) {
        const char *ack_ids[16];
        for (int i = 0; i < s_notifs.count; i++) ack_ids[i] = s_notifs.items[i].id;
        int ack_err = htp_ack_notifications(cx->client, ack_ids, s_notifs.count);
        net_calls++;
        if (ack_err == HTP_ERR_NETWORK) net_failures++;
        if (ack_err == HTP_OK) {
            out->notifs_acked = s_notifs.count;
            ring_append(cx->kv, ack_ids, s_notifs.count);
        }
    }

    if (net_calls > 0 && net_failures == net_calls) return -1;
    return 0;
}
