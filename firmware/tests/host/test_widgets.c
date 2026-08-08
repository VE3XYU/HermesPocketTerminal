#include "harness.h"
#include "ui_fb.h"
#include "ui_widgets.h"
#include <limits.h>
#include <stdio.h>
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

    /* clock right-alignment and its 4 px gap, derived from UI_BODY_W */
    int clock_x = UI_W - 2 - 5 * UI_BODY_W;               /* "10:15" right-aligned */
    CHECK(region_ink(clock_x, 0, UI_W - clock_x, UI_STATUS_H - 1) > 0);   /* clock drew */
    CHECK_EQ_INT(region_ink(clock_x - 4, 0, 4, UI_STATUS_H - 1), 0);      /* gap respected */

    /* At the 8 px body advance, every realistic strip fits whole: battery
     * "100%" + the widest possible pending count + "W" end left of the
     * clock's limit. Derivation, not screenshot: widths are chars * UI_BODY_W. */
    char pend_widest[16];
    snprintf(pend_widest, sizeof pend_widest, "^%d", INT_MAX);
    int limit = clock_x - 4;
    int full_cluster_end = 2 + 4 * UI_BODY_W + 4                      /* "100%" */
                         + (int)strlen(pend_widest) * UI_BODY_W + 4   /* "^2147483647" */
                         + 1 * UI_BODY_W;                             /* "W" */
    CHECK(full_cluster_end <= limit);
    fb_clear(&fb);
    ui_status_t full = { .battery_pct = 100, .wifi_ok = 1, .pending_uploads = INT_MAX,
                         .clock_hhmm = "10:15" };
    widget_status_line(&fb, &full);
    /* wifi "W" drew at its computed slot -- nothing was dropped */
    int wifi_x = 2 + 4 * UI_BODY_W + 4 + (int)strlen(pend_widest) * UI_BODY_W + 4;
    CHECK(region_ink(wifi_x, 0, UI_BODY_W, UI_STATUS_H - 1) > 0);

    /* priority-inversion guard: when an item does not fit, nothing after it
     * may draw in the gap it left behind (battery -> pending -> wifi order).
     * The only in-contract way to contest the strip at the 8 px advance is
     * an absurd battery_pct -- the widget takes any int, so the guard stays
     * testable even though realistic strips always fit (see above). */
    char batt_absurd[16];
    snprintf(batt_absurd, sizeof batt_absurd, "%d%%", 12345678);
    int batt_end = 2 + (int)strlen(batt_absurd) * UI_BODY_W + 4;
    CHECK(batt_end + (int)strlen(pend_widest) * UI_BODY_W > limit);  /* pending won't fit */
    CHECK(batt_end + 1 * UI_BODY_W <= limit);                        /* "W" alone would */
    fb_clear(&fb);
    ui_status_t contested = { .battery_pct = 12345678, .wifi_ok = 1,
                              .pending_uploads = INT_MAX, .clock_hhmm = "10:15" };
    widget_status_line(&fb, &contested);
    CHECK(region_ink(2, 0, batt_end - 2 - 4, UI_STATUS_H - 1) > 0);  /* battery drew */
    CHECK_EQ_INT(region_ink(batt_end, 0, limit - batt_end, UI_STATUS_H - 1), 0);
    /* ^ neither the dropped pending indicator nor "W" drew after the drop */

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
     * one-line rows (title + UI_LIST_ROWS at UI_ROW_H) exactly fill the
     * space above the banner -- the strip below stays untouched, cursor
     * inversion included */
    ui_list_t big = { .row_count = 20, .cursor = 15 };
    strcpy(big.title, "T");
    for (int i = 0; i < 20; i++) snprintf(big.rows[i].text, 64, "item %d", i);
    fb_clear(&fb);
    widget_list(&fb, &big);
    CHECK(fb_count_black(&fb) > 0);   /* rendered without crash; window math in unit below */
    CHECK_EQ_INT(region_ink(0, UI_H - UI_BANNER_H, UI_W, UI_BANNER_H), 0);

    /* two-line rows (Recordings previews): a >UI_LINE_CHARS text spills
     * onto the row's second body line; UI_LIST2_ROWS rows at UI_ROW2_H
     * partition the same space, banner strip untouched even scrolled */
    ui_list_t two = { .row_count = 10, .cursor = 0, .two_line = 1 };
    strcpy(two.title, "Recordings");
    for (int i = 0; i < 10; i++)
        snprintf(two.rows[i].text, 64,
                 "recording %d transcript opening that runs well past one line", i);
    fb_clear(&fb);
    widget_list(&fb, &two);
    int r0_y = UI_STATUS_H + UI_ROW_H;
    CHECK(region_ink(0, r0_y + 3, UI_W, UI_BODY_H) > 0);                  /* line 1 */
    CHECK(region_ink(0, r0_y + 3 + UI_TEXT_LINE_H, UI_W, UI_BODY_H) > 0); /* line 2 */
    /* cursor inversion floods the full two-line row height */
    int inv = region_ink(0, r0_y, UI_W, UI_ROW2_H);
    CHECK(inv > UI_W * UI_ROW2_H / 2);
    /* scrolled: last visible row ends exactly at the banner's top edge */
    two.cursor = 7;
    fb_clear(&fb);
    widget_list(&fb, &two);
    CHECK(fb_count_black(&fb) > 0);
    CHECK_EQ_INT(region_ink(0, UI_H - UI_BANNER_H, UI_W, UI_BANNER_H), 0);

    /* short two-line rows draw only one line: the second band stays empty */
    ui_list_t two_short = { .row_count = 2, .cursor = 1, .two_line = 1 };
    strcpy(two_short.title, "Recordings");
    strcpy(two_short.rows[0].text, "(pending)");
    strcpy(two_short.rows[1].text, "(not uploaded)");
    fb_clear(&fb);
    widget_list(&fb, &two_short);
    CHECK(region_ink(0, r0_y + 3, UI_W, UI_BODY_H) > 0);
    CHECK_EQ_INT(region_ink(0, r0_y + 3 + UI_TEXT_LINE_H, UI_W, UI_BODY_H), 0);

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

    /* banner: inverted strip at the bottom, one ellipsized body line */
    fb_clear(&fb);
    widget_banner(&fb, "Meeting with Alex at 10:00 AM tomorrow morning sharp");
    int strip = region_ink(0, UI_H - UI_BANNER_H, UI_W, UI_BANNER_H);
    CHECK(strip > UI_W * UI_BANNER_H / 2);   /* mostly black (inverted) */
    CHECK_EQ_INT(region_ink(0, 0, UI_W, UI_H - UI_BANNER_H), 0);
    return HARNESS_REPORT();
}
