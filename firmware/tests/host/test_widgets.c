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
    /* status line renders in its strip only */
    fb_clear(&fb);
    ui_status_t st = { .battery_pct = 78, .wifi_ok = 1, .pending_uploads = 2,
                       .clock_hhmm = "10:15" };
    widget_status_line(&fb, &st);
    CHECK(region_ink(0, 0, UI_W, UI_STATUS_H) > 0);
    CHECK_EQ_INT(region_ink(0, UI_STATUS_H, UI_W, UI_H - UI_STATUS_H), 0);

    /* clock right-alignment by measured width, and its gap */
    int clock_x = UI_W - UI_MARGIN_X - fb_text_width_prop("10:15", UI_FONT_SMALL);
    CHECK(region_ink(clock_x, 0, UI_W - clock_x, UI_STATUS_H - 1) > 0);   /* clock drew */
    CHECK_EQ_INT(region_ink(clock_x - UI_STATUS_GAP, 0, UI_STATUS_GAP,
                            UI_STATUS_H - 1), 0);                         /* gap respected */

    /* Every realistic strip fits whole: battery "100%" + a realistic
     * pending count + "W" end left of the clock's limit. Derivation by
     * measured widths, not screenshot. */
    int limit = clock_x - UI_STATUS_GAP;
    int w_batt = fb_text_width_prop("100%", UI_FONT_SMALL);
    int w_pend = fb_text_width_prop("^9", UI_FONT_SMALL);
    int w_wifi = UI_WIFI_W;
    int full_cluster_end = UI_MARGIN_X + w_batt + UI_STATUS_GAP
                         + w_pend + UI_STATUS_GAP + w_wifi;
    CHECK(full_cluster_end <= limit);
    fb_clear(&fb);
    ui_status_t full = { .battery_pct = 100, .wifi_ok = 1, .pending_uploads = 9,
                         .clock_hhmm = "10:15" };
    widget_status_line(&fb, &full);
    /* the wifi glyph drew at its slot -- nothing was dropped */
    int wifi_x = UI_MARGIN_X + w_batt + UI_STATUS_GAP + w_pend + UI_STATUS_GAP;
    CHECK(region_ink(wifi_x, 0, w_wifi, UI_STATUS_H - 1) > 0);

    /* C7 round 7, finding 4 -- the wifi glyph is a real mark, present only
     * when the link is up, and it fits its cell exactly (no ink outside
     * UI_WIFI_W x UI_WIFI_H, none in the divider row). */
    {
        int cell = region_ink(wifi_x, UI_STATUS_BASE - UI_WIFI_H, UI_WIFI_W, UI_WIFI_H);
        CHECK(cell > 20);                       /* a fan, not a stray pixel */
        CHECK(cell < UI_WIFI_W * UI_WIFI_H);    /* and not a solid block */
        /* everything the glyph draws is inside the cell */
        CHECK_EQ_INT(region_ink(wifi_x, 0, w_wifi, UI_STATUS_H - 1), cell);
        /* the column right of the glyph is clear of it (clock aside) */
        CHECK_EQ_INT(region_ink(wifi_x + UI_WIFI_W, 0, limit - wifi_x - UI_WIFI_W,
                                UI_STATUS_H - 1), 0);
        /* the mark is bottom-heavy: the dot rows carry the solid stroke */
        CHECK(region_ink(wifi_x, UI_STATUS_BASE - 3, UI_WIFI_W, 3) == 9);

        /* wifi down: the same strip, minus exactly the glyph */
        fb_clear(&fb);
        ui_status_t down = full;
        down.wifi_ok = 0;
        widget_status_line(&fb, &down);
        CHECK_EQ_INT(region_ink(wifi_x, 0, w_wifi, UI_STATUS_H - 1), 0);
        fb_clear(&fb);
        widget_status_line(&fb, &full);
    }

    /* priority-inversion guard: when an item does not fit, nothing after
     * it may draw in the gap it left behind (battery -> pending -> wifi
     * order). At the proportional size an INT_MAX pending indicator
     * (in-contract: the field is an int) really is wider than the free
     * space, so the guard is exercised without leaving the contract. */
    char pend_widest[16];
    snprintf(pend_widest, sizeof pend_widest, "^%d", INT_MAX);
    int batt_end = UI_MARGIN_X + w_batt + UI_STATUS_GAP;
    CHECK(batt_end + fb_text_width_prop(pend_widest, UI_FONT_SMALL) > limit);
    CHECK(batt_end + w_wifi <= limit);           /* "W" alone would fit */
    fb_clear(&fb);
    ui_status_t contested = { .battery_pct = 100, .wifi_ok = 1,
                              .pending_uploads = INT_MAX, .clock_hhmm = "10:15" };
    widget_status_line(&fb, &contested);
    CHECK(region_ink(UI_MARGIN_X, 0, w_batt, UI_STATUS_H - 1) > 0);  /* battery drew */
    CHECK_EQ_INT(region_ink(batt_end, 0, limit - batt_end, UI_STATUS_H - 1), 0);
    /* ^ neither the dropped pending indicator nor "W" drew after the drop */

    /* list: title row, rows in their bands, cursor row inverted */
    fb_clear(&fb);
    ui_list_t l = { .row_count = 3, .cursor = 1 };
    strcpy(l.title, "Today");
    strcpy(l.rows[0].text, "Buy milk");
    strcpy(l.rows[1].text, "Call dentist");
    strcpy(l.rows[2].text, "Water plants");
    widget_list(&fb, &l);
    CHECK(region_ink(0, UI_STATUS_H, UI_W, UI_TITLE_H) > 0);   /* title (emphasis) */
    int rows_y = UI_STATUS_H + UI_TITLE_H;
    int row0 = region_ink(0, rows_y, UI_W, UI_ROW_H);
    int row1 = region_ink(0, rows_y + UI_ROW_H, UI_W, UI_ROW_H);      /* cursor row */
    int row2 = region_ink(0, rows_y + 2 * UI_ROW_H, UI_W, UI_ROW_H);
    CHECK(row0 > 0);
    CHECK(row1 > row0 * 3);          /* inversion floods the cursor row with ink */
    /* a row's ink stays inside its band: the bands below the ones with
     * text stay empty (baseline + ascent/descent contained) */
    CHECK_EQ_INT(region_ink(0, rows_y + 3 * UI_ROW_H, UI_W,
                            UI_H - (rows_y + 3 * UI_ROW_H)), 0);

    /* C7 round 7, finding 3 -- breathing room. The clear bands are
     * derived, not guessed: nothing may be drawn in the top
     * (UI_ROW_BASE - ascent) or bottom (UI_ROW_H - UI_ROW_BASE - descent)
     * rows of a band, and both must leave >= 3 px clear of the CAP and of
     * the baseline. The same bands hold on the INVERTED cursor row --
     * where they read as solid black -- so the column never shifts. */
    {
        int top_clear = UI_ROW_BASE - UI_FONT_BODY_ASC;
        int bot_clear = UI_ROW_H - UI_ROW_BASE - UI_FONT_BODY_DESC;
        CHECK(UI_ROW_BASE - UI_FONT_BODY_CAP >= 3);     /* above the cap */
        CHECK(UI_ROW_H - UI_ROW_BASE >= 3);             /* below the baseline */
        CHECK(top_clear >= 1 && bot_clear >= 1);
        CHECK_EQ_INT(region_ink(0, rows_y, UI_W, top_clear), 0);
        CHECK_EQ_INT(region_ink(0, rows_y + UI_ROW_H - bot_clear, UI_W, bot_clear), 0);
        int inv_y = rows_y + UI_ROW_H;                  /* the cursor row */
        CHECK_EQ_INT(region_ink(0, inv_y, UI_W, top_clear), UI_W * top_clear);
        CHECK_EQ_INT(region_ink(0, inv_y + UI_ROW_H - bot_clear, UI_W, bot_clear),
                     UI_W * bot_clear);
        /* left inset: the first UI_ROW_PAD_X columns carry no glyph ink on
         * a normal row and are solid black on the inverted one */
        CHECK_EQ_INT(region_ink(0, rows_y, UI_ROW_PAD_X, UI_ROW_H), 0);
        CHECK_EQ_INT(region_ink(0, inv_y, UI_ROW_PAD_X, UI_ROW_H),
                     UI_ROW_PAD_X * UI_ROW_H);
    }

    /* done rows get a strike-through (more ink than the same row plain) */
    l.rows[2].done = 1;
    fb_clear(&fb);
    widget_list(&fb, &l);
    CHECK(region_ink(0, rows_y + 2 * UI_ROW_H, UI_W, UI_ROW_H) > row2);

    /* a long row is ellipsized to the measured budget: no ink reaches the
     * right margin columns */
    fb_clear(&fb);
    ui_list_t wide = { .row_count = 1, .cursor = -1 };
    strcpy(wide.title, "T");
    strcpy(wide.rows[0].text,
           "an item text that is much wider than one hundred ninety six pixels");
    widget_list(&fb, &wide);
    CHECK(region_ink(0, rows_y, UI_W, UI_ROW_H) > 0);
    CHECK_EQ_INT(region_ink(UI_W - UI_ROW_PAD_X, rows_y, UI_ROW_PAD_X, UI_ROW_H), 0);

    /* scrolling: cursor 15 of 20 keeps the cursor row visible, and the
     * one-line grid (title + UI_LIST_ROWS at UI_ROW_H) fills the height
     * below the title -- the LAST band is a real row when no banner is up
     * (C7 round 7: the banner strip is that band, not a reservation) */
    ui_list_t big = { .row_count = 20, .cursor = 15 };
    strcpy(big.title, "T");
    for (int i = 0; i < 20; i++)
        snprintf(big.rows[i].text, sizeof big.rows[i].text, "item %d", i);
    fb_clear(&fb);
    widget_list(&fb, &big);
    CHECK(fb_count_black(&fb) > 0);   /* rendered without crash; window math in unit below */
    CHECK(region_ink(0, UI_H - UI_ROW_H, UI_W, UI_ROW_H) > 0);   /* 5th row drew */
    /* ...and with a banner up the list gives that band back: one fewer
     * visible row, nothing painted over */
    big.reserve_banner = 1;
    fb_clear(&fb);
    widget_list(&fb, &big);
    CHECK_EQ_INT(region_ink(0, UI_H - UI_BANNER_H, UI_W, UI_BANNER_H), 0);
    /* the cursor row stays visible: the window scrolled so it is the last
     * drawn band, inverted */
    CHECK(region_ink(0, UI_H - 2 * UI_ROW_H, UI_W, UI_ROW_H) > UI_W * UI_ROW_H / 2);
    big.reserve_banner = 0;

    /* two-line rows (Recordings previews): a text wider than one line
     * spills onto the row's second body line at the text pitch;
     * UI_LIST2_ROWS rows at UI_ROW2_H own the FULL height below the
     * title (no banner is ever drawn over Recordings -- round 6) */
    ui_list_t two = { .row_count = 10, .cursor = 0, .two_line = 1 };
    strcpy(two.title, "Recordings");
    for (int i = 0; i < 10; i++)
        snprintf(two.rows[i].text, sizeof two.rows[i].text,
                 "recording %d transcript opening that runs well past one line of "
                 "the panel and must continue", i);
    fb_clear(&fb);
    widget_list(&fb, &two);
    int r0_y = UI_STATUS_H + UI_TITLE_H;
    /* both body lines drew inside their ascent..descent bands */
    CHECK(region_ink(0, r0_y + UI_ROW2_BASE1 - UI_FONT_BODY_ASC, UI_W,
                     UI_FONT_BODY_ASC + UI_FONT_BODY_DESC) > 0);
    CHECK(region_ink(0, r0_y + UI_ROW2_BASE2 - UI_FONT_BODY_ASC, UI_W,
                     UI_FONT_BODY_ASC + UI_FONT_BODY_DESC) > 0);
    /* cursor inversion floods the full two-line row height */
    int inv = region_ink(0, r0_y, UI_W, UI_ROW2_H);
    CHECK(inv > UI_W * UI_ROW2_H / 2);
    /* scrolled: the last visible row ends exactly at the panel's bottom
     * edge -- the grid reclaims the old banner reservation (rows there
     * prove the 3 x 50 partition, nothing paints below y 199 by
     * construction of fb_pixel) */
    two.cursor = 7;
    fb_clear(&fb);
    widget_list(&fb, &two);
    CHECK(fb_count_black(&fb) > 0);
    CHECK(region_ink(0, UI_H - UI_BANNER_H, UI_W, UI_BANNER_H) > 0);

    /* two-line previews break at WORD boundaries (round 6, the photo-
     * verified "buy a ne / w computer" failure): line 1 ends at the last
     * space that fits -- no partial-word ink past it -- and the rest
     * lands on line 2 */
    {
        const char *txt = "Remind me to buy a new computer please";
        ui_list_t wbl = { .row_count = 1, .cursor = -1, .two_line = 1 };
        strcpy(wbl.title, "Recordings");
        strcpy(wbl.rows[0].text, txt);
        int br = fb_wrap_break_prop(txt, UI_FONT_BODY, UI_ROW_TEXT_W);
        CHECK(br < (int)strlen(txt));            /* it does wrap */
        CHECK_EQ_INT(txt[br], ' ');              /* and on a boundary */
        char l1[64];
        memcpy(l1, txt, (size_t)br); l1[br] = '\0';
        int w1 = fb_text_width_prop(l1, UI_FONT_BODY);
        int bh = UI_FONT_BODY_ASC + UI_FONT_BODY_DESC;
        fb_clear(&fb);
        widget_list(&fb, &wbl);
        int b1_y = r0_y + UI_ROW2_BASE1 - UI_FONT_BODY_ASC;
        CHECK(region_ink(UI_ROW_PAD_X, b1_y, w1, bh) > 0);
        /* the old hard split painted "ne" past the last space -- banned */
        CHECK_EQ_INT(region_ink(UI_ROW_PAD_X + w1, b1_y,
                                UI_W - UI_ROW_PAD_X - w1, bh), 0);
        CHECK(region_ink(0, r0_y + UI_ROW2_BASE2 - UI_FONT_BODY_ASC,
                         UI_W, bh) > 0);         /* rest continued on line 2 */

        /* a single 30-char word still hard-breaks onto line 2 */
        strcpy(wbl.rows[0].text, "abcdefghijklmnopqrstuvwxyzabcd");
        fb_clear(&fb);
        widget_list(&fb, &wbl);
        CHECK(region_ink(0, b1_y, UI_W, bh) > 0);
        CHECK(region_ink(0, r0_y + UI_ROW2_BASE2 - UI_FONT_BODY_ASC,
                         UI_W, bh) > 0);
    }

    /* short two-line rows draw only one line: the second band stays empty */
    ui_list_t two_short = { .row_count = 2, .cursor = 1, .two_line = 1 };
    strcpy(two_short.title, "Recordings");
    strcpy(two_short.rows[0].text, "(pending)");
    strcpy(two_short.rows[1].text, "(not uploaded)");
    fb_clear(&fb);
    widget_list(&fb, &two_short);
    CHECK(region_ink(0, r0_y + UI_ROW2_BASE1 - UI_FONT_BODY_ASC, UI_W,
                     UI_FONT_BODY_ASC + UI_FONT_BODY_DESC) > 0);
    CHECK_EQ_INT(region_ink(0, r0_y + UI_ROW2_BASE1 + UI_FONT_BODY_DESC, UI_W,
                            UI_ROW2_H - (UI_ROW2_BASE1 + UI_FONT_BODY_DESC)), 0);

    /* text page: pagination is deterministic; the measured-wrap line
     * count can only exceed the perfect pixel packing, never undercut it */
    fb_clear(&fb);
    char longtext[2048];
    for (int i = 0; i < 2000; i++) longtext[i] = (i % 50 == 49) ? ' ' : 'a' + (i % 26);
    longtext[2000] = 0;
    int pages = widget_text_page(&fb, longtext, 0);
    int total_w = fb_text_width_prop(longtext, UI_FONT_BODY);
    CHECK(pages >= (total_w / UI_TEXT_W) / UI_TEXT_PAGE_LINES);
    CHECK(fb_count_black(&fb) > 0);
    CHECK_EQ_INT(region_ink(0, 0, UI_W, UI_STATUS_H), 0);   /* clear of the status strip */
    /* a full page's 7th line lands in the reclaimed banner area (no
     * banner is ever drawn over an entry -- round 6): ink proves the
     * 7-line grid is actually used */
    CHECK(region_ink(0, UI_H - UI_BANNER_H, UI_W, UI_BANNER_H) > 0);
    /* no line paints past the measured budget */
    CHECK_EQ_INT(region_ink(UI_MARGIN_X + UI_TEXT_W, 0,
                            UI_W - UI_MARGIN_X - UI_TEXT_W, UI_H), 0);
    fb_clear(&fb);
    int pages2 = widget_text_page(&fb, longtext, pages - 1);
    CHECK_EQ_INT(pages, pages2);
    CHECK_EQ_INT(widget_text_pages(longtext), pages);   /* count-only helper agrees */

    /* empty-state hint (round 6): two body lines, centered both ways,
     * confined to [y0, y1) -- margins derived from measured widths */
    {
        fb_clear(&fb);
        widget_empty_state(&fb, "Nothing yet", "Hold REC to talk", 50, 170);
        int w_wide = fb_text_width_prop("Hold REC to talk", UI_FONT_BODY);
        int xw = (UI_W - w_wide) / 2;
        CHECK_EQ_INT(region_ink(0, 0, UI_W, 50), 0);        /* nothing above */
        CHECK_EQ_INT(region_ink(0, 170, UI_W, UI_H - 170), 0); /* nothing below */
        CHECK(region_ink(xw, 50, w_wide, 120) > 0);         /* the hint drew */
        CHECK_EQ_INT(region_ink(0, 50, xw, 120), 0);        /* left margin clean */
        CHECK_EQ_INT(region_ink(UI_W - xw, 50, xw, 120), 0);/* right margin clean */
    }

    /* banner: inverted strip at the bottom, one ellipsized body line
     * (body, not emphasis, since the round-6 escalation) */
    fb_clear(&fb);
    widget_banner(&fb, "Meeting with Alex at 10:00 AM tomorrow morning sharp");
    int strip = region_ink(0, UI_H - UI_BANNER_H, UI_W, UI_BANNER_H);
    CHECK(strip > UI_W * UI_BANNER_H / 2);   /* mostly black (inverted) */
    CHECK(strip < UI_W * UI_BANNER_H);       /* but the text cut white into it */
    CHECK_EQ_INT(region_ink(0, 0, UI_W, UI_H - UI_BANNER_H), 0);
    /* banner text never exceeds its strip: the side padding columns stay
     * solid black even for an over-long text */
    {
        int y0 = UI_H - UI_BANNER_H;
        CHECK_EQ_INT(region_ink(0, y0, UI_BANNER_PAD, UI_BANNER_H),
                     UI_BANNER_PAD * UI_BANNER_H);
        CHECK_EQ_INT(region_ink(UI_W - UI_BANNER_PAD, y0, UI_BANNER_PAD, UI_BANNER_H),
                     UI_BANNER_PAD * UI_BANNER_H);
    }
    return HARNESS_REPORT();
}
