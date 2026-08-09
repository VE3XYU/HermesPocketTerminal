#include "harness.h"
#include "gesture.h"

/* helper: run the fsm through (rec,pwr,t) samples, return the first non-NONE gesture */
static gesture_t run(gesture_fsm_t *g, const int (*seq)[3], int n) {
    gesture_t got = GEST_NONE;
    for (int i = 0; i < n; i++) {
        gesture_t r = gesture_feed(g, seq[i][0], seq[i][1], (unsigned)seq[i][2]);
        if (r != GEST_NONE && got == GEST_NONE) got = r;
    }
    return got;
}

/* Feeds a whole press as the UI loop would: PWR closed over [t_down,
 * t_up), sampled every `poll` ms from 0 to t_end, with an optional single
 * OPEN glitch of `glitch_ms` starting at `glitch_at` (glitch_at < 0 = no
 * glitch). Returns the number of gestures emitted and stores them. */
static int feed_pwr(gesture_t *got, int cap, int t_down, int t_up,
                    int glitch_at, int glitch_ms, int t_end, int poll) {
    gesture_fsm_t g;
    gesture_init(&g);
    int n = 0;
    for (int t = 0; t <= t_end; t += poll) {
        int down = (t >= t_down && t < t_up);
        if (glitch_at >= 0 && t >= glitch_at && t < glitch_at + glitch_ms) down = 0;
        gesture_t r = gesture_feed(&g, 0, down, (unsigned)t);
        if (r != GEST_NONE && n < cap) got[n++] = r;
    }
    return n;
}

int main(void) {
    gesture_fsm_t g;
    gesture_t got[8];

    /* REC hold fires at 350 ms while still held */
    gesture_init(&g);
    const int hold[][3] = { {1,0,0}, {1,0,100}, {1,0,349}, {1,0,351} };
    CHECK_EQ_INT(run(&g, hold, 4), GEST_REC_HOLD_START);
    /* ...and does not fire again, nor emit SHORT on its debounced release */
    CHECK_EQ_INT(gesture_feed(&g, 1, 0, 400), GEST_NONE);
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 500), GEST_NONE);
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 560), GEST_NONE);

    /* REC quick tap = SHORT, on the DEBOUNCED release */
    gesture_init(&g);
    CHECK_EQ_INT(gesture_feed(&g, 1, 0, 0), GEST_NONE);
    CHECK_EQ_INT(gesture_feed(&g, 1, 0, 100), GEST_NONE);
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 200), GEST_NONE);   /* release seen... */
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 220), GEST_NONE);   /* ...20 ms: not yet */
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 240), GEST_REC_SHORT);   /* 40 ms >= 30 */

    /* PWR tap resolves on the debounced release -- no double-tap
     * vocabulary means no post-release waiting window (C7 round 5: the
     * retired 250 ms window delayed every tap's feedback by that much;
     * round 7's debounce costs one extra ~20 ms poll, not 250). */
    gesture_init(&g);
    CHECK_EQ_INT(gesture_feed(&g, 0, 1, 0), GEST_NONE);
    CHECK_EQ_INT(gesture_feed(&g, 0, 1, 100), GEST_NONE);
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 200), GEST_NONE);
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 240), GEST_PWR_SHORT);   /* the release edge */

    /* two quick taps = two SHORTs, not a double (double-tap retired) */
    gesture_init(&g);
    CHECK_EQ_INT(gesture_feed(&g, 0, 1, 0), GEST_NONE);
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 120), GEST_NONE);
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 160), GEST_PWR_SHORT);
    CHECK_EQ_INT(gesture_feed(&g, 0, 1, 200), GEST_NONE);        /* within the old 250 ms window */
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 320), GEST_NONE);
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 360), GEST_PWR_SHORT);

    /* PWR long: release after 600 */
    gesture_init(&g);
    const int plong[][3] = { {0,1,0}, {0,1,650}, {0,0,700}, {0,0,740} };
    CHECK_EQ_INT(run(&g, plong, 4), GEST_PWR_LONG);
    /* boundary: release at exactly 599 sampled ms is still a tap -- the
     * debounce window must NOT be counted into the press duration */
    gesture_init(&g);
    CHECK_EQ_INT(gesture_feed(&g, 0, 1, 0), GEST_NONE);
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 599), GEST_NONE);
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 660), GEST_PWR_SHORT);   /* 660-0 > 600, but 599 decides */

    /* PWR off: still held at 5000, fires while held */
    gesture_init(&g);
    const int poff[][3] = { {0,1,0}, {0,1,2000}, {0,1,4999}, {0,1,5001} };
    CHECK_EQ_INT(run(&g, poff, 4), GEST_PWR_OFF);
    /* release afterwards emits nothing (the hold was consumed) */
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 5200), GEST_NONE);
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 5300), GEST_NONE);

    /* ---- C7 round 7, finding 1: one lazy press must be ONE gesture ----
     * The operator's "hold just a little longer than a tap and I hear the
     * double sound". A soft press chatters at the contact; sampled at the
     * UI loop's 20 ms poll it presents open/closed/open and used to split
     * into two classified gestures (two clicks). */

    /* clean 120 ms tap: exactly one SHORT */
    CHECK_EQ_INT(feed_pwr(got, 8, 0, 120, -1, 0, 1000, 20), 1);
    CHECK_EQ_INT(got[0], GEST_PWR_SHORT);

    /* clean 700 ms press: exactly one LONG */
    CHECK_EQ_INT(feed_pwr(got, 8, 0, 700, -1, 0, 1500, 20), 1);
    CHECK_EQ_INT(got[0], GEST_PWR_LONG);

    /* 760 ms press with a 10 ms open blip at 680 that the poll DOES catch,
     * followed by the contact re-closing (release bounce): still exactly
     * one LONG */
    CHECK_EQ_INT(feed_pwr(got, 8, 0, 760, 680, 10, 1500, 20), 1);
    CHECK_EQ_INT(got[0], GEST_PWR_LONG);

    /* 700 ms press with a 10 ms open blip 20 ms in (make bounce -- the
     * diagnosed mechanism: the blip used to emit a SHORT, and the still-
     * held button then re-latched and emitted a second gesture on the real
     * release). One press, one gesture, and it is the LONG the operator
     * asked for -- the latch kept the ORIGINAL press edge. */
    CHECK_EQ_INT(feed_pwr(got, 8, 0, 700, 20, 10, 1500, 20), 1);
    CHECK_EQ_INT(got[0], GEST_PWR_LONG);

    /* same blip on a 120 ms tap: still one SHORT */
    CHECK_EQ_INT(feed_pwr(got, 8, 0, 120, 20, 10, 1000, 20), 1);
    CHECK_EQ_INT(got[0], GEST_PWR_SHORT);

    /* a 25 ms open glitch (under the debounce) mid-press is absorbed too */
    CHECK_EQ_INT(feed_pwr(got, 8, 0, 700, 300, 25, 1500, 20), 1);
    CHECK_EQ_INT(got[0], GEST_PWR_LONG);

    /* ...but two genuinely separate presses are still two gestures: a
     * 100 ms gap is far past the debounce window */
    {
        gesture_fsm_t s;
        gesture_init(&s);
        int n = 0;
        for (int t = 0; t <= 800; t += 20) {
            int down = (t < 120) || (t >= 220 && t < 340);
            gesture_t r = gesture_feed(&s, 0, down, (unsigned)t);
            if (r != GEST_NONE && n < 8) got[n++] = r;
        }
        CHECK_EQ_INT(n, 2);
        CHECK_EQ_INT(got[0], GEST_PWR_SHORT);
        CHECK_EQ_INT(got[1], GEST_PWR_SHORT);
    }

    /* the same discipline on REC: a lazy hold with a make-bounce still
     * starts hold-to-talk from the original press edge (at 350 ms, not
     * 350 ms after the bounce) and emits no stray SHORT */
    {
        gesture_fsm_t s;
        gesture_init(&s);
        gesture_t first = GEST_NONE;
        int first_t = -1, n = 0;
        for (int t = 0; t <= 800; t += 20) {
            int down = (t < 600) && !(t >= 20 && t < 30);
            gesture_t r = gesture_feed(&s, down, 0, (unsigned)t);
            if (r != GEST_NONE) { n++; if (first_t < 0) { first = r; first_t = t; } }
        }
        CHECK_EQ_INT(n, 1);
        CHECK_EQ_INT(first, GEST_REC_HOLD_START);
        CHECK(first_t < 380);   /* measured from the original press edge */
    }

    /* both buttons acting on the same sample: one gesture leaves per call
     * and the PWR release is NOT dropped -- it emerges on the next sample */
    {
        gesture_fsm_t s;
        gesture_init(&s);
        gesture_feed(&s, 1, 1, 0);                       /* both down */
        CHECK_EQ_INT(gesture_feed(&s, 0, 0, 100), GEST_NONE);   /* both open */
        gesture_t a = gesture_feed(&s, 0, 0, 140);
        gesture_t b = gesture_feed(&s, 0, 0, 160);
        CHECK_EQ_INT(a, GEST_REC_SHORT);
        CHECK_EQ_INT(b, GEST_PWR_SHORT);
    }
    return HARNESS_REPORT();
}
