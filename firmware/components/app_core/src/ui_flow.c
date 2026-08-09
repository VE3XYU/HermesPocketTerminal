#include "ui_flow.h"
#include "sidecar.h"
#include "rec_index.h"
#include "util.h"
#include <string.h>
#include <stdio.h>

/* Recordings browsing depth: how far back the cursor can scroll. Bounded
 * by ui_list_t.rows[32] and rec_index_list()'s max <= 32 guard; ten
 * scroll windows of UI_LIST2_ROWS two-line rows. */
#define UI_FLOW_REC_CAP 30

/* Flow scratch models: static, not stack (C5 stack ruling; see
 * rec_index.c). As locals these stacked multi-KB frames under the render
 * callbacks: ui_list_t is 4.7 KB, sidecar_t 1.2 KB. All users run on the
 * one main task and never nest -- each buffer is fully re-populated (or
 * memset) before every use. */
static ui_list_t s_list;
static sidecar_t s_sc;

/* ---- per-session Recordings cache (C7 round 5 efficiency) ----
 *
 * The browsable list is the raw index minus conversation captures (C7
 * round-3 finding 4). The filter matches on the sidecar's conversation_id
 * being non-empty -- a protocol-level mode distinction stamped at capture
 * time, never a reading of the transcript, so the terminal's
 * content-blindness invariant holds. Filtered captures stay on the card
 * and in every sync path (capture_pending_count and the retry scan read
 * the raw index); they just don't clutter the list.
 *
 * Before this cache, EVERY Recordings keypress re-derived the list --
 * cursor count, entry open and render each re-read the 16 KB index and up
 * to 30 sidecars (~90 file opens per press). Now the derivation runs once
 * and is memoized here (ids + the exact preview line the renderer shows);
 * a cursor move is O(1) on I/O. Invalidated via ui_flow_rec_invalidate()
 * wherever sidecar state can change (captures, sync retries/backfill) and
 * on ui_flow_init. Statics per the C5 ruling (~7 KB total).
 *
 * Depth note (conscious): the raw read is capped at UI_FLOW_REC_CAP before
 * filtering, so heavy conversation use shortens the visible history rather
 * than reading deeper -- same class as the Task 5 16 KB index cap. And the
 * Task 5 cap itself still stands: once the append-only index outgrows
 * 16 KB (~180 recordings), the NEWEST entries fall past it (fix out of
 * scope, noted since Task 18). */
typedef struct {
    int  valid;
    int  count;
    char ids[UI_FLOW_REC_CAP][64];
    char preview[UI_FLOW_REC_CAP][136];   /* matches ui_row_t.text: sized past
                                             the two-line pixel budget so the
                                             measured ellipsis is faithful */
} rec_cache_t;
static rec_cache_t s_rc;

/* Open-entry cache: transcript + page count for u->entry_id, so paging and
 * re-renders don't re-read + re-parse the sidecar per press. */
static char s_entry_id[64];       /* "" = nothing cached */
static char s_entry_text[1024];
static int  s_entry_pages;

void ui_flow_rec_invalidate(void) {
    s_rc.valid = 0;
    s_entry_id[0] = '\0';
}

static int rec_count(ui_flow_t *u) {
    if (!s_rc.valid) {
        int n = rec_index_list(u->storage, s_rc.ids, UI_FLOW_REC_CAP);
        int kept = 0;
        for (int i = 0; i < n; i++) {
            int have = sidecar_load(u->storage, s_rc.ids[i], &s_sc) == 0;
            if (have && s_sc.conversation_id[0])
                continue;                   /* conversation turn: not listed */
            if (kept != i) memcpy(s_rc.ids[kept], s_rc.ids[i], sizeof s_rc.ids[0]);
            if (have && s_sc.transcript[0])
                str_copy(s_rc.preview[kept], sizeof s_rc.preview[0], s_sc.transcript);
            else if (have && strcmp(s_sc.state, "uploaded") == 0)
                str_copy(s_rc.preview[kept], sizeof s_rc.preview[0], "(pending)");
            else    /* not uploaded yet, or the sidecar is unreadable */
                str_copy(s_rc.preview[kept], sizeof s_rc.preview[0], "(not uploaded)");
            kept++;
        }
        s_rc.count = kept;
        s_rc.valid = 1;
    }
    return s_rc.count;
}

static const char *entry_text(ui_flow_t *u) {
    if (strcmp(s_entry_id, u->entry_id) != 0) {
        sidecar_load(u->storage, u->entry_id, &s_sc);   /* failure leaves *out zeroed */
        str_copy(s_entry_text, sizeof s_entry_text,
                 s_sc.transcript[0] ? s_sc.transcript : "(no transcript yet)");
        s_entry_pages = widget_text_pages(s_entry_text);
        str_copy(s_entry_id, sizeof s_entry_id, u->entry_id);
    }
    return s_entry_text;
}

void ui_flow_init(ui_flow_t *u, htp_client_t *c, port_storage_t *st, port_kv_t *kv) {
    memset(u, 0, sizeof *u);
    u->client = c;
    u->storage = st;
    u->kv = kv;
    u->screen = SCR_DASHBOARD;
    u->cursor = -1;               /* resting display state */
    ui_flow_rec_invalidate();
}

int ui_flow_rest(ui_flow_t *u) {
    if (u->screen == SCR_DASHBOARD && u->cursor < 0) return 0;
    u->screen = SCR_DASHBOARD;
    u->cursor = -1;
    return 1;
}

/* ---- gesture grammar (see ui_flow.h for the full table) ---- */

/* Menu index a screen "comes from", so climbing back up lands the menu
 * cursor on the row you left through. */
static int menu_row_of(ui_screen_t s) {
    switch (s) {
        case SCR_RECORDINGS: return MENU_RECORDINGS;
        case SCR_SETTINGS:   return MENU_SETTINGS;
        default:             return MENU_DASHBOARD;
    }
}

static ui_action_t to_menu(ui_flow_t *u, int row) {
    u->screen = SCR_MENU;
    u->cursor = row;
    u->click = UI_CLICK_NEXT;     /* PWR-driven navigation */
    return UIF_REDRAW_PARTIAL;
}

static ui_action_t pwr_tap(ui_flow_t *u) {
    u->click = UI_CLICK_NEXT;
    switch (u->screen) {
        case SCR_MENU:
            u->cursor = (u->cursor + 1) % MENU_COUNT;
            return UIF_REDRAW_PARTIAL;

        case SCR_DASHBOARD:
            if (u->cursor < 0)                       /* resting: open the menu */
                return to_menu(u, MENU_DASHBOARD);
            if (u->dash.item_count >= 2) {
                u->cursor = (u->cursor + 1) % u->dash.item_count;
                return UIF_REDRAW_PARTIAL;
            }
            return to_menu(u, MENU_DASHBOARD);       /* <= 1 item: nowhere visible to go */

        case SCR_RECORDINGS: {
            int n = rec_count(u);
            if (n >= 2) {
                u->cursor = (u->cursor + 1) % n;
                return UIF_REDRAW_PARTIAL;
            }
            return to_menu(u, MENU_RECORDINGS);
        }

        case SCR_ENTRY: {
            (void)entry_text(u);                     /* ensure s_entry_pages */
            if (s_entry_pages >= 2) {
                u->entry_page = (u->entry_page + 1) % s_entry_pages;
                return UIF_REDRAW_PARTIAL;
            }
            u->screen = SCR_RECORDINGS;              /* one page: up one level */
            return UIF_REDRAW_PARTIAL;
        }

        case SCR_SETTINGS:
        default:
            return to_menu(u, menu_row_of(u->screen));
    }
}

static ui_action_t pwr_long(ui_flow_t *u) {
    switch (u->screen) {
        case SCR_ENTRY:
            u->click = UI_CLICK_NEXT;
            u->screen = SCR_RECORDINGS;
            return UIF_REDRAW_PARTIAL;
        case SCR_MENU:                               /* back out to the resting display */
            u->click = UI_CLICK_NEXT;
            ui_flow_rest(u);
            return UIF_REDRAW_PARTIAL;
        default:   /* dashboard (resting or opened), recordings, settings */
            return to_menu(u, menu_row_of(u->screen));
    }
}

static ui_action_t menu_select(ui_flow_t *u, char out_path[96]) {
    (void)out_path;
    u->click = UI_CLICK_SELECT;
    switch (u->cursor) {
        case MENU_RECORDINGS:
            u->screen = SCR_RECORDINGS;
            u->cursor = 0;
            return UIF_REDRAW_PARTIAL;
        case MENU_SETTINGS:
            u->screen = SCR_SETTINGS;
            u->cursor = 0;
            return UIF_REDRAW_PARTIAL;
        case MENU_SLEEP:
            /* Rest first: the retained sleep image is the dashboard, never
             * a menu with "Sleep" highlighted (round-3 finding 2's rule --
             * what the panel shows at sleep is what it shows forever). */
            ui_flow_rest(u);
            return UIF_SLEEP;
        case MENU_DASHBOARD:
        default:
            u->screen = SCR_DASHBOARD;
            u->cursor = 0;                           /* opened: cursor visible */
            return UIF_REDRAW_PARTIAL;
    }
}

static ui_action_t rec_tap(ui_flow_t *u, char out_path[96]) {
    /* Banner dismissal takes precedence only where the banner is DRAWN
     * (the dashboard); on other screens the model's pending banner must
     * not eat a select press -- that would be an invisible no-op, the
     * exact failure class this round removes. */
    if (u->screen == SCR_DASHBOARD && u->banner[0]) {
        u->banner[0] = '\0';
        u->click = UI_CLICK_SELECT;
        return UIF_REDRAW_PARTIAL;
    }
    switch (u->screen) {
        case SCR_MENU:
            return menu_select(u, out_path);

        case SCR_DASHBOARD: {
            if (u->cursor < 0) return UIF_NONE;      /* resting: REC tap is idle */
            int idx = u->cursor;
            if (idx >= u->dash.item_count) return UIF_NONE;
            if (u->dash.items[idx].done) return UIF_NONE;   /* already struck: a
                                                               re-complete would redraw
                                                               identical content */
            u->click = UI_CLICK_SELECT;
            return UIF_COMPLETE;                     /* caller clicks, then completes */
        }

        case SCR_RECORDINGS: {
            int n = rec_count(u);
            if (n <= 0) return UIF_NONE;
            int idx = u->cursor;
            if (idx < 0 || idx >= n) idx = 0;
            str_copy(u->entry_id, sizeof u->entry_id, s_rc.ids[idx]);
            u->entry_page = 0;
            u->screen = SCR_ENTRY;
            u->click = UI_CLICK_SELECT;
            return UIF_REDRAW_PARTIAL;
        }

        case SCR_ENTRY:
            sidecar_wav_path(out_path, u->entry_id);
            u->click = UI_CLICK_SELECT;
            return UIF_PLAY_WAV;

        case SCR_SETTINGS:
        default:
            return UIF_NONE;
    }
}

ui_action_t ui_flow_gesture(ui_flow_t *u, gesture_t g, char out_path[96]) {
    u->click = UI_CLICK_NONE;
    /* pass through from anywhere, before any screen logic; neither carries
     * a click -- capture has its own feedback (glyph + the recording
     * itself) and power-off has the countdown + wipe */
    if (g == GEST_REC_HOLD_START) return UIF_START_CAPTURE;
    if (g == GEST_PWR_OFF) return UIF_POWER_OFF;

    switch (g) {
        case GEST_REC_SHORT: return rec_tap(u, out_path);
        case GEST_PWR_SHORT: return pwr_tap(u);
        case GEST_PWR_LONG:  return pwr_long(u);
        default:             return UIF_NONE;
    }
}

int ui_flow_complete_cursor(ui_flow_t *u) {
    if (u->screen != SCR_DASHBOARD) return 0;
    int idx = u->cursor;
    if (idx < 0 || idx >= u->dash.item_count) return 0;
    if (u->dash.items[idx].done) return 0;
    char rev[24];
    if (htp_complete_item(u->client, u->dash.items[idx].id, rev) != HTP_OK)
        return 0;                                    /* nothing changed: no redraw */
    u->dash.items[idx].done = 1;
    u->kv->set(u->kv->ctx, "dash_rev", rev);
    return 1;
}

int ui_dash_content_equal(const htp_dashboard_t *a, const htp_dashboard_t *b) {
    if (a->item_count != b->item_count) return 0;
    if (strcmp(a->title, b->title) != 0) return 0;
    for (int i = 0; i < a->item_count && i < 32; i++) {
        if (!a->items[i].done != !b->items[i].done) return 0;
        if (strcmp(a->items[i].text, b->items[i].text) != 0) return 0;
        if (strcmp(a->items[i].style, b->items[i].style) != 0) return 0;
    }
    return 1;
}

int ui_flow_wants_full(ui_flow_t *u, ui_action_t a) {
    if (a == UIF_REDRAW_FULL) {
        u->partial_count = 0;
        return 1;
    }
    if (a == UIF_REDRAW_PARTIAL) {
        u->partial_count++;
        if (u->partial_count >= UI_GHOST_CLEAR_EVERY) {
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
    l->cursor = u->cursor;        /* < 0 while resting: no row inverts */
    /* Belt-and-braces cap (Task 19): every current item_count source
     * already clamps at 32 (the network parser, the cache loader), but
     * widget_list indexes rows[row_count - 1] and scrolls by it -- a
     * future source of a bad count must degrade to a truncated list, not
     * an OOB read of ui_list_t.rows[32]. */
    l->row_count = u->dash.item_count <= 32 ? u->dash.item_count : 32;
    /* C7 round 7: the banner strip IS the last row band, so a pending
     * banner costs the list its last visible row rather than painting over
     * it. Dashboard-only, because the banner is drawn nowhere else. */
    l->reserve_banner = (u->banner[0] != '\0');
    for (int i = 0; i < u->dash.item_count && i < 32; i++) {
        str_copy(l->rows[i].text, sizeof l->rows[i].text, u->dash.items[i].text);
        l->rows[i].done = u->dash.items[i].done;
    }
    widget_list(fb, l);
    if (u->dash.item_count == 0)
        /* Zero items used to render a bare screen under the status bar
         * (C7 round 6, finding 3). Title stays; the centered hint makes
         * the emptiness read as deliberate. Centered in the rows region
         * only -- the banner strip is reserved ONLY while a banner is
         * actually up (round 7 retired its permanent reservation; with
         * no banner the hint gets the full rows region down to UI_H). */
        widget_empty_state(fb, "Nothing yet", "Hold REC to talk",
                           UI_STATUS_H + UI_TITLE_H,
                           UI_H - (u->banner[0] ? UI_BANNER_H : 0));
    if (u->banner[0]) widget_banner(fb, u->banner);
}

static void render_menu(ui_flow_t *u, ui_fb_t *fb) {
    static const char *const k_rows[MENU_COUNT] =
        { "Dashboard", "Recordings", "Settings", "Sleep" };
    ui_list_t *l = &s_list;
    memset(l, 0, sizeof *l);
    str_copy(l->title, sizeof l->title, "Menu");
    l->cursor = u->cursor;
    l->row_count = MENU_COUNT;
    for (int i = 0; i < MENU_COUNT; i++)
        str_copy(l->rows[i].text, sizeof l->rows[i].text, k_rows[i]);
    widget_list(fb, l);
}

static void render_recordings(ui_flow_t *u, ui_fb_t *fb) {
    int n = rec_count(u);

    ui_list_t *l = &s_list;
    memset(l, 0, sizeof *l);
    str_copy(l->title, sizeof l->title, "Recordings");
    l->cursor = u->cursor;
    l->row_count = n;
    l->two_line = 1;   /* two body lines of transcript opening per row --
                          the C7 round-3 density requirement; the widget
                          splits and ellipsizes per pixel */
    for (int i = 0; i < n && i < 32; i++)
        str_copy(l->rows[i].text, sizeof l->rows[i].text, s_rc.preview[i]);
    widget_list(fb, l);

    if (n == 0)   /* same deliberate empty-state pattern as the dashboard
                     (a PWR tap here climbs back to the menu); centered in
                     the full rows region -- Recordings has no banner
                     reservation */
        widget_empty_state(fb, "No recordings", "Hold REC to talk",
                           UI_STATUS_H + UI_TITLE_H, UI_H);
}

static void render_entry(ui_flow_t *u, ui_fb_t *fb) {
    widget_text_page(fb, entry_text(u), u->entry_page);
}

/* Draws `s` split per measured pixel width across at most max_lines
 * body lines; when the tail still doesn't fit, the last line is
 * ellipsized (never clipped mid-glyph). Takes and returns the BASELINE,
 * advanced by the lines used. Splits at word boundaries when the value
 * has any (fb_wrap_break_prop, C7 round 6); space-less values (MAC,
 * host) hard-split at the pixel budget exactly as before. */
static int settings_lines(ui_fb_t *fb, int baseline, const char *s, int max_lines) {
    size_t len = strlen(s), off = 0;
    for (int ln = 0; ln < max_lines && (off < len || ln == 0); ln++) {
        char seg[80];
        if (ln == max_lines - 1) {
            fb_ellipsize_prop(seg, sizeof seg, s + off, UI_FONT_BODY, UI_TEXT_W);
            off = len;
        } else {
            size_t take = (size_t)fb_wrap_break_prop(s + off, UI_FONT_BODY, UI_TEXT_W);
            if (take > sizeof seg - 1) take = sizeof seg - 1;
            memcpy(seg, s + off, take);
            seg[take] = '\0';
            off += take;
            while (s[off] == ' ') off++;           /* line 2 starts on a word */
        }
        fb_text_prop(fb, UI_MARGIN_X, baseline, seg, UI_FONT_BODY, 1);
        baseline += UI_TEXT_LINE_H;
        if (off >= len) break;
    }
    return baseline;
}

/* Worst case 1 (MAC) + 1 (fw) + 2 (host) + 1 (sync) = 5 baselines ends at
 * UI_TEXT_FIRST_BASE + 4 * UI_TEXT_LINE_H = 136 (+5 descent = 141), well
 * inside the panel -- compile-checked (Task 19), the same discipline as
 * widgets.c's grid asserts. */
_Static_assert(UI_TEXT_FIRST_BASE + 4 * UI_TEXT_LINE_H + UI_FONT_BODY_DESC < UI_H,
               "the 5-baseline settings ladder must stay on the panel");

static void render_settings(ui_flow_t *u, ui_fb_t *fb) {
    int y = UI_TEXT_FIRST_BASE;
    /* The MAC (17 chars) measures ~215-227 px in the escalated body --
     * wider than the 196 px line -- and its one job is to be compared to
     * the case sticker character-for-character, so it renders in the
     * SMALL face (~157 px worst case) on ONE unbroken line rather than
     * splitting. The only chrome surface besides the status strip. */
    fb_text_prop(fb, UI_MARGIN_X, y, u->info.mac, UI_FONT_SMALL, 1);
    y += UI_TEXT_LINE_H;
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
        case SCR_MENU:       render_menu(u, fb);       break;
        case SCR_RECORDINGS: render_recordings(u, fb); break;
        case SCR_ENTRY:      render_entry(u, fb);      break;
        case SCR_SETTINGS:   render_settings(u, fb);   break;
        default: break;
    }
}
