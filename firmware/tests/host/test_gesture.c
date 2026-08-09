#include "harness.h"
#include "gesture.h"

/* Modeling gap (documented, Task 19): every case below feeds the FSM at an
 * uninterrupted 20 ms cadence. The real UI loop is NOT uninterrupted -- an
 * e-paper partial refresh blocks it for 300-500 ms, during which buttons go
 * unsampled and `now` then jumps. Consequences the suite does not model: a
 * press-and-release completed entirely inside a refresh is never seen at
 * all, and a press whose release lands inside one has its open edge dated
 * at the first post-refresh sample, so its measured duration stretches by
 * up to the refresh time (a tap released mid-refresh can classify LONG).
 * Both are inherent to sampled input on a blocking display, were accepted
 * in C7 round 7, and cannot be regression-tested here without also
 * modeling the display's timing. */

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
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 240), GEST_NONE);   /* ...40 ms: not yet */
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 260), GEST_REC_SHORT);   /* 60 ms >= 60 */

    /* PWR tap resolves on the debounced release -- no double-tap
     * vocabulary means no post-release waiting window (C7 round 5: the
     * retired 250 ms window delayed every tap's feedback by that much;
     * the release debounce costs ~60 ms after the open edge, not 250). */
    gesture_init(&g);
    CHECK_EQ_INT(gesture_feed(&g, 0, 1, 0), GEST_NONE);
    CHECK_EQ_INT(gesture_feed(&g, 0, 1, 100), GEST_NONE);
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 200), GEST_NONE);
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 260), GEST_PWR_SHORT);   /* the release edge */

    /* two quick taps = two SHORTs, not a double (double-tap retired) */
    gesture_init(&g);
    CHECK_EQ_INT(gesture_feed(&g, 0, 1, 0), GEST_NONE);
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 120), GEST_NONE);
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 180), GEST_PWR_SHORT);
    CHECK_EQ_INT(gesture_feed(&g, 0, 1, 220), GEST_NONE);        /* within the old 250 ms window */
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 320), GEST_NONE);
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 380), GEST_PWR_SHORT);

    /* PWR long: release after 600 */
    gesture_init(&g);
    const int plong[][3] = { {0,1,0}, {0,1,650}, {0,0,700}, {0,0,760} };
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

    /* ---- Task 19, the parked C7 double-beep: RELEASE chatter ----
     * Repro on hardware: hold noticeably longer than a tap, release ->
     * two blips. Mechanism (round-7 re-review prediction, confirmed here):
     * a slow finger roll-off makes the contact re-strike at intervals of
     * 50 ms and more; once the button has read open long enough to
     * classify, the next sampled re-strike latched a NEW press, and its
     * release classified a second gesture -> second click. The old 30 ms
     * window classified after 2 open polls (~40 ms), so any re-strike gap
     * >= ~50 ms split. At 60 ms the FSM waits 3 open polls and absorbs
     * re-strike gaps up to ~70 ms sampled at the 20 ms cadence. */

    /* long hold, then release chatter with re-strikes at 760 and 820
     * (gaps of 60/40 ms): exactly ONE gesture, and still the LONG the
     * press duration earned */
    {
        gesture_fsm_t s;
        gesture_init(&s);
        int n = 0;
        for (int t = 0; t <= 1500; t += 20) {
            int down = (t < 700) || (t == 760) || (t == 820);
            gesture_t r = gesture_feed(&s, 0, down, (unsigned)t);
            if (r != GEST_NONE && n < 8) got[n++] = r;
        }
        CHECK_EQ_INT(n, 1);
        CHECK_EQ_INT(got[0], GEST_PWR_LONG);
    }

    /* the same chatter on a REC tap: one SHORT, not tap + phantom select */
    {
        gesture_fsm_t s;
        gesture_init(&s);
        int n = 0;
        for (int t = 0; t <= 1000; t += 20) {
            int down = (t < 200) || (t == 260);
            gesture_t r = gesture_feed(&s, down, 0, (unsigned)t);
            if (r != GEST_NONE && n < 8) got[n++] = r;
        }
        CHECK_EQ_INT(n, 1);
        CHECK_EQ_INT(got[0], GEST_REC_SHORT);
    }

    /* ---- Fix round (review finding, Minor 6): the documented flip side of
     * the 60 ms debounce -- two DELIBERATE taps separated by only ~80 ms
     * must still classify as two gestures, not merge into one. This is the
     * boundary right past the ~70 ms the header comment documents as
     * absorbed; the pre-existing "two genuinely separate presses" case
     * above uses a comfortable 100 ms gap and does not pressure the edge.
     * Before the debounce widened to 60 ms this margin was much larger, so
     * this case would not have caught a regression back then -- it exists
     * to catch the debounce being widened further without re-checking this
     * cost. */
    {
        gesture_fsm_t s;
        gesture_init(&s);
        int n = 0;
        for (int t = 0; t <= 400; t += 20) {
            /* tap 1: closed [0,100), open at 100 -> classifies SHORT at
             * 160 (100 + 60 ms debounce). tap 2 starts at 180, an 80 ms
             * gap measured from the release edge at 100 -- well after the
             * first tap already classified and released the FSM. */
            int down = (t < 100) || (t >= 180 && t < 280);
            gesture_t r = gesture_feed(&s, 0, down, (unsigned)t);
            if (r != GEST_NONE && n < 8) got[n++] = r;
        }
        CHECK_EQ_INT(n, 2);
        CHECK_EQ_INT(got[0], GEST_PWR_SHORT);
        CHECK_EQ_INT(got[1], GEST_PWR_SHORT);
    }

    /* ---- Task 19 blessing of two emergent behaviors (ledger r6) ----
     * (a) A PWR press that began DURING reply playback is invisible until
     * the follow-up window's FSM starts sampling (playback polls only
     * REC): the press is latched on the window's first sample, its
     * pre-window duration is not counted, so the release classifies as a
     * tap -- and a tap is exactly what ends the conversation. */
    gesture_init(&g);
    CHECK_EQ_INT(gesture_feed(&g, 0, 1, 0), GEST_NONE);   /* already held at entry */
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 200), GEST_NONE);
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 280), GEST_PWR_SHORT);
    /* (b) REC held past the point it stopped playback keeps being held
     * into the window: hold-to-talk fires 350 ms after the first sample,
     * rolling the same physical hold into the follow-up recording. */
    gesture_init(&g);
    CHECK_EQ_INT(gesture_feed(&g, 1, 0, 0), GEST_NONE);   /* still held from the stop */
    CHECK_EQ_INT(gesture_feed(&g, 1, 0, 340), GEST_NONE);
    CHECK_EQ_INT(gesture_feed(&g, 1, 0, 360), GEST_REC_HOLD_START);

    /* both buttons acting on the same sample: one gesture leaves per call
     * and the PWR release is NOT dropped -- it emerges on the next sample */
    {
        gesture_fsm_t s;
        gesture_init(&s);
        gesture_feed(&s, 1, 1, 0);                       /* both down */
        CHECK_EQ_INT(gesture_feed(&s, 0, 0, 100), GEST_NONE);   /* both open */
        gesture_t a = gesture_feed(&s, 0, 0, 160);
        gesture_t b = gesture_feed(&s, 0, 0, 180);
        CHECK_EQ_INT(a, GEST_REC_SHORT);
        CHECK_EQ_INT(b, GEST_PWR_SHORT);
    }
    return HARNESS_REPORT();
}
