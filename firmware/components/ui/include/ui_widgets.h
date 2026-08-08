#ifndef UI_WIDGETS_H
#define UI_WIDGETS_H

#include <stddef.h>
#include "ui_fb.h"

/* Layout constants are part of the contract.
 *
 * The panel is 1.54 inches across 200 px (~130 dpi): scale-1 8 px glyphs
 * are ~1.5 mm tall and were rejected as illegible on hardware. Body text
 * (list items, transcripts, settings) is therefore scale 2 -- 16 px
 * glyphs, 12 characters per line -- and the vertical budget partitions
 * exactly:
 *
 *     UI_STATUS_H + (1 title + UI_LIST_ROWS) * UI_ROW_H + UI_BANNER_H
 *   =     20      +          7 * 22                     +     26      = 200
 *
 * so the list (including the cursor-row inversion) can never paint into
 * the banner strip, and the banner never covers list content.
 */
#define UI_TEXT_SCALE  2     /* body text: 16 px glyphs */
#define UI_LINE_CHARS  12    /* chars per scale-2 line: 12 * 16 = 192 px + margins */
#define UI_TEXT_LINE_H 20    /* scale-2 text pitch: 16 px glyph + 4 px leading */
#define UI_STATUS_H    20    /* status strip: y 0..19, scale-2 text, divider at y 19 */
#define UI_ROW_H       22    /* list rows: scale-2 text with 3 px top/bottom padding */
#define UI_BANNER_H    26    /* one scale-2 line, y 174..199 */
#define UI_LIST_ROWS   6     /* visible rows between the title row and the banner */
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
} ui_list_t;
void widget_list(ui_fb_t *f, const ui_list_t *l);

/* Renders one page of wrapped text (UI_LINE_CHARS chars per line at scale
 * UI_TEXT_SCALE, UI_TEXT_PAGE_LINES lines per page). Returns total page
 * count for the given text. page is 0-based. */
int widget_text_page(ui_fb_t *f, const char *text, int page);

/* Page count only -- same wrap arithmetic as widget_text_page without a
 * framebuffer (cursor/page bookkeeping must not cost a 5 KB scratch). */
int widget_text_pages(const char *text);

void widget_banner(ui_fb_t *f, const char *text);   /* inverted strip at bottom */
void ui_ellipsize(char *dst, size_t cap, const char *src, int max_chars);

#endif
