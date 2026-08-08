#ifndef UI_FB_H
#define UI_FB_H

#include <stdint.h>
#include <stddef.h>
#include "ui_font.h"

/* 200x200, 1 bit per pixel, bit set = WHITE (SSD1681 RAM convention),
 * row-major, 25 bytes per row, MSB = leftmost pixel.
 */
#define UI_W 200
#define UI_H 200
#define UI_STRIDE (UI_W / 8)

typedef struct {
    uint8_t px[UI_STRIDE * UI_H];
} ui_fb_t;

/* all white */
void fb_clear(ui_fb_t *f);

/* clips silently */
void fb_pixel(ui_fb_t *f, int x, int y, int black);

/* 1 = black; 0 outside */
int fb_get(const ui_fb_t *f, int x, int y);

void fb_hline(ui_fb_t *f, int x, int y, int w, int black);

/* outline */
void fb_rect(ui_fb_t *f, int x, int y, int w, int h, int black);

void fb_fill(ui_fb_t *f, int x, int y, int w, int h, int black);

void fb_invert(ui_fb_t *f, int x, int y, int w, int h);

void fb_text(ui_fb_t *f, int x, int y, const char *s, int scale, int black);

/* 8 * scale * chars */
int fb_text_width(const char *s, int scale);

/* Body text: Spleen 8x16 (vendored, BSD-2-Clause -- fonts/spleen_8x16.h),
 * a narrow 8 px advance at the same 16 px height the old scale-2 square
 * glyphs had, so a body line carries 24 chars instead of 12. Always scale
 * 1; the extra-large headline sizes stay on fb_text's scaled 8x8. */
void fb_text16(ui_fb_t *f, int x, int y, const char *s, int black);

/* 8 * chars */
int fb_text16_width(const char *s);

/* ---- proportional text (Liberation Sans ramp, see ui_font.h) ----
 *
 * All coordinates are the BASELINE (pen) position, GFX-style: glyph ink
 * spans [baseline - ascent, baseline + descent). Bytes outside ASCII
 * 0x20..0x7E render the '?' glyph (transcripts can carry stray UTF-8).
 * Clipping is fb_pixel's silent bounds check. No heap anywhere. */

void fb_text_prop(ui_fb_t *f, int x, int baseline, const char *s,
                  ui_font_id_t font, int black);

/* Pixel width: max of total advance and the right-most ink edge, so a
 * string that "fits" by this measure never paints past the edge. */
int fb_text_width_prop(const char *s, ui_font_id_t font);

/* Longest prefix (in bytes) whose measured width fits max_w. */
int fb_text_fit_prop(const char *s, ui_font_id_t font, int max_w);

/* Copies src to dst whole when it fits max_w px; otherwise the longest
 * prefix such that prefix + "..." fits, with the "..." appended -- the
 * ellipsis is measured, never assumed. dst never exceeds cap bytes. */
void fb_ellipsize_prop(char *dst, size_t cap, const char *src,
                       ui_font_id_t font, int max_w);

int fb_count_black(const ui_fb_t *f);

#endif
