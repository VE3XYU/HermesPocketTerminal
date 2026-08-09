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
 * ---- row breathing room (C7 round 7, finding 3) ----
 *
 * The bench photo showed cap-height letters all but touching the
 * inverted cursor bar: at UI_ROW_H 24 / UI_ROW_BASE 18 an ASCENDER
 * (18 px) started at row-relative y 0 -- literally on the bar's top
 * edge -- and text began 2 px from the panel edge. Both grids now hold
 * the same rule, asserted below:
 *
 *   >= 3 px clear above the cap top and >= 3 px clear below the
 *   baseline inside the row band, with the full ascent/descent band
 *   centred in the band, and >= UI_ROW_PAD_X px of left inset applied
 *   to normal and inverted rows alike so the column never moves.
 *
 * Finding the pixels: with the banner strip reserved as a SIXTH band the
 * one-line grid could only afford 24 px rows ((200 - 20 - 30 - 30) / 5),
 * and no re-cut of status/title/banner reaches a comfortable row without
 * making the (equally inverted) banner as cramped as the rows were. So
 * the banner strip stopped being a separate reservation and became
 * exactly the LAST ROW BAND: the list renders UI_LIST_ROWS rows when
 * there is no banner and UI_LIST_ROWS - 1 when there is one
 * (ui_list_t.reserve_banner), and the banner occupies the band the
 * dropped row vacated. Nothing is ever painted over -- a banner costs
 * one visible row while it is up, and a REC tap dismisses it. That buys
 * 30 px rows without touching the status strip, the title band or the
 * banner's own metrics:
 *
 *   UI_STATUS_H + UI_TITLE_H + UI_LIST_ROWS  * UI_ROW_H
 * =     20      +     30     +   5 * 30                  = 200
 *   UI_STATUS_H + UI_TITLE_H + (UI_LIST_ROWS - 1) * UI_ROW_H + UI_BANNER_H
 * =     20      +     30     +   4 * 30                      +    30    = 200
 *   UI_STATUS_H + UI_TITLE_H + UI_LIST2_ROWS * UI_ROW2_H
 * =     20      +     30     +   3 * 50                               = 200
 *
 * The two-line grid already met the rule (BASE1 20 = 3 px above cap) and
 * keeps its geometry; it owns the full height below the title, because
 * Recordings never shows a banner. The menu is four rows in a five-row
 * grid, so it simply leaves the last band blank -- the "menu rows taller
 * because there are only four" outcome falls out of the same numbers.
 * Every baseline keeps the font's full ascent/descent band inside its
 * row or strip -- _Static_asserts in widgets.c hold the arithmetic.
 */
#define UI_MARGIN_X    2                        /* left/right text margin */
#define UI_TEXT_W      (UI_W - 2 * UI_MARGIN_X) /* 196 px usable line width */
#define UI_STATUS_H    20   /* status strip: y 0..19, divider at y 19 */
#define UI_STATUS_BASE 14   /* small baseline inside the strip */
#define UI_STATUS_GAP  6    /* px between status items */
#define UI_TITLE_H     30   /* list title row (emphasis) */
#define UI_TITLE_BASE  22   /* emphasis baseline rel. title-row top */
#define UI_ROW_PAD_X   6    /* list left/right text inset -- normal AND
                               inverted rows, so the column never shifts;
                               matches UI_BANNER_PAD so a dashboard row and
                               the banner under it share one text column */
#define UI_ROW_TEXT_W  (UI_W - 2 * UI_ROW_PAD_X)  /* 188 px list line width */
#define UI_ROW_H       30   /* one-line list row (body band 23, 4 above / 3 below) */
#define UI_ROW_BASE    22   /* body baseline rel. row top */
#define UI_LIST_ROWS   5    /* one-line rows below the title; the last band is
                               the banner strip when a banner is up */
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
 * draw: at the body font's narrowest advance (5 px) two UI_ROW_TEXT_W
 * (188 px) lines hold at most 76 characters, so a 136-byte buffer
 * means a byte-
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
    int reserve_banner;       /* one-line grid only: a banner is up, so the
                                 last row band belongs to it -- draw
                                 UI_LIST_ROWS - 1 rows (C7 round 7) */
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
