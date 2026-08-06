#ifndef UI_WIDGETS_H
#define UI_WIDGETS_H

#include <stddef.h>
#include "ui_fb.h"

/* layout constants are part of the contract */
#define UI_STATUS_H   16      /* status line: y 0..15 */
#define UI_ROW_H      14      /* list rows, scale-1 text with 3px padding */
#define UI_BANNER_H   32      /* notification banner: bottom rows */
#define UI_LIST_ROWS  10      /* visible rows between status line and banner */

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

/* Renders one page of wrapped text (chars-per-line 24 at scale 1).
 * Returns total page count for the given text. page is 0-based. */
int widget_text_page(ui_fb_t *f, const char *text, int page);

void widget_banner(ui_fb_t *f, const char *text);   /* inverted strip at bottom */
void ui_ellipsize(char *dst, size_t cap, const char *src, int max_chars);

#endif
