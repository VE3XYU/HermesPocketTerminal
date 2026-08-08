#include "harness.h"
#include "ui_fb.h"
#include "ui_font_metrics.h"
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

    /* ---- proportional renderer (Liberation Sans ramp) ---- */

    /* the ramp is proportional: 'W' is wider than 'i', and the three
     * sizes order body < emphasis < hero for the same string */
    CHECK(fb_text_width_prop("W", UI_FONT_BODY) > fb_text_width_prop("i", UI_FONT_BODY));
    int wb = fb_text_width_prop("Noted", UI_FONT_BODY);
    int we = fb_text_width_prop("Noted", UI_FONT_EMPH);
    int wh = fb_text_width_prop("Noted", UI_FONT_HERO);
    CHECK(wb > 0);
    CHECK(wb < we);
    CHECK(we < wh);
    /* width is monotone in the string: a prefix is never wider */
    CHECK(fb_text_width_prop("Note", UI_FONT_BODY) <= wb);

    /* cap heights track the reference ramp (see ui_font_metrics.h):
     * ink of "H" spans exactly [baseline - cap, baseline) */
    {
        static const struct { ui_font_id_t id; int cap, asc, desc; } ramp[] = {
            { UI_FONT_BODY, UI_FONT_BODY_CAP, UI_FONT_BODY_ASC, UI_FONT_BODY_DESC },
            { UI_FONT_EMPH, UI_FONT_EMPH_CAP, UI_FONT_EMPH_ASC, UI_FONT_EMPH_DESC },
            { UI_FONT_HERO, UI_FONT_HERO_CAP, UI_FONT_HERO_ASC, UI_FONT_HERO_DESC },
        };
        for (int i = 0; i < 3; i++) {
            int base = 100;
            fb_clear(&fb);
            fb_text_prop(&fb, 10, base, "H", ramp[i].id, 1);
            int top = -1, bot = -1;
            for (int y = 0; y < UI_H; y++)
                for (int x = 0; x < UI_W; x++)
                    if (fb_get(&fb, x, y)) { if (top < 0) top = y; bot = y; }
            CHECK_EQ_INT(top, base - ramp[i].cap);
            CHECK_EQ_INT(bot, base - 1);
            /* descenders stay inside the declared band */
            fb_clear(&fb);
            fb_text_prop(&fb, 10, base, "gjpqy(", ramp[i].id, 1);
            int lo = -1, hi = -1;
            for (int y = 0; y < UI_H; y++)
                for (int x = 0; x < UI_W; x++)
                    if (fb_get(&fb, x, y)) { if (lo < 0) lo = y; hi = y; }
            CHECK(lo >= base - ramp[i].asc);
            CHECK(hi <= base + ramp[i].desc - 1);
        }
    }

    /* ink never lands right of the measured width (string chosen to fit
     * the 200 px panel so nothing clips) */
    {
        const char *s = "Remind me to buy";
        int w = fb_text_width_prop(s, UI_FONT_BODY);
        CHECK(w < UI_W);
        fb_clear(&fb);
        fb_text_prop(&fb, 0, 100, s, UI_FONT_BODY, 1);
        int right = -1;
        for (int y = 0; y < UI_H; y++)
            for (int x = 0; x < UI_W; x++)
                if (fb_get(&fb, x, y) && x > right) right = x;
        CHECK(right < w);
        CHECK(right >= w - 4);   /* and the measure is tight, not padded */
    }

    /* out-of-range bytes render the '?' glyph, not garbage */
    fb_clear(&fb);
    fb_text_prop(&fb, 0, 40, "\x01\xc3", UI_FONT_BODY, 1);
    int ink_bad = fb_count_black(&fb);
    fb_clear(&fb);
    fb_text_prop(&fb, 0, 40, "??", UI_FONT_BODY, 1);
    CHECK_EQ_INT(fb_count_black(&fb), ink_bad);

    /* fit: longest prefix whose measured width fits the budget */
    {
        const char *s = "Call dentist about Tuesday";
        int w_all = fb_text_width_prop(s, UI_FONT_BODY);
        CHECK_EQ_INT(fb_text_fit_prop(s, UI_FONT_BODY, w_all), (int)strlen(s));
        int n = fb_text_fit_prop(s, UI_FONT_BODY, w_all - 1);
        CHECK(n < (int)strlen(s));
        char pre[64];
        memcpy(pre, s, (size_t)n); pre[n] = '\0';
        CHECK(fb_text_width_prop(pre, UI_FONT_BODY) <= w_all - 1);
        pre[n] = s[n]; pre[n + 1] = '\0';
        CHECK(fb_text_width_prop(pre, UI_FONT_BODY) > w_all - 1);
    }

    /* ellipsize: fits-whole passes through; too-long ends in a measured
     * "..." and the result really fits the budget */
    {
        char out[64];
        fb_ellipsize_prop(out, sizeof out, "short", UI_FONT_BODY, 196);
        CHECK_EQ_STR(out, "short");
        const char *lng = "a rather long transcript opening that cannot fit";
        fb_ellipsize_prop(out, sizeof out, lng, UI_FONT_BODY, 120);
        size_t ol = strlen(out);
        CHECK(ol > 3);
        CHECK_EQ_STR(out + ol - 3, "...");
        CHECK(fb_text_width_prop(out, UI_FONT_BODY) <= 120);
        /* threshold is exact: at the string's own width, no ellipsis */
        int w_lng = fb_text_width_prop(lng, UI_FONT_BODY);
        fb_ellipsize_prop(out, sizeof out, lng, UI_FONT_BODY, w_lng);
        CHECK_EQ_STR(out, lng);
        fb_ellipsize_prop(out, sizeof out, lng, UI_FONT_BODY, w_lng - 1);
        CHECK(strcmp(out, lng) != 0);
    }

    /* clipping off every edge stays silent */
    fb_clear(&fb);
    fb_text_prop(&fb, -30, 5, "clip", UI_FONT_HERO, 1);
    fb_text_prop(&fb, 190, 199, "clip", UI_FONT_HERO, 1);
    CHECK(fb_count_black(&fb) >= 0);
    return HARNESS_REPORT();
}
