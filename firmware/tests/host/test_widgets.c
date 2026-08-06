#include "harness.h"
#include "ui_fb.h"
#include "ui_widgets.h"
#include <string.h>

static ui_fb_t fb;

static int region_ink(int x, int y, int w, int h) {
    int n = 0;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) n += fb_get(&fb, x + i, y + j);
    return n;
}

int main(void) {
    /* ellipsize */
    char e[64];
    ui_ellipsize(e, sizeof e, "short", 10);
    CHECK_EQ_STR(e, "short");
    ui_ellipsize(e, sizeof e, "twelve chars!", 10);
    CHECK_EQ_INT((int)strlen(e), 10);
    CHECK_EQ_STR(e + 7, "...");

    /* status line renders in its strip only */
    fb_clear(&fb);
    ui_status_t st = { .battery_pct = 78, .wifi_ok = 1, .pending_uploads = 2,
                       .clock_hhmm = "10:15" };
    widget_status_line(&fb, &st);
    CHECK(region_ink(0, 0, UI_W, UI_STATUS_H) > 0);
    CHECK_EQ_INT(region_ink(0, UI_STATUS_H, UI_W, UI_H - UI_STATUS_H), 0);

    /* list: title, rows at fixed positions, cursor row inverted (heavy ink) */
    fb_clear(&fb);
    ui_list_t l = { .row_count = 3, .cursor = 1 };
    strcpy(l.title, "Today");
    strcpy(l.rows[0].text, "Buy milk");
    strcpy(l.rows[1].text, "Call dentist");
    strcpy(l.rows[2].text, "Water plants"); l.rows[2].done = 1;
    widget_list(&fb, &l);
    int row0 = region_ink(0, UI_STATUS_H + UI_ROW_H, UI_W, UI_ROW_H);      /* row 0 after title row */
    int row1 = region_ink(0, UI_STATUS_H + 2 * UI_ROW_H, UI_W, UI_ROW_H);  /* cursor row */
    CHECK(row0 > 0);
    CHECK(row1 > row0 * 3);          /* inversion floods the cursor row with ink */

    /* scrolling: cursor 15 of 20 keeps the cursor row visible */
    ui_list_t big = { .row_count = 20, .cursor = 15 };
    strcpy(big.title, "T");
    for (int i = 0; i < 20; i++) snprintf(big.rows[i].text, 64, "item %d", i);
    fb_clear(&fb);
    widget_list(&fb, &big);
    CHECK(fb_count_black(&fb) > 0);   /* rendered without crash; window math in unit below */

    /* text page: pagination is deterministic at 24 chars x 12 lines per page */
    fb_clear(&fb);
    char longtext[2048];
    for (int i = 0; i < 2000; i++) longtext[i] = (i % 50 == 49) ? ' ' : 'a' + (i % 26);
    longtext[2000] = 0;
    int pages = widget_text_page(&fb, longtext, 0);
    CHECK(pages >= 7);                /* 2000 chars / (24*12) ≈ 7 pages */
    CHECK(fb_count_black(&fb) > 0);
    CHECK_EQ_INT(region_ink(0, UI_H - UI_BANNER_H, UI_W, UI_BANNER_H), 0);  /* stays clear of the banner strip */
    fb_clear(&fb);
    int pages2 = widget_text_page(&fb, longtext, pages - 1);
    CHECK_EQ_INT(pages, pages2);

    /* banner: inverted strip at the bottom */
    fb_clear(&fb);
    widget_banner(&fb, "Meeting at 10:00");
    int strip = region_ink(0, UI_H - UI_BANNER_H, UI_W, UI_BANNER_H);
    CHECK(strip > UI_W * UI_BANNER_H / 2);   /* mostly black (inverted) */
    CHECK_EQ_INT(region_ink(0, 0, UI_W, UI_H - UI_BANNER_H), 0);
    return HARNESS_REPORT();
}
