#ifndef UI_FLOW_H
#define UI_FLOW_H

#include "htp_client.h"
#include "ports.h"
#include "gesture.h"
#include "ui_fb.h"
#include "ui_widgets.h"

typedef enum { SCR_DASHBOARD, SCR_RECORDINGS, SCR_ENTRY, SCR_SETTINGS } ui_screen_t;
typedef enum { UIF_NONE, UIF_REDRAW_PARTIAL, UIF_REDRAW_FULL,
               UIF_START_CAPTURE, UIF_PLAY_WAV, UIF_POWER_OFF } ui_action_t;

/* C7 refresh policy (finding A): partial refresh is the default for EVERY
 * within-session update -- cursor, strike, banner, page turns, screen
 * cycles. A full (the black/white strobe) happens only (1) on the session's
 * first draw after deep sleep/cold boot, where the panel's previous-image
 * RAM was lost (caller-driven, see main.c present()), and (2) on this
 * ghost-clear cadence, since mode-2 partials slowly accumulate ghosting.
 * 12 is a first guess (>= 10 per the C7 round-1 requirement), UNTUNED:
 * adjust after bench observation of ghost buildup. */
#define UI_GHOST_CLEAR_EVERY 12

typedef struct {
    char mac[18]; char fw_version[16]; char bridge_host[64]; int sync_interval_s;
} ui_settings_info_t;

typedef struct {
    htp_client_t *client;
    port_storage_t *storage;
    port_kv_t *kv;
    ui_screen_t screen;
    int cursor;                  /* per-screen cursor/page */
    htp_dashboard_t dash;        /* current dashboard model (loaded by caller) */
    char banner[200];            /* "" = none */
    ui_settings_info_t info;
    ui_status_t status;
    int partial_count;           /* ghost-clear full every UI_GHOST_CLEAR_EVERY partials */
    char entry_id[64];           /* open recording */
    int entry_page;
} ui_flow_t;

void ui_flow_init(ui_flow_t *u, htp_client_t *c, port_storage_t *st, port_kv_t *kv);
void ui_flow_render(ui_flow_t *u, ui_fb_t *fb);   /* draws current screen into fb */
/* Applies one gesture. out_path receives the WAV path for UIF_PLAY_WAV. */
ui_action_t ui_flow_gesture(ui_flow_t *u, gesture_t g, char out_path[96]);
/* Refresh-discipline helper: call with the action; returns 1 when the caller
 * should do a FULL refresh. Gestures only ever ask for partials now (see the
 * policy above UI_GHOST_CLEAR_EVERY); UIF_REDRAW_FULL remains the way a
 * caller explicitly requests a full and resets the ghost-clear budget. */
int ui_flow_wants_full(ui_flow_t *u, ui_action_t a);

/* 1 when the two dashboards would render identical pixels: title,
 * item_count, and per-item text/done/style. rev and item ids are
 * deliberately ignored -- they never touch the panel. C7 finding B: the
 * bridge bumps its content-hash rev whenever the agent republishes, so a
 * "changed" dashboard fetch can carry a screen the operator is already
 * looking at (e.g. right after a complete gesture drew the strike); the
 * render callback uses this to skip the repaint entirely. */
int ui_dash_content_equal(const htp_dashboard_t *a, const htp_dashboard_t *b);

#endif
