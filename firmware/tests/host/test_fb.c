#include "harness.h"
#include "ui_fb.h"
#include <string.h>

int main(void) {
    ui_fb_t fb;
    fb_clear(&fb);
    CHECK_EQ_INT(fb_count_black(&fb), 0);
    CHECK_EQ_INT((int)sizeof fb.px, 5000);

    fb_pixel(&fb, 0, 0, 1);
    fb_pixel(&fb, 199, 199, 1);
    CHECK_EQ_INT(fb_get(&fb, 0, 0), 1);
    CHECK_EQ_INT(fb_get(&fb, 199, 199), 1);
    CHECK_EQ_INT(fb_count_black(&fb), 2);
    CHECK_EQ_INT((fb.px[0] & 0x80) == 0, 1);       /* bit cleared = black, MSB first */

    fb_pixel(&fb, -1, 0, 1); fb_pixel(&fb, 0, 200, 1); fb_pixel(&fb, 200, 0, 1);
    CHECK_EQ_INT(fb_count_black(&fb), 2);           /* clipping is silent */
    CHECK_EQ_INT(fb_get(&fb, -5, 300), 0);

    fb_pixel(&fb, 0, 0, 0); fb_pixel(&fb, 199, 199, 0);
    fb_fill(&fb, 10, 10, 20, 5, 1);
    CHECK_EQ_INT(fb_count_black(&fb), 100);
    fb_invert(&fb, 10, 10, 20, 5);
    CHECK_EQ_INT(fb_count_black(&fb), 0);
    fb_invert(&fb, 0, 0, 8, 1);
    CHECK_EQ_INT(fb_count_black(&fb), 8);
    fb_clear(&fb);

    fb_rect(&fb, 50, 50, 10, 10, 1);                /* outline = perimeter */
    CHECK_EQ_INT(fb_count_black(&fb), 36);
    fb_clear(&fb);

    /* Text: renders ink, width is deterministic, 2x scale = 4x the ink */
    fb_text(&fb, 0, 0, "HTP", 1, 1);
    int ink1 = fb_count_black(&fb);
    CHECK(ink1 > 20);
    CHECK_EQ_INT(fb_text_width("HTP", 1), 24);
    CHECK_EQ_INT(fb_text_width("HTP", 2), 48);
    fb_clear(&fb);
    fb_text(&fb, 0, 0, "HTP", 2, 1);
    CHECK_EQ_INT(fb_count_black(&fb), ink1 * 4);
    fb_clear(&fb);

    /* Inverse text (black=0 on filled bg) and edge clipping don't crash */
    fb_fill(&fb, 0, 0, 200, 16, 1);
    fb_text(&fb, 2, 4, "banner", 1, 0);
    CHECK(fb_count_black(&fb) < 200 * 16 / 8 * 8);
    fb_text(&fb, 190, 190, "clipped text far off screen", 2, 1);
    return HARNESS_REPORT();
}
