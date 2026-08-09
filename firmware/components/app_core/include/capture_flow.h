#ifndef CAPTURE_FLOW_H
#define CAPTURE_FLOW_H

#include "htp_client.h"
#include "sidecar.h"
#include "ports.h"

typedef enum {
    CAPTURE_DONE,
    CAPTURE_REPLY_READY,
    CAPTURE_FAILED,
    CAPTURE_OFFLINE,
    CAPTURE_TIMEOUT,
    CAPTURE_AUTH_ERROR
} capture_outcome_t;

typedef struct {
    htp_client_t  *client;
    port_storage_t *storage;
    port_clock_t   *clock;
    void (*on_status)(void *ui_ctx, const char *line);  /* short status line for the display */
    void *ui_ctx;
    unsigned poll_interval_ms;   /* 1000 in production */
    unsigned poll_window_ms;     /* 60000 in production */
    const char *reply_path;      /* "/reply.tmp.wav" */
} capture_ctx_t;

/* Run after the WAV is finalized on SD and the sidecar exists (state not_uploaded).
 * Uploads (up to 3 attempts with backoff inside the session), then polls at
 * poll_interval_ms until a terminal state or poll_window_ms elapses.
 * Updates the sidecar at every transition. On CAPTURE_REPLY_READY the reply has
 * already been downloaded to reply_path. */
capture_outcome_t capture_run(capture_ctx_t *cx, sidecar_t *sc);

/* Sync step 1: re-upload every sidecar in state not_uploaded (or reported unknown).
 * Returns number of captures successfully confirmed uploaded. Does NOT poll. */
int capture_retry_pending(capture_ctx_t *cx);

/* Number of recordings still awaiting upload (sidecar state not_uploaded):
 * the status header's pending-uploads count, and the "did this sync change
 * anything worth redrawing" signal. Storage only, no network. */
int capture_pending_count(port_storage_t *st);

#endif
