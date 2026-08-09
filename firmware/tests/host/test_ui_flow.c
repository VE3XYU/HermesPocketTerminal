#include "harness.h"
#include "ui_flow.h"
#include "ui_fb.h"
#include "sidecar.h"
#include "rec_index.h"
#include "fakes/fake_transport.h"
#include "fakes/fake_storage.h"
#include "fakes/fake_kv.h"
#include <string.h>

size_t str_copy(char *, size_t, const char *);

static fake_transport_t ft; static htp_transport_t tr; static htp_client_t cl;
static fake_storage_t fs; static port_storage_t st;
static fake_kv_t fk; static port_kv_t kv;
static ui_flow_t u; static ui_fb_t fb; static char path[96];

/* long enough to wrap past one 7-line page (widget_text_pages asserts so) */
static const char *LONG_TRANSCRIPT =
    "the quick brown fox jumps over the lazy dog and keeps running "
    "the quick brown fox jumps over the lazy dog and keeps running "
    "the quick brown fox jumps over the lazy dog and keeps running "
    "the quick brown fox jumps over the lazy dog and keeps running "
    "the quick brown fox jumps over the lazy dog and keeps running "
    "the quick brown fox jumps over the lazy dog and keeps running "
    "the quick brown fox jumps over the lazy dog and keeps running "
    "the quick brown fox jumps over the lazy dog and keeps running";

static void fresh(void) {
    ft_init(&ft, &tr); htp_client_init(&cl, &tr, "tok");
    fstore_init(&fs, &st); fkv_init(&fk, &kv);
    ui_flow_init(&u, &cl, &st, &kv);
    /* seed a dashboard model as the wake path would */
    u.dash.item_count = 2;
    str_copy(u.dash.title, sizeof u.dash.title, "Today");
    str_copy(u.dash.items[0].id, 32, "t-9f2");  str_copy(u.dash.items[0].text, 64, "Buy milk");
    str_copy(u.dash.items[1].id, 32, "t-c41");  str_copy(u.dash.items[1].text, 64, "Call dentist");
    /* seed two recordings: c-old has a multi-page transcript, c-new none */
    sidecar_t a; sidecar_init(&a, "c-old");
    str_copy(a.state, sizeof a.state, "done");
    str_copy(a.transcript, sizeof a.transcript, LONG_TRANSCRIPT);
    sidecar_save(&st, &a); rec_index_append(&st, "c-old");
    sidecar_t b; sidecar_init(&b, "c-new");        /* not uploaded, no transcript */
    sidecar_save(&st, &b); rec_index_append(&st, "c-new");
    /* newest capture on the card is a conversation turn ("hey hermes"):
     * it must stay out of the Recordings list (protocol-level mode
     * distinction via conversation_id, round-3 finding 4) while remaining
     * on the card for sync/upload */
    sidecar_t c; sidecar_init(&c, "c-conv");
    str_copy(c.conversation_id, sizeof c.conversation_id, "conv-7");
    str_copy(c.transcript, sizeof c.transcript, "words spoken to hermes");
    sidecar_save(&st, &c); rec_index_append(&st, "c-conv");
}

int main(void) {
    fresh();

    /* ---- resting display state: wake shows the dashboard, no cursor ---- */
    CHECK_EQ_INT(u.screen, SCR_DASHBOARD);
    CHECK(u.cursor < 0);
    /* a REC tap at rest is idle (reference grammar): no click, no redraw */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_REC_SHORT, path), UIF_NONE);
    CHECK_EQ_INT(u.click, UI_CLICK_NONE);

    /* ---- PWR tap at rest opens the MENU ---- */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.screen, SCR_MENU);
    CHECK_EQ_INT(u.cursor, MENU_DASHBOARD);
    CHECK_EQ_INT(u.click, UI_CLICK_NEXT);
    ui_flow_render(&u, &fb);                       /* menu renders with a cursor row */
    CHECK(fb_count_black(&fb) > 0);

    /* menu: PWR tap advances and wraps, each with a NEXT click */
    for (int want = 1; want <= MENU_COUNT; want++) {
        CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_SHORT, path), UIF_REDRAW_PARTIAL);
        CHECK_EQ_INT(u.cursor, want % MENU_COUNT);
        CHECK_EQ_INT(u.click, UI_CLICK_NEXT);
    }

    /* ---- REC tap selects: Dashboard opens with a visible cursor ---- */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_REC_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.screen, SCR_DASHBOARD);
    CHECK_EQ_INT(u.cursor, 0);
    CHECK_EQ_INT(u.click, UI_CLICK_SELECT);

    /* cursor moves and wraps across the 2 items */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.cursor, 1);
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.cursor, 0);

    /* ---- complete under cursor: click-then-POST split ---- */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_REC_SHORT, path), UIF_COMPLETE);
    CHECK_EQ_INT(u.click, UI_CLICK_SELECT);
    CHECK_EQ_INT(u.dash.items[0].done, 0);         /* nothing happened yet */
    ft_push_fixture(&ft, 200, "complete_post.json");
    CHECK_EQ_INT(ui_flow_complete_cursor(&u), 1);
    CHECK_EQ_INT(u.dash.items[0].done, 1);
    CHECK(strstr(ft.req[0].body, "t-9f2") != NULL);
    /* an already-struck item is not re-completable: identical redraw banned */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_REC_SHORT, path), UIF_NONE);
    CHECK_EQ_INT(u.click, UI_CLICK_NONE);
    CHECK_EQ_INT(ft.req_count, 1);
    /* a failed POST changes nothing (caller then skips the repaint) */
    u.cursor = 1;
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_REC_SHORT, path), UIF_COMPLETE);
    ft_push(&ft, 0, 500, "{\"error\":\"boom\"}");
    CHECK_EQ_INT(ui_flow_complete_cursor(&u), 0);
    CHECK_EQ_INT(u.dash.items[1].done, 0);
    u.cursor = 0;

    /* ---- banner dismiss takes precedence ON THE DASHBOARD only ---- */
    str_copy(u.banner, sizeof u.banner, "Meeting at 10:00");
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_REC_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_STR(u.banner, "");
    CHECK_EQ_INT(u.click, UI_CLICK_SELECT);
    CHECK_EQ_INT(u.dash.items[0].done, 1);         /* no second complete call */
    CHECK_EQ_INT(ft.req_count, 2);

    /* ---- the uniform <=1 rule: nowhere visible to advance -> up a level ---- */
    u.dash.item_count = 1;
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.screen, SCR_MENU);              /* not an invisible wrap-in-place */
    CHECK_EQ_INT(u.cursor, MENU_DASHBOARD);
    u.dash.item_count = 2;

    /* a pending banner must NOT eat a menu select (it isn't drawn here) */
    str_copy(u.banner, sizeof u.banner, "Later meeting");
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_REC_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.screen, SCR_DASHBOARD);         /* the select happened */
    CHECK_EQ_STR(u.banner, "Later meeting");       /* banner survives for the dashboard */
    u.banner[0] = '\0';

    /* ---- menu -> Recordings ---- */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_LONG, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.screen, SCR_MENU);              /* long-PWR = up to the menu */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.cursor, MENU_RECORDINGS);
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_REC_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.screen, SCR_RECORDINGS);
    CHECK_EQ_INT(u.cursor, 0);

    /* conversation filter (round-3 finding 4): three captures on the card,
     * two browsable -- the cursor wraps at 2, so "c-conv" is not listed */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.cursor, 1);
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.cursor, 0);

    /* ---- the session cache: cursor moves + renders cost ZERO reads ----
     * (before round 5 every keypress re-read the index + up to 30 sidecars
     * across three call sites -- the measured ~90 file opens per press) */
    ui_flow_render(&u, &fb);                       /* builds the cache at most once */
    CHECK(fb_count_black(&fb) > 0);
    {
        int reads_before = fs.read_count;
        ui_flow_gesture(&u, GEST_PWR_SHORT, path); /* cursor 1 */
        ui_flow_render(&u, &fb);
        ui_flow_gesture(&u, GEST_PWR_SHORT, path); /* cursor 0 */
        ui_flow_render(&u, &fb);
        CHECK_EQ_INT(fs.read_count, reads_before); /* O(1) I/O per keypress */
        ui_flow_rec_invalidate();                  /* capture/sync event... */
        ui_flow_render(&u, &fb);
        CHECK(fs.read_count > reads_before);       /* ...forces one rebuild */
    }

    /* ---- REC tap opens the entry under the cursor (c-new, newest kept) ---- */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_REC_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.screen, SCR_ENTRY);
    CHECK_EQ_STR(u.entry_id, "c-new");
    CHECK_EQ_INT(u.click, UI_CLICK_SELECT);
    /* "(no transcript yet)" is one page: a PWR tap has nowhere to page, so
     * it climbs back to the list instead of redrawing identical content */
    CHECK_EQ_INT(widget_text_pages("(no transcript yet)"), 1);
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.screen, SCR_RECORDINGS);
    CHECK_EQ_INT(u.click, UI_CLICK_NEXT);

    /* re-open: play/stop and back-out */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_REC_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.screen, SCR_ENTRY);
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_REC_SHORT, path), UIF_PLAY_WAV);
    CHECK_EQ_STR(path, "/rec/c-new.wav");
    CHECK_EQ_INT(u.click, UI_CLICK_SELECT);
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_LONG, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.screen, SCR_RECORDINGS);

    /* multi-page entry: c-old's transcript wraps past one page and PWR
     * taps page through and wrap around (a visible change every tap) */
    CHECK(widget_text_pages(LONG_TRANSCRIPT) >= 2);
    ui_flow_gesture(&u, GEST_PWR_SHORT, path);     /* cursor 1 = c-old */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_REC_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_STR(u.entry_id, "c-old");
    ui_flow_render(&u, &fb);
    CHECK_EQ_INT(u.entry_page, 0);
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.entry_page, 1);
    CHECK_EQ_INT(u.screen, SCR_ENTRY);             /* multi-page: stays in the entry */
    while (u.entry_page != 0)                      /* wraps back to page 0 */
        CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.screen, SCR_ENTRY);

    /* long-PWR climbs: entry -> recordings -> menu (cursor remembers) */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_LONG, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.screen, SCR_RECORDINGS);
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_LONG, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.screen, SCR_MENU);
    CHECK_EQ_INT(u.cursor, MENU_RECORDINGS);

    /* ---- menu -> Settings ---- */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.cursor, MENU_SETTINGS);
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_REC_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.screen, SCR_SETTINGS);
    /* settings render: values hard-split per measured pixel width;
     * everything stays above the banner strip (body budget: 5 lines max) */
    str_copy(u.info.mac, sizeof u.info.mac, "AA:BB:CC:DD:EE:FF");
    str_copy(u.info.fw_version, sizeof u.info.fw_version, "fw-test");
    str_copy(u.info.bridge_host, sizeof u.info.bridge_host, "bridge.example.net:8787");
    u.info.sync_interval_s = 600;
    ui_flow_render(&u, &fb);
    CHECK(fb_count_black(&fb) > 0);
    {
        int banner_ink = 0;
        for (int y = UI_H - UI_BANNER_H; y < UI_H; y++)
            for (int x = 0; x < UI_W; x++) banner_ink += fb_get(&fb, x, y);
        CHECK_EQ_INT(banner_ink, 0);
    }
    /* settings has one position: REC selects nothing, PWR climbs to menu */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_REC_SHORT, path), UIF_NONE);
    CHECK_EQ_INT(u.click, UI_CLICK_NONE);
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.screen, SCR_MENU);
    CHECK_EQ_INT(u.cursor, MENU_SETTINGS);

    /* ---- capture and power-off pass through from anywhere, clickless
     * (each has its own feedback: the REC glyph / the countdown + wipe) ---- */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_REC_HOLD_START, path), UIF_START_CAPTURE);
    CHECK_EQ_INT(u.click, UI_CLICK_NONE);
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_OFF, path), UIF_POWER_OFF);
    CHECK_EQ_INT(u.click, UI_CLICK_NONE);

    /* ---- menu Sleep: rests the display first (the retained sleep image
     * is the dashboard, never the menu) ---- */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.cursor, MENU_SLEEP);
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_REC_SHORT, path), UIF_SLEEP);
    CHECK_EQ_INT(u.click, UI_CLICK_SELECT);
    CHECK_EQ_INT(u.screen, SCR_DASHBOARD);
    CHECK(u.cursor < 0);

    /* ui_flow_rest reports whether it changed anything */
    CHECK_EQ_INT(ui_flow_rest(&u), 0);             /* already resting */
    ui_flow_gesture(&u, GEST_PWR_SHORT, path);     /* -> menu */
    CHECK_EQ_INT(ui_flow_rest(&u), 1);

    /* long-PWR on the menu backs out to the resting display */
    ui_flow_gesture(&u, GEST_PWR_SHORT, path);     /* -> menu again */
    CHECK_EQ_INT(u.screen, SCR_MENU);
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_LONG, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.screen, SCR_DASHBOARD);
    CHECK(u.cursor < 0);

    /* ---- empty Recordings: says so, and PWR climbs out ---- */
    {
        fstore_init(&fs, &st); fkv_init(&fk, &kv);   /* wipe the card */
        ui_flow_init(&u, &cl, &st, &kv);
        sidecar_t c; sidecar_init(&c, "c-conv2");    /* only a conversation turn */
        str_copy(c.conversation_id, sizeof c.conversation_id, "conv-8");
        sidecar_save(&st, &c); rec_index_append(&st, "c-conv2");
        ui_flow_gesture(&u, GEST_PWR_SHORT, path);   /* rest -> menu */
        ui_flow_gesture(&u, GEST_PWR_SHORT, path);   /* -> Recordings row */
        CHECK_EQ_INT(ui_flow_gesture(&u, GEST_REC_SHORT, path), UIF_REDRAW_PARTIAL);
        CHECK_EQ_INT(u.screen, SCR_RECORDINGS);
        ui_flow_render(&u, &fb);        /* title + centered empty-state hint */
        CHECK(fb_count_black(&fb) > 0);
        {   /* the hint lands in the rows region, below the title band */
            int hint = 0;
            for (int y = UI_STATUS_H + UI_TITLE_H; y < UI_H; y++)
                for (int x = 0; x < UI_W; x++) hint += fb_get(&fb, x, y);
            CHECK(hint > 0);
        }
        CHECK_EQ_INT(ui_flow_gesture(&u, GEST_REC_SHORT, path), UIF_NONE);
        CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_SHORT, path), UIF_REDRAW_PARTIAL);
        CHECK_EQ_INT(u.screen, SCR_MENU);
        CHECK_EQ_INT(u.cursor, MENU_RECORDINGS);
    }

    /* ---- empty dashboard (round 6, finding 3): title retained, centered
     * hint in the rows region -- an obviously deliberate screen, not a
     * bare one. Round 7 retired the banner strip's PERMANENT reservation:
     * with no banner up, the hint now centers across the FULL rows
     * region (UI_H - UI_STATUS_H - UI_TITLE_H = 150 px), not the old
     * fixed 120 px window that always left the banner band untouched
     * whether or not a banner was actually pending.
     *
     * Re-derived from widget_empty_state's math (band = ASC 18 +
     * LINE_H 24 + DESC 5 = 47 px): base1 = y0 + (y1-y0-band)/2 + ASC
     *   = 50 + (150-47)/2 + 18 = 119
     * so line 1's baseline is 119 and line 2's is 143 (+UI_TEXT_LINE_H);
     * with the descender band the ink spans y in [101,148] -- comfortably
     * inside the old 120 px window and nowhere near y=170. The old
     * "banner_ink == 0" assertion therefore still holds, but for a
     * different reason than before the fix: not because the strip is
     * reserved (it isn't, with no banner up), just because two short
     * centered lines don't reach that far down even in the taller
     * region. ---- */
    {
        u.dash.item_count = 0;
        str_copy(u.dash.title, sizeof u.dash.title, "Today");
        ui_flow_rest(&u);
        ui_flow_render(&u, &fb);
        int title_ink = 0, hint_ink = 0, tail_ink = 0;
        for (int y = UI_STATUS_H; y < UI_STATUS_H + UI_TITLE_H; y++)
            for (int x = 0; x < UI_W; x++) title_ink += fb_get(&fb, x, y);
        for (int y = UI_STATUS_H + UI_TITLE_H; y < UI_H; y++)
            for (int x = 0; x < UI_W; x++) hint_ink += fb_get(&fb, x, y);
        for (int y = UI_H - UI_BANNER_H; y < UI_H; y++)
            for (int x = 0; x < UI_W; x++) tail_ink += fb_get(&fb, x, y);
        CHECK(title_ink > 0);        /* "Today" is still there */
        CHECK(hint_ink > 0);         /* "Nothing yet" / "Hold REC to talk", full region */
        CHECK_EQ_INT(tail_ink, 0);   /* still empty -- not reserved, just unreached */

        /* Discriminators for the exact centering bug (Task 19): centered
         * over the FULL 150 px rows region, the hint's ink spans
         * y [101,148) (derivation above); mis-centered in the old fixed
         * 120 px window it would span [86,133). The plain hint_ink > 0
         * check passes either way -- zero ink above y 101 plus ink in
         * [134,148) is what tells the two apart. */
        {
            int above = 0, low = 0;
            for (int y = UI_STATUS_H + UI_TITLE_H; y < 101; y++)
                for (int x = 0; x < UI_W; x++) above += fb_get(&fb, x, y);
            for (int y = 134; y < 148; y++)
                for (int x = 0; x < UI_W; x++) low += fb_get(&fb, x, y);
            CHECK_EQ_INT(above, 0);
            CHECK(low > 0);
        }

        /* ---- same empty dashboard, but with a banner up: the fix keeps
         * the OLD fixed 120 px window in this case (y1 is UI_H -
         * UI_BANNER_H whenever u->banner[0] is set), so the hint is
         * confined above the banner strip and the banner itself owns the
         * bottom band, matching the pre-fix geometry exactly. ---- */
        str_copy(u.banner, sizeof u.banner, "Sync complete");
        ui_flow_render(&u, &fb);
        int hint_ink2 = 0, banner_ink = 0;
        for (int y = UI_STATUS_H + UI_TITLE_H; y < UI_H - UI_BANNER_H; y++)
            for (int x = 0; x < UI_W; x++) hint_ink2 += fb_get(&fb, x, y);
        for (int y = UI_H - UI_BANNER_H; y < UI_H; y++)
            for (int x = 0; x < UI_W; x++) banner_ink += fb_get(&fb, x, y);
        CHECK(hint_ink2 > 0);                        /* hint still shows, confined above the banner */
        CHECK(banner_ink > UI_W * UI_BANNER_H / 2);  /* the banner's inverted strip, not the hint */
        u.banner[0] = '\0';
    }

    /* ---- C7 round 7: the banner strip IS the list's last row band ----
     * With no banner the dashboard uses all UI_LIST_ROWS bands; with one
     * up it renders UI_LIST_ROWS - 1 rows and the banner takes the band
     * the dropped row vacated. Nothing is ever painted over. */
    {
        ui_flow_init(&u, &cl, &st, &kv);
        u.dash.item_count = UI_LIST_ROWS + 1;   /* more items than bands */
        str_copy(u.dash.title, sizeof u.dash.title, "Today");
        for (int i = 0; i < u.dash.item_count; i++)
            snprintf(u.dash.items[i].text, 64, "item number %d", i);
        u.cursor = 0;

        int last_y = UI_H - UI_ROW_H;
        ui_flow_render(&u, &fb);
        int rows_only = 0;
        for (int y = last_y; y < UI_H; y++)
            for (int x = 0; x < UI_W; x++) rows_only += fb_get(&fb, x, y);
        CHECK(rows_only > 0);                   /* the 5th row drew there */
        CHECK(rows_only < UI_W * UI_ROW_H / 2); /* as text, not an inverted strip */

        str_copy(u.banner, sizeof u.banner, "Meeting with Alex at 10:00");
        ui_flow_render(&u, &fb);
        int with_banner = 0;
        for (int y = last_y; y < UI_H; y++)
            for (int x = 0; x < UI_W; x++) with_banner += fb_get(&fb, x, y);
        CHECK(with_banner > UI_W * UI_ROW_H / 2);   /* now the inverted banner */
        CHECK(with_banner < UI_W * UI_ROW_H);       /* with its text cut white */
        /* the 4th row is still real content, not overpainted */
        {
            int fourth = 0;
            for (int y = last_y - UI_ROW_H; y < last_y; y++)
                for (int x = 0; x < UI_W; x++) fourth += fb_get(&fb, x, y);
            CHECK(fourth > 0);
            CHECK(fourth < UI_W * UI_ROW_H / 2);
        }
        u.banner[0] = '\0';
        fresh();
    }

    /* dashboard content diff (C7 finding B): a bridge rev bump with
     * pixel-identical content must not repaint. rev and item ids are NOT
     * content -- they never touch the panel. */
    {
        htp_dashboard_t a, b;
        memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
        a.item_count = b.item_count = 2;
        str_copy(a.title, sizeof a.title, "Today");
        str_copy(a.rev, sizeof a.rev, "r-1");
        str_copy(a.items[0].id, 32, "t-9f2"); str_copy(a.items[0].text, 64, "Buy milk");
        a.items[0].done = 1;
        str_copy(a.items[1].id, 32, "t-c41"); str_copy(a.items[1].text, 64, "Call dentist");
        str_copy(a.items[1].style, 12, "dim");
        b = a;
        CHECK_EQ_INT(ui_dash_content_equal(&a, &b), 1);
        /* the load-bearing case: only rev + ids changed (agent republish) */
        str_copy(b.rev, sizeof b.rev, "r-2");
        str_copy(b.items[0].id, 32, "t-000");
        CHECK_EQ_INT(ui_dash_content_equal(&a, &b), 1);
        b = a; b.items[0].done = 0;                       /* strike state differs */
        CHECK_EQ_INT(ui_dash_content_equal(&a, &b), 0);
        b = a; str_copy(b.items[1].text, 64, "Call mom"); /* text differs */
        CHECK_EQ_INT(ui_dash_content_equal(&a, &b), 0);
        b = a; b.items[1].style[0] = '\0';                /* style differs (dim marker) */
        CHECK_EQ_INT(ui_dash_content_equal(&a, &b), 0);
        b = a; b.item_count = 1;                          /* item removed */
        CHECK_EQ_INT(ui_dash_content_equal(&a, &b), 0);
        b = a; str_copy(b.title, sizeof b.title, "Later"); /* title differs */
        CHECK_EQ_INT(ui_dash_content_equal(&a, &b), 0);
    }

    /* refresh discipline: exactly one ghost-clear full per
     * UI_GHOST_CLEAR_EVERY partials, from a reset counter */
    CHECK(ui_flow_wants_full(&u, UIF_REDRAW_FULL));   /* explicit full: 1, resets */
    int fulls = 0;
    for (int i = 0; i < UI_GHOST_CLEAR_EVERY; i++)
        if (ui_flow_wants_full(&u, UIF_REDRAW_PARTIAL)) fulls++;
    CHECK_EQ_INT(fulls, 1);
    CHECK_EQ_INT(u.partial_count, 0);   /* the ghost-clear full reset the budget */
    return HARNESS_REPORT();
}
