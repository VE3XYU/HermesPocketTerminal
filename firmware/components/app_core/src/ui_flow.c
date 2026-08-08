#include "ui_flow.h"
#include "sidecar.h"
#include "rec_index.h"
#include "util.h"
#include <string.h>
#include <stdio.h>

/* recordings list cap: matches the render window budget (Task 10 brief). */
#define UI_FLOW_REC_CAP (UI_LIST_ROWS * 3)

/* Flow scratch models: static, not stack (C5 stack ruling; see
 * rec_index.c). As locals these stacked multi-KB frames under the render
 * callbacks: the id list is 1.9 KB (three call sites), ui_list_t 2.4 KB,
 * sidecar_t 1.2 KB, and the pagination framebuffer 5 KB. All users run
 * on the one main task and never nest -- each buffer is fully
 * re-populated (or memset) before every use. */
static char      s_rec_ids[UI_FLOW_REC_CAP][64];
static ui_list_t s_list;
static sidecar_t s_sc;
static ui_fb_t   s_scratch_fb;   /* entry_total_pages page counting only */

void ui_flow_init(ui_flow_t *u, htp_client_t *c, port_storage_t *st, port_kv_t *kv) {
    memset(u, 0, sizeof *u);
    u->client = c;
    u->storage = st;
    u->kv = kv;
    u->screen = SCR_DASHBOARD;
}

/* ---- gesture helpers ---- */

/* Row count for the active screen's cursor (Dashboard items / Recordings
 * list). Entry paginates via entry_page (see entry_total_pages) instead. */
static int active_row_count(ui_flow_t *u) {
    switch (u->screen) {
        case SCR_DASHBOARD:
            return u->dash.item_count;
        case SCR_RECORDINGS:
            return rec_index_list(u->storage, s_rec_ids, UI_FLOW_REC_CAP);
        default:
            return 1;
    }
}

static int entry_total_pages(ui_flow_t *u) {
    sidecar_load(u->storage, u->entry_id, &s_sc);
    const char *text = s_sc.transcript[0] ? s_sc.transcript : "(no transcript yet)";
    fb_clear(&s_scratch_fb);
    return widget_text_page(&s_scratch_fb, text, 0);
}

ui_action_t ui_flow_gesture(ui_flow_t *u, gesture_t g, char out_path[96]) {
    /* pass through from anywhere, before any screen logic */
    if (g == GEST_REC_HOLD_START) return UIF_START_CAPTURE;
    if (g == GEST_PWR_OFF) return UIF_POWER_OFF;

    switch (g) {
        case GEST_REC_SHORT: {
            if (u->banner[0]) {
                u->banner[0] = '\0';
                return UIF_REDRAW_PARTIAL;
            }
            if (u->screen == SCR_DASHBOARD) {
                if (u->dash.item_count > 0) {
                    int idx = u->cursor;
                    if (idx < 0 || idx >= u->dash.item_count) idx = 0;
                    char rev[24];
                    int err = htp_complete_item(u->client, u->dash.items[idx].id, rev);
                    if (err == HTP_OK) {
                        u->dash.items[idx].done = 1;
                        u->kv->set(u->kv->ctx, "dash_rev", rev);
                    }
                }
                return UIF_REDRAW_PARTIAL;
            }
            if (u->screen == SCR_ENTRY) {
                sidecar_wav_path(out_path, u->entry_id);
                return UIF_PLAY_WAV;
            }
            return UIF_NONE;
        }

        case GEST_PWR_SHORT: {
            if (u->screen == SCR_ENTRY) {
                int pages = entry_total_pages(u);
                if (pages < 1) pages = 1;
                u->entry_page = (u->entry_page + 1) % pages;
            } else {
                int rows = active_row_count(u);
                if (rows < 1) rows = 1;
                u->cursor = (u->cursor + 1) % rows;
            }
            return UIF_REDRAW_PARTIAL;
        }

        case GEST_PWR_DOUBLE: {
            /* ENTRY counts as Recordings for the purpose of the cycle */
            ui_screen_t eff = (u->screen == SCR_ENTRY) ? SCR_RECORDINGS : u->screen;
            switch (eff) {
                case SCR_DASHBOARD:  u->screen = SCR_RECORDINGS; break;
                case SCR_RECORDINGS: u->screen = SCR_SETTINGS;   break;
                default:             u->screen = SCR_DASHBOARD;  break;
            }
            u->cursor = 0;
            return UIF_REDRAW_FULL;
        }

        case GEST_PWR_LONG: {
            if (u->screen == SCR_RECORDINGS) {
                int n = rec_index_list(u->storage, s_rec_ids, UI_FLOW_REC_CAP);
                if (n <= 0) return UIF_NONE;
                int idx = u->cursor;
                if (idx < 0 || idx >= n) idx = 0;
                str_copy(u->entry_id, sizeof u->entry_id, s_rec_ids[idx]);
                u->entry_page = 0;
                u->screen = SCR_ENTRY;
                return UIF_REDRAW_FULL;
            }
            if (u->screen == SCR_ENTRY) {
                u->screen = SCR_RECORDINGS;
                return UIF_REDRAW_FULL;
            }
            return UIF_NONE;
        }

        default:
            return UIF_NONE;
    }
}

int ui_flow_wants_full(ui_flow_t *u, ui_action_t a) {
    if (a == UIF_REDRAW_FULL) {
        u->partial_count = 0;
        return 1;
    }
    if (a == UIF_REDRAW_PARTIAL) {
        u->partial_count++;
        if (u->partial_count >= 8) {
            u->partial_count = 0;
            return 1;
        }
        return 0;
    }
    return 0;
}

/* ---- render ---- */

static void render_dashboard(ui_flow_t *u, ui_fb_t *fb) {
    ui_list_t *l = &s_list;
    memset(l, 0, sizeof *l);
    str_copy(l->title, sizeof l->title, u->dash.title);
    l->cursor = u->cursor;
    l->row_count = u->dash.item_count;
    for (int i = 0; i < u->dash.item_count && i < 32; i++) {
        str_copy(l->rows[i].text, sizeof l->rows[i].text, u->dash.items[i].text);
        l->rows[i].done = u->dash.items[i].done;
    }
    widget_list(fb, l);
    if (u->banner[0]) widget_banner(fb, u->banner);
}

static void render_recordings(ui_flow_t *u, ui_fb_t *fb) {
    int n = rec_index_list(u->storage, s_rec_ids, UI_FLOW_REC_CAP);

    ui_list_t *l = &s_list;
    memset(l, 0, sizeof *l);
    str_copy(l->title, sizeof l->title, "Recordings");
    l->cursor = u->cursor;
    l->row_count = n;
    for (int i = 0; i < n && i < 32; i++) {
        sidecar_load(u->storage, s_rec_ids[i], &s_sc);
        if (s_sc.transcript[0]) {
            ui_ellipsize(l->rows[i].text, sizeof l->rows[i].text, s_sc.transcript, 22);
        } else if (strcmp(s_sc.state, "uploaded") == 0) {
            str_copy(l->rows[i].text, sizeof l->rows[i].text, "(pending)");
        } else {
            str_copy(l->rows[i].text, sizeof l->rows[i].text, "(not uploaded)");
        }
    }
    widget_list(fb, l);
}

static void render_entry(ui_flow_t *u, ui_fb_t *fb) {
    sidecar_load(u->storage, u->entry_id, &s_sc);
    const char *text = s_sc.transcript[0] ? s_sc.transcript : "(no transcript yet)";
    widget_text_page(fb, text, u->entry_page);
}

static void render_settings(ui_flow_t *u, ui_fb_t *fb) {
    int y = UI_STATUS_H + 4;
    fb_text(fb, 2, y, u->info.mac, 1, 1); y += 12;
    fb_text(fb, 2, y, u->info.fw_version, 1, 1); y += 12;
    fb_text(fb, 2, y, u->info.bridge_host, 1, 1); y += 12;
    char buf[32];
    snprintf(buf, sizeof buf, "sync %ds", u->info.sync_interval_s);
    fb_text(fb, 2, y, buf, 1, 1);
}

void ui_flow_render(ui_flow_t *u, ui_fb_t *fb) {
    fb_clear(fb);
    widget_status_line(fb, &u->status);
    switch (u->screen) {
        case SCR_DASHBOARD:  render_dashboard(u, fb);  break;
        case SCR_RECORDINGS: render_recordings(u, fb); break;
        case SCR_ENTRY:      render_entry(u, fb);      break;
        case SCR_SETTINGS:   render_settings(u, fb);   break;
        default: break;
    }
}
