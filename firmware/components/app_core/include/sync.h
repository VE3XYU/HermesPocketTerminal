#ifndef SYNC_H
#define SYNC_H

#include "htp_client.h"
#include "ports.h"
#include "capture_flow.h"

typedef struct {
    htp_client_t   *client;
    port_storage_t *storage;
    port_kv_t      *kv;         /* keys: "dash_rev", "sync_s", "acked" (comma list) */
    port_clock_t   *clock;
    capture_ctx_t  *capture;    /* for capture_retry_pending */
    /* Render callbacks return 0 once content is actually on the panel (ack gate). */
    int  (*render_dashboard)(void *ui_ctx, const htp_dashboard_t *d);
    int  (*render_notifications)(void *ui_ctx, const htp_notifications_t *n);
    void (*chime)(void *ui_ctx);
    void *ui_ctx;
    void (*set_rtc)(void *rtc_ctx, long long epoch);  /* drift correction */
    void *rtc_ctx;
} sync_ctx_t;

typedef struct {
    int uploads_retried, notifs_fetched, notifs_acked;
    int dashboard_changed;      /* 1 when a redraw happened */
    int sync_interval_s;        /* value to arm the next timer wake with */
} sync_report_t;

int sync_cycle(sync_ctx_t *cx, sync_report_t *out);   /* 0 ok; -1 total network failure */

#endif
