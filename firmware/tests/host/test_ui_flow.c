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

static void fresh(void) {
    ft_init(&ft, &tr); htp_client_init(&cl, &tr, "tok");
    fstore_init(&fs, &st); fkv_init(&fk, &kv);
    ui_flow_init(&u, &cl, &st, &kv);
    /* seed a dashboard model as the wake path would */
    u.dash.item_count = 2;
    str_copy(u.dash.title, sizeof u.dash.title, "Today");
    str_copy(u.dash.items[0].id, 32, "t-9f2");  str_copy(u.dash.items[0].text, 64, "Buy milk");
    str_copy(u.dash.items[1].id, 32, "t-c41");  str_copy(u.dash.items[1].text, 64, "Call dentist");
    /* seed two recordings */
    sidecar_t a; sidecar_init(&a, "c-old");
    str_copy(a.state, sizeof a.state, "done");
    str_copy(a.transcript, sizeof a.transcript, "the older recording words");
    sidecar_save(&st, &a); rec_index_append(&st, "c-old");
    sidecar_t b; sidecar_init(&b, "c-new");        /* not uploaded, no transcript */
    sidecar_save(&st, &b); rec_index_append(&st, "c-new");
    /* newest capture on the card is a conversation turn ("hey hermes"):
     * it must stay out of the Recordings list (protocol-level mode
     * distinction via conversation_id, finding 4) while remaining on the
     * card for sync/upload */
    sidecar_t c; sidecar_init(&c, "c-conv");
    str_copy(c.conversation_id, sizeof c.conversation_id, "conv-7");
    str_copy(c.transcript, sizeof c.transcript, "words spoken to hermes");
    sidecar_save(&st, &c); rec_index_append(&st, "c-conv");
}

int main(void) {
    fresh();
    CHECK_EQ_INT(u.screen, SCR_DASHBOARD);

    /* cursor moves and wraps; actions are partial redraws */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.cursor, 1);
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.cursor, 0);

    /* complete under cursor */
    ft_push_fixture(&ft, 200, "complete_post.json");
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_REC_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.dash.items[0].done, 1);
    CHECK(strstr(ft.req[0].body, "t-9f2") != NULL);

    /* banner dismiss takes precedence over complete */
    str_copy(u.banner, sizeof u.banner, "Meeting at 10:00");
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_REC_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_STR(u.banner, "");
    CHECK_EQ_INT(u.dash.items[0].done, 1);    /* unchanged, no second complete call */
    CHECK_EQ_INT(ft.req_count, 1);

    /* screen cycling is a partial (C7 refresh policy: within-session updates
     * never flash; fulls are first-draw or ghost-clear only) */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_DOUBLE, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.screen, SCR_RECORDINGS);
    CHECK_EQ_INT(u.cursor, 0);

    /* conversation-capture filter (finding 4): three captures on the card,
     * two browsable -- the cursor wraps at 2, so "c-conv" (newest, but a
     * conversation turn) is not in the list and cursor 0 is "c-new" */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.cursor, 1);
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.cursor, 0);

    /* recordings render newest-first with placeholders */
    ui_flow_render(&u, &fb);
    CHECK(fb_count_black(&fb) > 0);

    /* open entry (cursor 0 = c-new, NOT the filtered c-conv) -- partial,
     * same policy */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_LONG, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.screen, SCR_ENTRY);
    CHECK_EQ_STR(u.entry_id, "c-new");
    /* paging: "(no transcript yet)" (19 chars) fits one UI_LINE_CHARS body
     * line, well inside one page, so PWR-short wraps back to page 0 */
    CHECK_EQ_INT(widget_text_pages("(no transcript yet)"), 1);
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.entry_page, 0);
    /* play from entry */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_REC_SHORT, path), UIF_PLAY_WAV);
    CHECK_EQ_STR(path, "/rec/c-new.wav");
    /* back out -- partial, same policy */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_LONG, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.screen, SCR_RECORDINGS);

    /* settings via cycle; capture and power-off pass through from anywhere */
    ui_flow_gesture(&u, GEST_PWR_DOUBLE, path);
    CHECK_EQ_INT(u.screen, SCR_SETTINGS);
    /* settings render: values hard-wrap at UI_LINE_CHARS; everything
     * stays above the banner strip (body budget: 5 lines max) */
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
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_REC_HOLD_START, path), UIF_START_CAPTURE);
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_OFF, path), UIF_POWER_OFF);

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
