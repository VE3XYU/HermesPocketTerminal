#include "ui_widgets.h"
#include <string.h>
#include <stdio.h>

/* NOTE: all text is proportional (Liberation Sans ramp via fb_text_prop;
 * printable ASCII U+0020..U+007E, anything outside renders '?'). Fitting
 * is measured per pixel -- fb_ellipsize_prop / fb_text_fit_prop -- never
 * per character count. The "dim row" leading marker described in the
 * design ("a leading middle dot") is rendered with the ASCII period,
 * since the font carries no glyph for the non-ASCII character. widgets.c
 * must never include the font headers itself (Task 8 carried-forward
 * constraint) -- all text goes through the fb layer.
 */

/* The vertical budget must partition the panel exactly (see ui_widgets.h):
 * the one-line grid fills the height below the title, and the banner strip
 * IS its last row band (a banner costs one visible row while it is up --
 * ui_list_t.reserve_banner -- and is only ever drawn over the dashboard);
 * the two-line grid owns the full height below the title (Recordings never
 * shows a banner -- a reserved strip would cost a preview row). */
_Static_assert(UI_STATUS_H + UI_TITLE_H + UI_LIST_ROWS * UI_ROW_H == UI_H,
               "status + title + rows must partition the 200 px height");
_Static_assert(UI_BANNER_H == UI_ROW_H,
               "the banner strip is exactly the last one-line row band");
_Static_assert(UI_STATUS_H + UI_TITLE_H + UI_LIST2_ROWS * UI_ROW2_H == UI_H,
               "the two-line row grid must partition the full 200 px height");
/* Every baseline keeps its font's full ascent/descent band inside its
 * row or strip. */
_Static_assert(UI_STATUS_BASE >= UI_FONT_SMALL_ASC &&
               UI_STATUS_BASE + UI_FONT_SMALL_DESC <= UI_STATUS_H - 1,
               "status text must clear the divider row");
_Static_assert(UI_TITLE_BASE >= UI_FONT_EMPH_ASC &&
               UI_TITLE_BASE + UI_FONT_EMPH_DESC <= UI_TITLE_H,
               "title ink stays inside the title row");
_Static_assert(UI_ROW_BASE >= UI_FONT_BODY_ASC &&
               UI_ROW_BASE + UI_FONT_BODY_DESC <= UI_ROW_H,
               "body ink stays inside a one-line row");
_Static_assert(UI_ROW2_BASE1 >= UI_FONT_BODY_ASC &&
               UI_ROW2_BASE2 + UI_FONT_BODY_DESC <= UI_ROW2_H &&
               UI_ROW2_BASE2 - UI_ROW2_BASE1 == UI_TEXT_LINE_H,
               "two body lines at the text pitch stay inside a two-line row");
_Static_assert(UI_BANNER_BASE >= UI_FONT_BODY_ASC &&
               UI_BANNER_BASE + UI_FONT_BODY_DESC <= UI_BANNER_H,
               "banner ink stays inside the banner strip");

/* C7 round 7, finding 3 -- the cramped-highlight rule, on every band that
 * can carry an inverted bar or sit under one: at least UI_ROW_CLEAR px of
 * clear space above the cap top and below the baseline. Containment
 * (above) is about ink not escaping the band; this is about the band not
 * hugging the ink. */
#define UI_ROW_CLEAR 3
_Static_assert(UI_ROW_BASE - UI_FONT_BODY_CAP >= UI_ROW_CLEAR &&
               UI_ROW_H - UI_ROW_BASE >= UI_ROW_CLEAR,
               "a one-line row must breathe above the cap and below the baseline");
_Static_assert(UI_ROW2_BASE1 - UI_FONT_BODY_CAP >= UI_ROW_CLEAR &&
               UI_ROW2_H - UI_ROW2_BASE2 >= UI_ROW_CLEAR,
               "a two-line row must breathe above line 1 and below line 2");
_Static_assert(UI_BANNER_BASE - UI_FONT_BODY_CAP >= UI_ROW_CLEAR &&
               UI_BANNER_H - UI_BANNER_BASE >= UI_ROW_CLEAR,
               "the banner strip must breathe like a row");
/* Rows and the banner under them share one text column. */
_Static_assert(UI_ROW_PAD_X == UI_BANNER_PAD,
               "list rows and the banner must start at the same x");
/* Text pages fill the height below the status strip (no banner is ever
 * drawn over an entry). */
_Static_assert(UI_TEXT_FIRST_BASE - UI_FONT_BODY_ASC >= UI_STATUS_H &&
               UI_TEXT_FIRST_BASE + (UI_TEXT_PAGE_LINES - 1) * UI_TEXT_LINE_H
                   + UI_FONT_BODY_DESC < UI_H,
               "a full text page must fit on the panel");

void widget_status_line(ui_fb_t *f, const ui_status_t *st) {
    char buf[24];

    /* The strip renders in the SMALL face (the pre-round-6 body): it is
     * chrome -- numbers and single letters, not prose -- and holding it
     * at the reference size keeps the strip at 20 px, which buys the
     * dashboard a whole extra row over escalating it with the body. */

    /* Clock first (right-aligned by measured width): it always fits alone. */
    int limit = UI_W - UI_MARGIN_X;
    if (st->clock_hhmm[0]) {
        int w = fb_text_width_prop(st->clock_hhmm, UI_FONT_SMALL);
        int cx = UI_W - UI_MARGIN_X - w;
        fb_text_prop(f, cx, UI_STATUS_BASE, st->clock_hhmm, UI_FONT_SMALL, 1);
        limit = cx - UI_STATUS_GAP;
    }

    /* Left cluster in priority order -- battery, pending uploads, wifi.
     * Each item is drawn only when its measured width fits entirely
     * before `limit`: lower-priority items drop whole, nothing is ever
     * clipped mid-glyph, and once an item is dropped, nothing after it
     * may draw either -- otherwise a lower-priority item can fit in the
     * gap a higher-priority one left behind, inverting the hierarchy.
     * (Every realistic strip -- "100%" + "^<int max>" + "W" + clock --
     * fits outright at the body size, so the drop path is a guard, not
     * an expectation; the host test derives both facts.) */
    int x = UI_MARGIN_X;
    int stop = 0;
    if (!stop && st->battery_pct != -1) {
        snprintf(buf, sizeof buf, "%d%%", st->battery_pct);
        int w = fb_text_width_prop(buf, UI_FONT_SMALL);
        if (x + w <= limit) { fb_text_prop(f, x, UI_STATUS_BASE, buf, UI_FONT_SMALL, 1); x += w + UI_STATUS_GAP; }
        else stop = 1;
    }
    if (!stop && st->pending_uploads > 0) {
        snprintf(buf, sizeof buf, "^%d", st->pending_uploads);
        int w = fb_text_width_prop(buf, UI_FONT_SMALL);
        if (x + w <= limit) { fb_text_prop(f, x, UI_STATUS_BASE, buf, UI_FONT_SMALL, 1); x += w + UI_STATUS_GAP; }
        else stop = 1;
    }
    if (!stop && st->wifi_ok) {
        int w = fb_text_width_prop("W", UI_FONT_SMALL);
        if (x + w <= limit) fb_text_prop(f, x, UI_STATUS_BASE, "W", UI_FONT_SMALL, 1);
    }

    fb_hline(f, 0, UI_STATUS_H - 1, UI_W, 1);
}

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n'; }

/* One body line of a list row: ellipsized to the pixel budget, struck
 * through when done (the strike is what the completion gesture promises;
 * two rows thick so it survives partial-refresh ghosting). */
static void row_line(ui_fb_t *f, int x, int baseline, const char *text,
                     int max_w, int done) {
    char line[sizeof ((ui_row_t *)0)->text];
    fb_ellipsize_prop(line, sizeof line, text, UI_FONT_BODY, max_w);
    if (!line[0]) return;
    fb_text_prop(f, x, baseline, line, UI_FONT_BODY, 1);
    if (done)
        fb_fill(f, x, baseline - 5, fb_text_width_prop(line, UI_FONT_BODY), 2, 1);
}

void widget_list(ui_fb_t *f, const ui_list_t *l) {
    /* The title shares the rows' left inset so the whole list reads as one
     * column (C7 round 7, finding 3). */
    char title[sizeof l->title];
    fb_ellipsize_prop(title, sizeof title, l->title, UI_FONT_EMPH, UI_ROW_TEXT_W);
    fb_text_prop(f, UI_ROW_PAD_X, UI_STATUS_H + UI_TITLE_BASE, title,
                 UI_FONT_EMPH, 1);

    /* One-line rows (dashboard, menu) or two-line rows (recordings
     * previews): same scroll-window arithmetic, parametrized by the row
     * grid. On the one-line grid a banner takes the last row band, so the
     * window is one row shorter while one is up. */
    int rows_max = l->two_line ? UI_LIST2_ROWS
                              : UI_LIST_ROWS - (l->reserve_banner ? 1 : 0);
    int row_h    = l->two_line ? UI_ROW2_H    : UI_ROW_H;

    int max_start = l->row_count - rows_max;
    int start = l->cursor - rows_max + 1;
    if (start > max_start) start = max_start;
    if (start < 0) start = 0;

    int visible = l->row_count - start;
    if (visible > rows_max) visible = rows_max;
    if (visible < 0) visible = 0;

    for (int k = 0; k < visible; k++) {
        int idx = start + k;
        const ui_row_t *row = &l->rows[idx];
        int y = UI_STATUS_H + UI_TITLE_H + row_h * k;

        /* A dim row leads with a measured ". " marker; the text budget
         * shrinks by its width. */
        int x = UI_ROW_PAD_X;
        int max_w = UI_ROW_TEXT_W;
        int base1 = y + (l->two_line ? UI_ROW2_BASE1 : UI_ROW_BASE);
        if (row->dim) {
            fb_text_prop(f, x, base1, ". ", UI_FONT_BODY, 1);
            int dw = fb_text_width_prop(". ", UI_FONT_BODY);
            x += dw;
            max_w -= dw;
        }

        if (!l->two_line) {
            row_line(f, x, base1, row->text, max_w, row->done);
        } else {
            /* Split at a WORD BOUNDARY across the row's two body lines
             * (C7 round 6: the photo-verified "buy a ne / w computer"
             * failure): line 1 ends at the last blank that fits, and
             * only a single word wider than the whole line hard-splits.
             * Only the second line carries the ellipsis. */
            int fit = fb_wrap_break_prop(row->text, UI_FONT_BODY, max_w);
            char seg[sizeof row->text];
            memcpy(seg, row->text, (size_t)fit);
            seg[fit] = '\0';
            if (seg[0]) {
                fb_text_prop(f, x, base1, seg, UI_FONT_BODY, 1);
                if (row->done)
                    fb_fill(f, x, base1 - 5,
                            fb_text_width_prop(seg, UI_FONT_BODY), 2, 1);
            }
            const char *rest = row->text + fit;
            while (is_ws(*rest)) rest++;        /* no leading gap on line 2 */
            if (*rest)
                row_line(f, x, y + UI_ROW2_BASE2, rest, max_w, row->done);
        }

        if (idx == l->cursor) fb_invert(f, 0, y, UI_W, row_h);
    }
}

/* Longest body-line buffer: UI_TEXT_W px at the body's narrowest advance
 * (5 px) is 39 characters. */
#define WRAP_BUF 80

static void wrap_emit(ui_fb_t *f, const char *line, int line_index,
                      int first_line, int last_line) {
    if (f && line_index >= first_line && line_index < last_line)
        fb_text_prop(f, UI_MARGIN_X,
                     UI_TEXT_FIRST_BASE + (line_index - first_line) * UI_TEXT_LINE_H,
                     line, UI_FONT_BODY, 1);
}

/* Word-wraps `text` into UI_TEXT_W-px lines by measured width (hard-
 * splitting words longer than a line), drawing only lines within
 * [first_line, last_line) at the page baselines. f may be NULL for a
 * pure count pass. Returns the total line count regardless of the
 * requested window, so callers can derive a page count. */
static int wrap_lines(ui_fb_t *f, const char *text, int first_line, int last_line) {
    char line[WRAP_BUF];
    size_t len = 0;
    int line_index = 0;

    size_t i = 0;
    while (text[i]) {
        while (is_ws(text[i])) i++;
        if (!text[i]) break;

        size_t wstart = i;
        while (text[i] && !is_ws(text[i])) i++;
        size_t wlen = i - wstart;
        size_t wpos = 0;

        while (wpos < wlen) {
            size_t remaining = wlen - wpos;
            size_t need_space = (len > 0) ? 1 : 0;
            size_t room = sizeof line - 1 - len - need_space;
            size_t take = remaining < room ? remaining : room;

            if (take > 0) {
                size_t oldlen = len;
                if (need_space) line[len++] = ' ';
                memcpy(line + len, text + wstart + wpos, take);
                len += take;
                line[len] = '\0';
                if (fb_text_width_prop(line, UI_FONT_BODY) <= UI_TEXT_W) {
                    wpos += take;
                    if (wpos >= wlen) break;    /* word done, stay on line */
                    /* buffer-full mid-word (width still fine): flush */
                    wrap_emit(f, line, line_index, first_line, last_line);
                    line_index++;
                    len = 0;
                    continue;
                }
                len = oldlen;                   /* too wide: revert */
                line[len] = '\0';
            }

            if (len > 0) {
                /* the word starts a fresh line instead */
                wrap_emit(f, line, line_index, first_line, last_line);
                line_index++;
                len = 0;
                continue;
            }

            /* fresh line, word chunk still too wide: hard split at the
             * pixel budget (fit >= 1 guarantees progress) */
            {
                char tmp[WRAP_BUF];
                size_t t = remaining < sizeof tmp - 1 ? remaining : sizeof tmp - 1;
                memcpy(tmp, text + wstart + wpos, t);
                tmp[t] = '\0';
                int fit = fb_text_fit_prop(tmp, UI_FONT_BODY, UI_TEXT_W);
                if (fit < 1) fit = 1;
                tmp[fit] = '\0';
                wrap_emit(f, tmp, line_index, first_line, last_line);
                line_index++;
                wpos += (size_t)fit;
            }
        }
    }
    if (len > 0) {
        wrap_emit(f, line, line_index, first_line, last_line);
        line_index++;
    }
    return line_index;
}

static int pages_for(int total_lines) {
    int pages = (total_lines + UI_TEXT_PAGE_LINES - 1) / UI_TEXT_PAGE_LINES;
    return pages < 1 ? 1 : pages;
}

int widget_text_page(ui_fb_t *f, const char *text, int page) {
    int first_line = page * UI_TEXT_PAGE_LINES;
    int last_line = first_line + UI_TEXT_PAGE_LINES;
    return pages_for(wrap_lines(f, text, first_line, last_line));
}

int widget_text_pages(const char *text) {
    return pages_for(wrap_lines(NULL, text, 0, 0));
}

void widget_empty_state(ui_fb_t *f, const char *line1, const char *line2,
                        int y0, int y1) {
    /* Total ink band of two body lines at the text pitch. */
    int band = UI_FONT_BODY_ASC + UI_TEXT_LINE_H + UI_FONT_BODY_DESC;
    int base1 = y0 + (y1 - y0 - band) / 2 + UI_FONT_BODY_ASC;
    int w1 = fb_text_width_prop(line1, UI_FONT_BODY);
    int w2 = fb_text_width_prop(line2, UI_FONT_BODY);
    fb_text_prop(f, (UI_W - w1) / 2, base1, line1, UI_FONT_BODY, 1);
    fb_text_prop(f, (UI_W - w2) / 2, base1 + UI_TEXT_LINE_H, line2,
                 UI_FONT_BODY, 1);
}

void widget_banner(ui_fb_t *f, const char *text) {
    int y0 = UI_H - UI_BANNER_H;
    fb_fill(f, 0, y0, UI_W, UI_BANNER_H, 1);

    /* One BODY line -- a notification is a glance-and-clear interaction,
     * not a reading surface -- ellipsized to the strip's measured budget
     * rather than wrapped. Body, not emphasis, since the round-6
     * escalation: 17 px caps are the previous rounds' emphasis height,
     * and the 188 px budget still carries ~18 characters where the new
     * emphasis would carry ~13 and truncate almost everything. */
    char line[80];
    fb_ellipsize_prop(line, sizeof line, text, UI_FONT_BODY,
                      UI_W - 2 * UI_BANNER_PAD);
    fb_text_prop(f, UI_BANNER_PAD, y0 + UI_BANNER_BASE, line, UI_FONT_BODY, 0);
}
