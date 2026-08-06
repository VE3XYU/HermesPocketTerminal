#include "ui_fb.h"
#include <string.h>
#include "font8x8_basic.h"

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

void fb_text(ui_fb_t *f, int x, int y, const char *s, int scale, int black) {
    for (; *s; s++) {
        unsigned ch = (unsigned char)*s;
        if (ch > 127) ch = '?';
        const unsigned char *glyph = (const unsigned char *)font8x8_basic[ch];
        for (int gy = 0; gy < 8; gy++)
            for (int gx = 0; gx < 8; gx++)
                if (glyph[gy] & (1 << gx))          /* font8x8: LSB = leftmost */
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++)
                            fb_pixel(f, x + gx * scale + sx, y + gy * scale + sy, black);
        x += 8 * scale;
    }
}

int fb_text_width(const char *s, int scale) { return (int)strlen(s) * 8 * scale; }

int fb_count_black(const ui_fb_t *f) {
    int n = 0;
    for (int y = 0; y < UI_H; y++)
        for (int x = 0; x < UI_W; x++) n += fb_get(f, x, y);
    return n;
}
