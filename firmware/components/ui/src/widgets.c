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
    int x = 2;

    if (st->battery_pct != -1) {
        snprintf(buf, sizeof buf, "%d%%", st->battery_pct);
        fb_text(f, x, 4, buf, 1, 1);
        x += fb_text_width(buf, 1) + 4;
    }
    if (st->wifi_ok) {
        fb_text(f, x, 4, "W", 1, 1);
        x += fb_text_width("W", 1) + 4;
    }
    if (st->pending_uploads > 0) {
        snprintf(buf, sizeof buf, "^%d", st->pending_uploads);
        fb_text(f, x, 4, buf, 1, 1);
        x += fb_text_width(buf, 1) + 4;
    }
    if (st->clock_hhmm[0]) {
        int w = fb_text_width(st->clock_hhmm, 1);
        fb_text(f, UI_W - 2 - w, 4, st->clock_hhmm, 1, 1);
    }

    fb_hline(f, 0, UI_STATUS_H - 1, UI_W, 1);
}

void widget_list(ui_fb_t *f, const ui_list_t *l) {
    int title_y = UI_STATUS_H + 3;
    fb_text(f, 2, title_y, l->title, 1, 1);
    fb_text(f, 3, title_y, l->title, 1, 1); /* bold: redraw offset by 1px */

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

        char body[64];
        ui_ellipsize(body, sizeof body, row->text, 22);

        char line[96];
        size_t p = 0;
        if (row->dim) { line[p++] = '.'; line[p++] = ' '; }
        if (row->done) { memcpy(line + p, "[x] ", 4); p += 4; }
        size_t blen = strlen(body);
        if (p + blen >= sizeof line) blen = sizeof(line) - p - 1;
        memcpy(line + p, body, blen);
        p += blen;
        line[p] = '\0';

        fb_text(f, 2, y + 3, line, 1, 1);

        if (idx == l->cursor) fb_invert(f, 0, y, UI_W, UI_ROW_H);
    }
}

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n'; }

#define TEXT_LINE_CHARS 24
#define TEXT_LINES_PER_PAGE 12
#define TEXT_LINE_H UI_ROW_H

/* Word-wraps `text` into TEXT_LINE_CHARS-wide lines (hard-breaking words
 * longer than a line), drawing only lines within [first_line, last_line)
 * at y = UI_STATUS_H + 4 + row*TEXT_LINE_H. Returns the total line count
 * regardless of the requested window, so callers can derive a page count. */
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
                if (line_index >= first_line && line_index < last_line)
                    fb_text(f, 2, y0 + (line_index - first_line) * TEXT_LINE_H, line, 1, 1);
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
        if (line_index >= first_line && line_index < last_line)
            fb_text(f, 2, y0 + (line_index - first_line) * TEXT_LINE_H, line, 1, 1);
        line_index++;
    }
    return line_index;
}

int widget_text_page(ui_fb_t *f, const char *text, int page) {
    int total_lines = wrap_lines(f, text, 0, 0); /* count only: empty window draws nothing */
    int pages = (total_lines + TEXT_LINES_PER_PAGE - 1) / TEXT_LINES_PER_PAGE;
    if (pages < 1) pages = 1;

    int first_line = page * TEXT_LINES_PER_PAGE;
    int last_line = first_line + TEXT_LINES_PER_PAGE;
    wrap_lines(f, text, first_line, last_line);
    return pages;
}

void widget_banner(ui_fb_t *f, const char *text) {
    int y0 = UI_H - UI_BANNER_H;
    fb_fill(f, 0, y0, UI_W, UI_BANNER_H, 1);

    size_t len = strlen(text);
    size_t split = len;
    if (len > TEXT_LINE_CHARS) {
        split = TEXT_LINE_CHARS;
        for (size_t i = TEXT_LINE_CHARS; i > 0; i--) {
            if (text[i - 1] == ' ') { split = i - 1; break; }
        }
    }

    char line1[TEXT_LINE_CHARS + 1];
    memcpy(line1, text, split);
    line1[split] = '\0';

    const char *rest = text + split;
    while (*rest == ' ') rest++;
    char line2[TEXT_LINE_CHARS + 1];
    ui_ellipsize(line2, sizeof line2, rest, TEXT_LINE_CHARS);

    fb_text(f, 2, y0 + 4, line1, 1, 0);
    if (line2[0]) fb_text(f, 2, y0 + 4 + 10, line2, 1, 0);
}
