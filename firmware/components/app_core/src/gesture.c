#include "gesture.h"
#include <string.h>

void gesture_init(gesture_fsm_t *g) { memset(g, 0, sizeof *g); }

gesture_t gesture_feed(gesture_fsm_t *g, int rec, int pwr, unsigned now) {
    gesture_t out = GEST_NONE;

    /* record button */
    if (rec && !g->rec_down) { g->rec_down = 1; g->rec_t0 = now; g->rec_hold_fired = 0; }
    else if (rec && g->rec_down && !g->rec_hold_fired && now - g->rec_t0 >= GEST_REC_HOLD_MS) {
        g->rec_hold_fired = 1; out = GEST_REC_HOLD_START;
    } else if (!rec && g->rec_down) {
        g->rec_down = 0;
        if (!g->rec_hold_fired && out == GEST_NONE) out = GEST_REC_SHORT;
    }

    /* power button: SHORT resolves on the release edge itself -- there is
     * no double-tap in the vocabulary, so no post-release waiting window
     * (C7 round 5; the removed window cost every tap 250 ms of latency). */
    if (pwr && !g->pwr_down) {
        g->pwr_down = 1; g->pwr_t0 = now; g->pwr_off_fired = 0;
    } else if (pwr && g->pwr_down && !g->pwr_off_fired && now - g->pwr_t0 >= GEST_PWR_OFF_MS) {
        g->pwr_off_fired = 1;
        if (out == GEST_NONE) out = GEST_PWR_OFF;
    } else if (!pwr && g->pwr_down) {
        g->pwr_down = 0;
        if (!g->pwr_off_fired && out == GEST_NONE)
            out = (now - g->pwr_t0 >= GEST_PWR_LONG_MS) ? GEST_PWR_LONG : GEST_PWR_SHORT;
    }
    return out;
}
