#ifndef UI_WIDGETS_H
#define UI_WIDGETS_H

#include <stddef.h>
#include "ui_fb.h"

/* Layout constants are part of the contract.
 *
 * The panel is 1.54 inches across 200 px (~130 dpi): scale-1 8 px glyphs
 * are ~1.5 mm tall and were rejected as illegible on hardware, and the
 * square scale-2 8x8 glyphs (16 px wide) fit only 12 chars/line -- too
 * little information per line (C7 round-3 finding 5). Body text (list
 * items, transcripts, settings, the status strip) is therefore the
 * vendored Spleen 8x16 (fb_text16): the same 16 px height, half the
 * advance, 24 characters per line. Headlines ("Noted", "REC") stay on
 * the 8x8 font at scale UI_HEAD_SCALE = 24 px.
 *
 * The vertical budget partitions exactly -- with the same arithmetic for
 * both row heights, since UI_ROW2_H = 2 * UI_ROW_H:
 *
 *     UI_STATUS_H + (1 title + UI_LIST_ROWS)  * UI_ROW_H  + UI_BANNER_H
 *   =     20      + 22 +      6 * 22                      +     26      = 200
 *     UI_STATUS_H + UI_ROW_H + UI_LIST2_ROWS * UI_ROW2_H + UI_BANNER_H
 *   =     20      + 22 +      3 * 44                      +     26      = 200
 *
 * so the list (including the cursor-row inversion) can never paint into
 * the banner strip, and the banner never covers list content.
 */
#define UI_BODY_W      8     /* body glyph advance (Spleen 8x16, fb_text16) */
#define UI_BODY_H      16    /* body glyph height */
#define UI_HEAD_SCALE  3     /* headlines: 8x8 font at scale 3 = 24 px */
#define UI_LINE_CHARS  24    /* chars per body line: 24 * 8 = 192 px + margins */
#define UI_TEXT_LINE_H 20    /* body text pitch: 16 px glyph + 4 px leading */
#define UI_STATUS_H    20    /* status strip: y 0..19, body text, divider at y 19 */
#define UI_ROW_H       22    /* one-line list row: body text, 3 px top/bottom pad */
#define UI_ROW2_H      44    /* two-line list row (Recordings previews) */
#define UI_BANNER_H    26    /* one body line, y 174..199 */
#define UI_LIST_ROWS   6     /* visible one-line rows between title row and banner */
#define UI_LIST2_ROWS  3     /* visible two-line rows between title row and banner */
#define UI_TEXT_PAGE_LINES 7 /* transcript page: 7 lines x UI_LINE_CHARS chars */

typedef struct {
    int battery_pct;          /* -1 hides */
    int wifi_ok;              /* 0/1 */
    int pending_uploads;
    char clock_hhmm[6];       /* "" hides */
} ui_status_t;
void widget_status_line(ui_fb_t *f, const ui_status_t *st);

typedef struct { char text[64]; int done; int dim; } ui_row_t;
typedef struct {
    char title[48];
    ui_row_t rows[32]; int row_count;
    int cursor;               /* absolute index; widget scrolls the window */
    int two_line;             /* 1: UI_LIST2_ROWS rows of two body lines each
                                 (2 * UI_LINE_CHARS chars -- Recordings
                                 transcript previews); 0: UI_LIST_ROWS
                                 one-line rows */
} ui_list_t;
void widget_list(ui_fb_t *f, const ui_list_t *l);

/* Renders one page of wrapped body text (UI_LINE_CHARS chars per line,
 * UI_TEXT_PAGE_LINES lines per page). Returns total page count for the
 * given text. page is 0-based. */
int widget_text_page(ui_fb_t *f, const char *text, int page);

/* Page count only -- same wrap arithmetic as widget_text_page without a
 * framebuffer (cursor/page bookkeeping must not cost a 5 KB scratch). */
int widget_text_pages(const char *text);

void widget_banner(ui_fb_t *f, const char *text);   /* inverted strip at bottom */
void ui_ellipsize(char *dst, size_t cap, const char *src, int max_chars);

#endif
