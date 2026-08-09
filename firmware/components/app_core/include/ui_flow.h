#ifndef UI_FLOW_H
#define UI_FLOW_H

#include "htp_client.h"
#include "ports.h"
#include "gesture.h"
#include "ui_fb.h"
#include "ui_widgets.h"

/* Screens. SCR_DASHBOARD doubles as the RESTING display state: cursor < 0
 * means "just showing the list" (the wake screen, and the image the panel
 * retains through sleep); cursor >= 0 means the operator opened it from
 * the menu and is steering the cursor. */
typedef enum { SCR_DASHBOARD, SCR_RECORDINGS, SCR_ENTRY, SCR_SETTINGS,
               SCR_MENU } ui_screen_t;

typedef enum { UIF_NONE, UIF_REDRAW_PARTIAL, UIF_REDRAW_FULL,
               UIF_START_CAPTURE, UIF_PLAY_WAV, UIF_POWER_OFF,
               UIF_SLEEP,        /* menu "Sleep": caller deep-sleeps now */
               UIF_COMPLETE      /* caller: click first, then
                                    ui_flow_complete_cursor() (it POSTs --
                                    feedback must not wait on the network) */
             } ui_action_t;

/* Click feedback (C7 round 5): every ACCEPTED press gets an instant sound,
 * keyed by button so the mapping is learnable in one session --
 *   NEXT   = any accepted PWR press (advance / back / open menu)
 *   SELECT = any accepted REC press (select / complete / play / dismiss)
 * UI_CLICK_NONE = the press changed nothing and was not accepted (no
 * click, no redraw -- a no-op press must not pretend otherwise). */
typedef enum { UI_CLICK_NONE, UI_CLICK_NEXT, UI_CLICK_SELECT } ui_click_t;

/* Menu rows, in display order. */
enum { MENU_DASHBOARD, MENU_RECORDINGS, MENU_SETTINGS, MENU_SLEEP, MENU_COUNT };

/* C7 refresh policy (finding A): partial refresh is the default for EVERY
 * within-session update -- cursor, strike, banner, page turns, screen
 * changes. A full (the black/white strobe) happens only (1) on the session's
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
    int cursor;                  /* per-screen cursor; < 0 on SCR_DASHBOARD = resting */
    htp_dashboard_t dash;        /* current dashboard model (loaded by caller) */
    char banner[200];            /* "" = none */
    ui_settings_info_t info;
    ui_status_t status;
    int partial_count;           /* ghost-clear full every UI_GHOST_CLEAR_EVERY partials */
    char entry_id[64];           /* open recording */
    int entry_page;
    ui_click_t click;            /* set by ui_flow_gesture: feedback for THIS press */
} ui_flow_t;

void ui_flow_init(ui_flow_t *u, htp_client_t *c, port_storage_t *st, port_kv_t *kv);
void ui_flow_render(ui_flow_t *u, ui_fb_t *fb);   /* draws current screen into fb */

/* Applies one gesture under the menu grammar (C7 round 5, the reference
 * device's own two-button language):
 *
 *   resting dashboard   PWR tap/long -> MENU          REC tap -> dismiss banner
 *   MENU                PWR tap -> next row (wraps)   REC tap -> select row
 *   dashboard (opened)  PWR tap -> cursor down        REC tap -> complete item
 *   recordings          PWR tap -> cursor down        REC tap -> open entry
 *   entry               PWR tap -> next page          REC tap -> play WAV
 *   settings            PWR tap -> up to menu         REC tap -> (nothing)
 *   anywhere            PWR long -> up one level; REC hold -> capture;
 *                       PWR held 5 s -> power off
 *
 * The one uniform navigation rule: a PWR tap ADVANCES only when there are
 * at least two positions to move between; with zero or one it climbs one
 * level instead (entry -> recordings, any list/settings -> menu, resting
 * dashboard -> menu). A tap therefore always lands a visible change --
 * never an invisible wrap onto itself (the round-5 operator failure).
 * out_path receives the WAV path for UIF_PLAY_WAV. u->click is set for
 * every call (UI_CLICK_NONE when the press was not accepted). */
ui_action_t ui_flow_gesture(ui_flow_t *u, gesture_t g, char out_path[96]);

/* UIF_COMPLETE second half: POSTs the completion of the item under the
 * dashboard cursor and marks the model. Split from the gesture so the
 * caller can play the click BEFORE this network round-trip. Returns 1 when
 * the model changed (item now done -> redraw + re-snapshot), 0 when the
 * POST failed or the cursor is invalid (nothing changed -> no redraw). */
int ui_flow_complete_cursor(ui_flow_t *u);

/* Returns to the resting dashboard display state (the designed sleep
 * image). 1 = state changed (caller should redraw), 0 = already resting. */
int ui_flow_rest(ui_flow_t *u);

/* Invalidates the per-session Recordings cache (filtered id list, preview
 * lines, open-entry transcript). Call whenever a capture or sync may have
 * changed sidecar state -- the same points that reset the pending-upload
 * memo. Without the cache every Recordings keypress re-read the index and
 * up to 30 sidecars x3 call sites (~90 file opens per press); with it a
 * cursor move costs zero I/O. */
void ui_flow_rec_invalidate(void);

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
