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

    /* crowded status: at scale 2, "100%" + "^32" + "W" + clock is wider than
     * the panel, so lower-priority items must drop whole -- the 4px gap
     * left of the right-aligned clock stays empty (nothing bleeds into it,
     * nothing is clipped mid-glyph) */
    fb_clear(&fb);
    ui_status_t crowded = { .battery_pct = 100, .wifi_ok = 1, .pending_uploads = 32,
                            .clock_hhmm = "10:15" };
    widget_status_line(&fb, &crowded);
    int clock_x = UI_W - 2 - 5 * 8 * UI_TEXT_SCALE;       /* "10:15" right-aligned */
    CHECK(region_ink(clock_x, 0, UI_W - clock_x, UI_STATUS_H - 1) > 0);   /* clock drew */
    CHECK_EQ_INT(region_ink(clock_x - 4, 0, 4, UI_STATUS_H - 1), 0);      /* gap respected */
    CHECK_EQ_INT(region_ink(0, UI_STATUS_H, UI_W, UI_H - UI_STATUS_H), 0);

    /* priority-inversion regression: after "100%" the gap before the clock
     * is wide enough for "W" (16px) but not for "^32" (48px) -- pending
     * must drop *and* nothing lower-priority may draw in the space it left
     * behind, or "W" appears where the dropped pending indicator belongs,
     * inverting the battery -> pending -> wifi hierarchy. */
    int batt_w = 4 * 8 * UI_TEXT_SCALE;                    /* "100%" */
    int pend_w = 3 * 8 * UI_TEXT_SCALE;                    /* "^32" */
    int wifi_w = 1 * 8 * UI_TEXT_SCALE;                    /* "W" */
    int batt_end = 2 + batt_w + 4;                         /* x after battery + gap */
    int gap_limit = clock_x - 4;
    CHECK(batt_end + wifi_w <= gap_limit);   /* sanity: "W" alone would fit here */
    CHECK(batt_end + pend_w > gap_limit);    /* sanity: "^32" does not fit here */
    CHECK_EQ_INT(region_ink(batt_end, 0, wifi_w, UI_STATUS_H - 1), 0);   /* "W" did not
        draw in the gap the dropped "^32" left behind */

    /* list: title, rows at fixed positions, cursor row inverted (heavy ink) */
    fb_clear(&fb);
    ui_list_t l = { .row_count = 3, .cursor = 1 };
    strcpy(l.title, "Today");
    strcpy(l.rows[0].text, "Buy milk");
    strcpy(l.rows[1].text, "Call dentist");
    strcpy(l.rows[2].text, "Water plants");
    widget_list(&fb, &l);
    int row0 = region_ink(0, UI_STATUS_H + UI_ROW_H, UI_W, UI_ROW_H);      /* row 0 after title row */
    int row1 = region_ink(0, UI_STATUS_H + 2 * UI_ROW_H, UI_W, UI_ROW_H);  /* cursor row */
    int row2 = region_ink(0, UI_STATUS_H + 3 * UI_ROW_H, UI_W, UI_ROW_H);
    CHECK(row0 > 0);
    CHECK(row1 > row0 * 3);          /* inversion floods the cursor row with ink */

    /* done rows get a strike-through (more ink than the same row plain) */
    l.rows[2].done = 1;
    fb_clear(&fb);
    widget_list(&fb, &l);
    CHECK(region_ink(0, UI_STATUS_H + 3 * UI_ROW_H, UI_W, UI_ROW_H) > row2);

    /* scrolling: cursor 15 of 20 keeps the cursor row visible, and the
     * scale-2 rows (title + UI_LIST_ROWS at UI_ROW_H) exactly fill the
     * space above the banner -- the strip below stays untouched, cursor
     * inversion included */
    ui_list_t big = { .row_count = 20, .cursor = 15 };
    strcpy(big.title, "T");
    for (int i = 0; i < 20; i++) snprintf(big.rows[i].text, 64, "item %d", i);
    fb_clear(&fb);
    widget_list(&fb, &big);
    CHECK(fb_count_black(&fb) > 0);   /* rendered without crash; window math in unit below */
    CHECK_EQ_INT(region_ink(0, UI_H - UI_BANNER_H, UI_W, UI_BANNER_H), 0);

    /* text page: pagination is deterministic at UI_LINE_CHARS chars x
     * UI_TEXT_PAGE_LINES lines per page */
    fb_clear(&fb);
    char longtext[2048];
    for (int i = 0; i < 2000; i++) longtext[i] = (i % 50 == 49) ? ' ' : 'a' + (i % 26);
    longtext[2000] = 0;
    int pages = widget_text_page(&fb, longtext, 0);
    /* wrap can only add lines over the perfect packing, never remove them */
    CHECK(pages >= 2000 / (UI_LINE_CHARS * UI_TEXT_PAGE_LINES));
    CHECK(fb_count_black(&fb) > 0);
    CHECK_EQ_INT(region_ink(0, UI_H - UI_BANNER_H, UI_W, UI_BANNER_H), 0);  /* stays clear of the banner strip */
    fb_clear(&fb);
    int pages2 = widget_text_page(&fb, longtext, pages - 1);
    CHECK_EQ_INT(pages, pages2);
    CHECK_EQ_INT(widget_text_pages(longtext), pages);   /* count-only helper agrees */

    /* banner: inverted strip at the bottom, one ellipsized scale-2 line */
    fb_clear(&fb);
    widget_banner(&fb, "Meeting with Alex at 10:00 AM");
    int strip = region_ink(0, UI_H - UI_BANNER_H, UI_W, UI_BANNER_H);
    CHECK(strip > UI_W * UI_BANNER_H / 2);   /* mostly black (inverted) */
    CHECK_EQ_INT(region_ink(0, 0, UI_W, UI_H - UI_BANNER_H), 0);
    return HARNESS_REPORT();
}
