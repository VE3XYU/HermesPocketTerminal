#include "ui_flow.h"
#include "sidecar.h"
#include "rec_index.h"
#include "util.h"
#include <string.h>
#include <stdio.h>

/* Recordings browsing depth: how far back PWR-short can scroll. Bounded
 * by ui_list_t.rows[32] and rec_index_list()'s max <= 32 guard; five
 * scroll windows of UI_LIST_ROWS at the scale-2 row height. */
#define UI_FLOW_REC_CAP 30

/* Flow scratch models: static, not stack (C5 stack ruling; see
 * rec_index.c). As locals these stacked multi-KB frames under the render
 * callbacks: the id list is 1.9 KB (three call sites), ui_list_t 2.4 KB,
 * sidecar_t 1.2 KB. All users run on the one main task and never nest --
 * each buffer is fully re-populated (or memset) before every use. */
static char      s_rec_ids[UI_FLOW_REC_CAP][64];
static ui_list_t s_list;
static sidecar_t s_sc;

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
    return widget_text_pages(text);   /* count-only: no scratch framebuffer */
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
    /* Known limitation (Task 5, fix out of scope here): rec_index_list()
     * reads the index from offset 0 with a 16 KB cap, so once the
     * append-only index outgrows that (~180 recordings at ~89 B/entry)
     * the NEWEST entries -- the ones this screen exists to show -- fall
     * past the cap and stop appearing. */
    int n = rec_index_list(u->storage, s_rec_ids, UI_FLOW_REC_CAP);

    ui_list_t *l = &s_list;
    memset(l, 0, sizeof *l);
    str_copy(l->title, sizeof l->title, "Recordings");
    l->cursor = u->cursor;
    l->row_count = n;
    for (int i = 0; i < n && i < 32; i++) {
        sidecar_load(u->storage, s_rec_ids[i], &s_sc);
        if (s_sc.transcript[0]) {
            ui_ellipsize(l->rows[i].text, sizeof l->rows[i].text, s_sc.transcript,
                         UI_LINE_CHARS);
        } else if (strcmp(s_sc.state, "uploaded") == 0) {
            str_copy(l->rows[i].text, sizeof l->rows[i].text, "(pending)");
        } else {
            /* the design's "(not uploaded)" is 14 chars -- 2 over the
             * scale-2 line budget -- so it is shortened, not ellipsized */
            str_copy(l->rows[i].text, sizeof l->rows[i].text, "(not sent)");
        }
    }
    widget_list(fb, l);
}

static void render_entry(ui_flow_t *u, ui_fb_t *fb) {
    sidecar_load(u->storage, u->entry_id, &s_sc);
    const char *text = s_sc.transcript[0] ? s_sc.transcript : "(no transcript yet)";
    widget_text_page(fb, text, u->entry_page);
}

/* Draws `s` hard-wrapped at UI_LINE_CHARS chars across at most max_lines
 * scale-2 lines; when the tail still doesn't fit, the last line is
 * ellipsized (never clipped mid-glyph). Returns y advanced by the lines
 * used. Hard wrap, not word wrap: settings values (MAC, host) have no
 * useful word boundaries. */
static int settings_lines(ui_fb_t *fb, int y, const char *s, int max_lines) {
    size_t len = strlen(s), off = 0;
    for (int ln = 0; ln < max_lines && (off < len || ln == 0); ln++) {
        char seg[UI_LINE_CHARS + 1];
        if (ln == max_lines - 1 && len - off > UI_LINE_CHARS) {
            ui_ellipsize(seg, sizeof seg, s + off, UI_LINE_CHARS);
            off = len;
        } else {
            size_t take = len - off;
            if (take > UI_LINE_CHARS) take = UI_LINE_CHARS;
            memcpy(seg, s + off, take);
            seg[take] = '\0';
            off += take;
        }
        fb_text(fb, 2, y, seg, UI_TEXT_SCALE, 1);
        y += UI_TEXT_LINE_H;
    }
    return y;
}

static void render_settings(ui_flow_t *u, ui_fb_t *fb) {
    /* MAC (17 chars) and the bridge host wrap onto two scale-2 lines;
     * worst case 2+1+2+1 = 6 lines ends at y = 24 + 6*20 = 144..159,
     * clear of the banner strip (y >= 174). */
    int y = UI_STATUS_H + 4;
    y = settings_lines(fb, y, u->info.mac, 2);
    y = settings_lines(fb, y, u->info.fw_version, 1);
    y = settings_lines(fb, y, u->info.bridge_host, 2);
    char buf[32];
    snprintf(buf, sizeof buf, "sync %ds", u->info.sync_interval_s);
    settings_lines(fb, y, buf, 1);
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
