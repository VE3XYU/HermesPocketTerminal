#ifndef UI_WIDGETS_H
#define UI_WIDGETS_H

#include <stddef.h>
#include "ui_fb.h"
#include "ui_font_metrics.h"

/* Layout constants are part of the contract.
 *
 * The type ramp is the fourth bench escalation (C7 round 6) of what
 * began as the shipped product's FreeSans ramp; reading text now runs a
 * full step LARGER than that reference, and the reference-sized body
 * survives as a "small" chrome role. Generated from Liberation Sans
 * (fonts/, SIL OFL 1.1):
 *
 *   role   font           cap  asc  desc  used for
 *   small  UI_FONT_SMALL   12   14    4   status strip, settings MAC line
 *   body   UI_FONT_BODY    17   18    5   rows, previews, transcripts,
 *                                         settings, banner, status msgs
 *   emph   UI_FONT_EMPH    20   22    6   list titles
 *   hero   UI_FONT_HERO    25   27    8   outcome words ("Noted"), "REC"
 *
 * The banner is BODY, not emphasis: at the escalated sizes a 188 px
 * emphasis line carries ~13 characters and truncates almost every real
 * notification, while a body line (17 px caps -- the previous rounds'
 * emphasis height) still carries ~18. Text positions are BASELINES
 * (fb_text_prop). Widths are measured (fb_text_width_prop) and text is
 * fitted per pixel, never per character count.
 *
 * The vertical budget partitions exactly. The one-line grid reserves the
 * banner strip (a notification banner is only ever drawn over the
 * dashboard -- C7 round 5 scoped dismissal there); the two-line grid
 * owns the full height below the title, because Recordings never shows
 * a banner and a reserved-but-empty strip would cost a whole preview:
 *
 *   UI_STATUS_H + UI_TITLE_H + UI_LIST_ROWS  * UI_ROW_H  + UI_BANNER_H
 * =     20      +     30     +   5 * 24                  +     30       = 200
 *   UI_STATUS_H + UI_TITLE_H + UI_LIST2_ROWS * UI_ROW2_H
 * =     20      +     30     +   3 * 50                               = 200
 *
 * so the dashboard list (cursor inversion included) can never paint into
 * the banner strip and the banner never covers dashboard content. Every
 * baseline is chosen so the font's full ascent/descent band stays inside
 * its row or strip -- _Static_asserts in widgets.c hold the arithmetic.
 */
#define UI_MARGIN_X    2                        /* left/right text margin */
#define UI_TEXT_W      (UI_W - 2 * UI_MARGIN_X) /* 196 px usable line width */
#define UI_STATUS_H    20   /* status strip: y 0..19, divider at y 19 */
#define UI_STATUS_BASE 14   /* small baseline inside the strip */
#define UI_STATUS_GAP  6    /* px between status items */
#define UI_TITLE_H     30   /* list title row (emphasis) */
#define UI_TITLE_BASE  22   /* emphasis baseline rel. title-row top */
#define UI_ROW_H       24   /* one-line list row (body band 23 + 1) */
#define UI_ROW_BASE    18   /* body baseline rel. row top */
#define UI_LIST_ROWS   5    /* visible one-line rows below the title row */
#define UI_ROW2_H      50   /* two-line list row (Recordings previews) */
#define UI_ROW2_BASE1  20   /* first body baseline rel. row top */
#define UI_ROW2_BASE2  44   /* second body baseline (BASE1 + line pitch) */
#define UI_LIST2_ROWS  3    /* visible two-line rows below the title row */
#define UI_BANNER_H    30   /* inverted strip, y 170..199 (body line) */
#define UI_BANNER_BASE 22   /* body baseline rel. strip top */
#define UI_BANNER_PAD  6    /* banner side padding */
#define UI_TEXT_LINE_H 24   /* body text pitch (band 23 + 1 breathing) */
#define UI_TEXT_PAGE_LINES 7 /* transcript page: 7 pixel-wrapped body lines
                                (pages, like Recordings, own the full height:
                                no banner is ever drawn over an entry) */
#define UI_TEXT_FIRST_BASE (UI_STATUS_H + 2 + UI_FONT_BODY_ASC) /* page line 0 */

typedef struct {
    int battery_pct;          /* -1 hides */
    int wifi_ok;              /* 0/1 */
    int pending_uploads;
    char clock_hhmm[6];       /* "" hides */
} ui_status_t;
void widget_status_line(ui_fb_t *f, const ui_status_t *st);

/* Row text capacity covers the widest content a two-line row can ever
 * draw: at the body font's narrowest advance (5 px) two 196 px lines
 * hold at most 78 characters, so a 136-byte buffer means a byte-
 * truncated copy still overflows the pixel budget and the ellipsis can
 * never be lost to truncation. */
typedef struct { char text[136]; int done; int dim; } ui_row_t;
typedef struct {
    char title[48];
    ui_row_t rows[32]; int row_count;
    int cursor;               /* absolute index; widget scrolls the window */
    int two_line;             /* 1: UI_LIST2_ROWS rows of two body lines
                                 each, hard-split per pixel (Recordings
                                 transcript previews); 0: UI_LIST_ROWS
                                 one-line rows */
} ui_list_t;
void widget_list(ui_fb_t *f, const ui_list_t *l);

/* Renders one page of pixel-word-wrapped body text (UI_TEXT_W px per
 * line, UI_TEXT_PAGE_LINES lines per page). Returns total page count for
 * the given text. page is 0-based. */
int widget_text_page(ui_fb_t *f, const char *text, int page);

/* Page count only -- same wrap arithmetic as widget_text_page without a
 * framebuffer (cursor/page bookkeeping must not cost a 5 KB scratch). */
int widget_text_pages(const char *text);

void widget_banner(ui_fb_t *f, const char *text);   /* inverted strip at bottom */

/* Deliberate empty state (C7 round 6): two short body lines, each
 * horizontally centered by measured width, the pair vertically centered
 * in [y0, y1). Drawn under a retained title so an empty list reads as
 * intentional ("Nothing yet" / "Hold REC to talk"), never as a bare or
 * broken screen. */
void widget_empty_state(ui_fb_t *f, const char *line1, const char *line2,
                        int y0, int y1);

#endif
