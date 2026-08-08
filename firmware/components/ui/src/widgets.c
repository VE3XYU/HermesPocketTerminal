#include "ui_widgets.h"
#include <string.h>
#include <stdio.h>

/* NOTE: font8x8_basic covers only ASCII (U+0000..U+007F); fb_text maps any
 * byte > 127 to '?'. The "dim row" leading marker described in the design
 * ("a leading middle dot") is therefore rendered with the ASCII period
 * below rather than the non-ASCII character, since the shared font has no
 * glyph for it. widgets.c must never include fonts/font8x8_basic.h itself
 * (Task 8 carried-forward constraint) -- all text goes through fb_text.
 */

/* The vertical budget must partition the panel exactly (see ui_widgets.h):
 * the list's last row (and its cursor inversion) ends at the banner's top
 * edge, so neither can ever paint over the other. */
_Static_assert(UI_STATUS_H + (UI_LIST_ROWS + 1) * UI_ROW_H + UI_BANNER_H == UI_H,
               "status + title + rows + banner must partition the 200 px height");
/* Text pages stay clear of the banner strip by construction. */
_Static_assert(UI_STATUS_H + 4 + (UI_TEXT_PAGE_LINES - 1) * UI_TEXT_LINE_H + 8 * UI_TEXT_SCALE
                   <= UI_H - UI_BANNER_H,
               "a full text page must fit above the banner strip");

void ui_ellipsize(char *dst, size_t cap, const char *src, int max_chars) {
    if (cap == 0) return;
    if (max_chars < 0) max_chars = 0;
    size_t maxc = (size_t)max_chars;
    if (maxc > cap - 1) maxc = cap - 1;

    size_t len = strlen(src);
    if (len <= maxc) {
        memcpy(dst, src, len);
        dst[len] = '\0';
        return;
    }

    size_t keep = (maxc >= 3) ? maxc - 3 : 0;
    size_t dots = maxc - keep;
    if (dots > 3) dots = 3;
    memcpy(dst, src, keep);
    memcpy(dst + keep, "...", dots);
    dst[keep + dots] = '\0';
}

void widget_status_line(ui_fb_t *f, const ui_status_t *st) {
    char buf[24];

    /* Clock first (right-aligned): it always fits alone. */
    int limit = UI_W - 2;
    if (st->clock_hhmm[0]) {
        int w = fb_text_width(st->clock_hhmm, UI_TEXT_SCALE);
        int cx = UI_W - 2 - w;
        fb_text(f, cx, 2, st->clock_hhmm, UI_TEXT_SCALE, 1);
        limit = cx - 4;
    }

    /* Left cluster in priority order -- battery, pending uploads, wifi.
     * At scale 2 a fully crowded strip ("100%" + "^32" + "W" + clock) is
     * wider than the panel, so each item is drawn only when it fits
     * entirely before `limit`: lower-priority items drop whole, nothing
     * is ever clipped mid-glyph. */
    int x = 2;
    if (st->battery_pct != -1) {
        snprintf(buf, sizeof buf, "%d%%", st->battery_pct);
        int w = fb_text_width(buf, UI_TEXT_SCALE);
        if (x + w <= limit) { fb_text(f, x, 2, buf, UI_TEXT_SCALE, 1); x += w + 4; }
    }
    if (st->pending_uploads > 0) {
        snprintf(buf, sizeof buf, "^%d", st->pending_uploads);
        int w = fb_text_width(buf, UI_TEXT_SCALE);
        if (x + w <= limit) { fb_text(f, x, 2, buf, UI_TEXT_SCALE, 1); x += w + 4; }
    }
    if (st->wifi_ok) {
        int w = fb_text_width("W", UI_TEXT_SCALE);
        if (x + w <= limit) fb_text(f, x, 2, "W", UI_TEXT_SCALE, 1);
    }

    fb_hline(f, 0, UI_STATUS_H - 1, UI_W, 1);
}

void widget_list(ui_fb_t *f, const ui_list_t *l) {
    char title[UI_LINE_CHARS + 1];
    ui_ellipsize(title, sizeof title, l->title, UI_LINE_CHARS);
    int title_y = UI_STATUS_H + 3;
    fb_text(f, 2, title_y, title, UI_TEXT_SCALE, 1);
    fb_text(f, 3, title_y, title, UI_TEXT_SCALE, 1); /* bold: redraw offset by 1px */

    int max_start = l->row_count - UI_LIST_ROWS;
    int start = l->cursor - UI_LIST_ROWS + 1;
    if (start > max_start) start = max_start;
    if (start < 0) start = 0;

    int visible = l->row_count - start;
    if (visible > UI_LIST_ROWS) visible = UI_LIST_ROWS;
    if (visible < 0) visible = 0;

    for (int k = 0; k < visible; k++) {
        int idx = start + k;
        const ui_row_t *row = &l->rows[idx];
        int y = UI_STATUS_H + UI_ROW_H * (k + 1);

        /* 12 chars per row at scale 2; a dim marker costs 2 of them. */
        int budget = UI_LINE_CHARS - (row->dim ? 2 : 0);
        char body[UI_LINE_CHARS + 1];
        ui_ellipsize(body, sizeof body, row->text, budget);

        char line[UI_LINE_CHARS + 3];
        size_t p = 0;
        if (row->dim) { line[p++] = '.'; line[p++] = ' '; }
        size_t blen = strlen(body);
        memcpy(line + p, body, blen);
        p += blen;
        line[p] = '\0';

        fb_text(f, 2, y + 3, line, UI_TEXT_SCALE, 1);

        /* Done rows are struck through rather than prefixed: a "[x] "
         * marker would cost a third of the 12-char budget, and the
         * strike-through is what the completion gesture promises. Two
         * rows thick so it survives partial-refresh ghosting. */
        if (row->done && line[0])
            fb_fill(f, 2, y + 3 + 7, fb_text_width(line, UI_TEXT_SCALE), 2, 1);

        if (idx == l->cursor) fb_invert(f, 0, y, UI_W, UI_ROW_H);
    }
}

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n'; }

#define TEXT_LINE_CHARS UI_LINE_CHARS

/* Word-wraps `text` into TEXT_LINE_CHARS-wide lines (hard-breaking words
 * longer than a line), drawing only lines within [first_line, last_line)
 * at y = UI_STATUS_H + 4 + row*UI_TEXT_LINE_H. f may be NULL for a pure
 * count pass. Returns the total line count regardless of the requested
 * window, so callers can derive a page count. */
static int wrap_lines(ui_fb_t *f, const char *text, int first_line, int last_line) {
    char line[TEXT_LINE_CHARS + 1];
    int col = 0;
    int line_index = 0;
    int y0 = UI_STATUS_H + 4;

    size_t i = 0;
    while (text[i]) {
        while (is_ws(text[i])) i++;
        if (!text[i]) break;

        size_t wstart = i;
        while (text[i] && !is_ws(text[i])) i++;
        size_t wlen = i - wstart;
        size_t wpos = 0;

        while (wpos < wlen) {
            int need_space = (col > 0) ? 1 : 0;
            int avail = TEXT_LINE_CHARS - col - need_space;
            if (avail <= 0) {
                line[col] = '\0';
                if (f && line_index >= first_line && line_index < last_line)
                    fb_text(f, 2, y0 + (line_index - first_line) * UI_TEXT_LINE_H,
                            line, UI_TEXT_SCALE, 1);
                line_index++;
                col = 0;
                continue; /* re-evaluate need_space/avail with a fresh line */
            }
            size_t remaining = wlen - wpos;
            size_t take = remaining < (size_t)avail ? remaining : (size_t)avail;
            if (need_space) line[col++] = ' ';
            memcpy(line + col, text + wstart + wpos, take);
            col += (int)take;
            wpos += take;
        }
    }
    if (col > 0) {
        line[col] = '\0';
        if (f && line_index >= first_line && line_index < last_line)
            fb_text(f, 2, y0 + (line_index - first_line) * UI_TEXT_LINE_H,
                    line, UI_TEXT_SCALE, 1);
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

void widget_banner(ui_fb_t *f, const char *text) {
    int y0 = UI_H - UI_BANNER_H;
    fb_fill(f, 0, y0, UI_W, UI_BANNER_H, 1);

    /* One scale-2 line: 12 chars is what the strip holds, so the text is
     * ellipsized to fit rather than wrapped (the dismiss gesture is a
     * glance-and-clear interaction, not a reading surface). */
    char line[UI_LINE_CHARS + 1];
    ui_ellipsize(line, sizeof line, text, UI_LINE_CHARS);
    fb_text(f, 4, y0 + 5, line, UI_TEXT_SCALE, 0);
}
