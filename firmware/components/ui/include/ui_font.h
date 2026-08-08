#ifndef UI_FONT_H
#define UI_FONT_H

#include <stdint.h>

/* Adafruit-GFX-format glyph tables. The LAYOUT is an open de-facto
 * standard (bitmapOffset / width / height / xAdvance / xOffset / yOffset
 * over a bit-packed bitmap pool); the implementation here and the tables
 * in fonts/ are generated fresh from Liberation Sans (SIL OFL 1.1) by
 * fonts/gen_gfx_font.py -- nothing is copied from Adafruit or from the
 * reference firmware.
 *
 * The type ramp mirrors the shipped product on this panel: a ~9 pt
 * proportional body, ~12 pt bold emphasis, ~18.5 pt bold hero, cap
 * heights within 1 px of that product's FreeSans ramp. Callers address
 * fonts by role id; the tables themselves are private to fb.c (the
 * Task 8 constraint: widgets never include font headers).
 */

typedef struct {
    uint16_t bitmap_offset;      /* first byte of this glyph in the pool */
    uint8_t  width, height;      /* glyph bitmap size, px */
    uint8_t  x_advance;          /* pen advance, px */
    int8_t   x_offset, y_offset; /* bitmap origin rel. to pen x / baseline */
} ui_glyph_t;

typedef struct {
    const uint8_t    *bitmap;    /* bit-packed pool, MSB first, per-glyph
                                    byte-aligned */
    const ui_glyph_t *glyph;     /* ASCII 0x20..0x7E: 95 entries */
    uint8_t first, last;
    uint8_t y_advance;           /* font-natural line pitch (layout uses
                                    its own tighter pitch, like the
                                    reference does) */
    uint8_t cap;                 /* 'H' height above baseline, px */
    uint8_t ascent, descent;     /* max ink above/below baseline, ASCII */
} ui_font_t;

typedef enum {
    UI_FONT_BODY,   /* Liberation Sans Regular 9 pt: cap 12, asc 14, desc 4 */
    UI_FONT_EMPH,   /* Liberation Sans Bold 12 pt: cap 17, asc 18, desc 5 */
    UI_FONT_HERO,   /* Liberation Sans Bold 18.5 pt: cap 25, asc 27, desc 8 */
} ui_font_id_t;

#endif
