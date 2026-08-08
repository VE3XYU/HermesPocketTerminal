#include "ui_fb.h"
#include <string.h>
#include "lib_sans_body.h"
#include "lib_sans_emph.h"
#include "lib_sans_hero.h"

/* Role id -> generated table (fonts stay private to this file). */
static const ui_font_t *font_for(ui_font_id_t id) {
    switch (id) {
        case UI_FONT_EMPH: return &lib_sans_emph;
        case UI_FONT_HERO: return &lib_sans_hero;
        default:           return &lib_sans_body;
    }
}

void fb_clear(ui_fb_t *f) { memset(f->px, 0xff, sizeof f->px); }

void fb_pixel(ui_fb_t *f, int x, int y, int black) {
    if (x < 0 || x >= UI_W || y < 0 || y >= UI_H) return;
    uint8_t *b = &f->px[y * UI_STRIDE + x / 8];
    uint8_t mask = (uint8_t)(0x80 >> (x & 7));
    if (black) *b &= (uint8_t)~mask; else *b |= mask;
}

int fb_get(const ui_fb_t *f, int x, int y) {
    if (x < 0 || x >= UI_W || y < 0 || y >= UI_H) return 0;
    return !(f->px[y * UI_STRIDE + x / 8] & (0x80 >> (x & 7)));
}

void fb_hline(ui_fb_t *f, int x, int y, int w, int black) {
    for (int i = 0; i < w; i++) fb_pixel(f, x + i, y, black);
}

void fb_fill(ui_fb_t *f, int x, int y, int w, int h, int black) {
    for (int j = 0; j < h; j++) fb_hline(f, x, y + j, w, black);
}

void fb_rect(ui_fb_t *f, int x, int y, int w, int h, int black) {
    fb_hline(f, x, y, w, black); fb_hline(f, x, y + h - 1, w, black);
    for (int j = 1; j < h - 1; j++) { fb_pixel(f, x, y + j, black); fb_pixel(f, x + w - 1, y + j, black); }
}

void fb_invert(ui_fb_t *f, int x, int y, int w, int h) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int xx = x + i, yy = y + j;
            if (xx < 0 || xx >= UI_W || yy < 0 || yy >= UI_H) continue;
            f->px[yy * UI_STRIDE + xx / 8] ^= (uint8_t)(0x80 >> (xx & 7));
        }
}

/* ---- proportional renderer (GFX glyph format, ui_font.h) ---- */

static const ui_glyph_t *glyph_for(const ui_font_t *fo, unsigned char c) {
    if (c < fo->first || c > fo->last) c = '?';
    return &fo->glyph[c - fo->first];
}

void fb_text_prop(ui_fb_t *f, int x, int baseline, const char *s,
                  ui_font_id_t font, int black) {
    const ui_font_t *fo = font_for(font);
    for (; *s; s++) {
        const ui_glyph_t *g = glyph_for(fo, (unsigned char)*s);
        const uint8_t *bits = &fo->bitmap[g->bitmap_offset];
        int bit = 0;
        for (int gy = 0; gy < g->height; gy++)
            for (int gx = 0; gx < g->width; gx++, bit++)
                if (bits[bit >> 3] & (0x80 >> (bit & 7)))
                    fb_pixel(f, x + g->x_offset + gx,
                             baseline + g->y_offset + gy, black);
        x += g->x_advance;
    }
}

int fb_text_width_prop(const char *s, ui_font_id_t font) {
    const ui_font_t *fo = font_for(font);
    int pen = 0, right = 0;
    for (; *s; s++) {
        const ui_glyph_t *g = glyph_for(fo, (unsigned char)*s);
        int ink = pen + g->x_offset + g->width;
        if (ink > right) right = ink;
        pen += g->x_advance;
    }
    return pen > right ? pen : right;
}

int fb_text_fit_prop(const char *s, ui_font_id_t font, int max_w) {
    const ui_font_t *fo = font_for(font);
    int pen = 0, width = 0, fit = 0;
    for (int i = 0; s[i]; i++) {
        const ui_glyph_t *g = glyph_for(fo, (unsigned char)s[i]);
        int ink = pen + g->x_offset + g->width;
        pen += g->x_advance;
        if (ink > width) width = ink;
        if (pen > width) width = pen;    /* width is monotone in i */
        if (width > max_w) break;
        fit = i + 1;
    }
    return fit;
}

void fb_ellipsize_prop(char *dst, size_t cap, const char *src,
                       ui_font_id_t font, int max_w) {
    if (cap == 0) return;
    size_t len = strlen(src);
    if (len > cap - 1) len = cap - 1;    /* work inside dst's budget */

    memcpy(dst, src, len);
    dst[len] = '\0';
    if (fb_text_width_prop(dst, font) <= max_w) return;

    if (cap < 4) { dst[0] = '\0'; return; }  /* no room for "..." at all */
    int dots_w = fb_text_width_prop("...", font);
    int keep = fb_text_fit_prop(dst, font, max_w - dots_w);
    if ((size_t)keep > cap - 4) keep = (int)(cap - 4);  /* room for "..." */
    memcpy(dst + keep, "...", 4);
}

int fb_count_black(const ui_fb_t *f) {
    int n = 0;
    for (int y = 0; y < UI_H; y++)
        for (int x = 0; x < UI_W; x++) n += fb_get(f, x, y);
    return n;
}
