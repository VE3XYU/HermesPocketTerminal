#include "gesture.h"
#include <string.h>

void gesture_init(gesture_fsm_t *g) { memset(g, 0, sizeof *g); }

/* Both buttons run the same edge discipline (C7 round 7, finding 1):
 *
 *   press   latched on the FIRST closed sample (no minimum duration), and
 *           the latch survives any open glitch shorter than
 *           GEST_RELEASE_DEBOUNCE_MS -- a closed sample simply cancels the
 *           pending release and leaves *_t0 alone.
 *   release classified only after the button has read open for the whole
 *           debounce window, and classified against *_open_t0 (when it
 *           actually opened), not against `now`.
 *
 * Everything that fires WHILE HELD (REC hold-to-talk, PWR power-off) is
 * untouched by the debounce: those thresholds are measured from the press
 * edge and fire on a closed sample.
 *
 * At most one gesture leaves per call. When REC has already claimed the
 * slot, PWR keeps its pending state instead of consuming it silently, so
 * the PWR gesture emerges on the next sample rather than being dropped. */
gesture_t gesture_feed(gesture_fsm_t *g, int rec, int pwr, unsigned now) {
    gesture_t out = GEST_NONE;

    /* record button */
    if (rec) {
        g->rec_open = 0;                    /* closed: cancel any pending release */
        if (!g->rec_down) {
            g->rec_down = 1; g->rec_t0 = now; g->rec_hold_fired = 0;
        } else if (!g->rec_hold_fired && now - g->rec_t0 >= GEST_REC_HOLD_MS) {
            g->rec_hold_fired = 1; out = GEST_REC_HOLD_START;
        }
    } else if (g->rec_down) {
        if (!g->rec_open) { g->rec_open = 1; g->rec_open_t0 = now; }
        else if (now - g->rec_open_t0 >= GEST_RELEASE_DEBOUNCE_MS) {
            g->rec_down = 0; g->rec_open = 0;
            if (!g->rec_hold_fired) out = GEST_REC_SHORT;   /* else the hold was consumed */
        }
    }

    /* power button: SHORT/LONG resolve on the DEBOUNCED release edge --
     * there is no double-tap in the vocabulary, so no post-release waiting
     * window (C7 round 5; the removed window cost every tap 250 ms of
     * latency). */
    if (pwr) {
        g->pwr_open = 0;
        if (!g->pwr_down) {
            g->pwr_down = 1; g->pwr_t0 = now; g->pwr_off_fired = 0;
        } else if (!g->pwr_off_fired && now - g->pwr_t0 >= GEST_PWR_OFF_MS &&
                   out == GEST_NONE) {
            g->pwr_off_fired = 1; out = GEST_PWR_OFF;
        }
    } else if (g->pwr_down) {
        if (!g->pwr_open) { g->pwr_open = 1; g->pwr_open_t0 = now; }
        else if (now - g->pwr_open_t0 >= GEST_RELEASE_DEBOUNCE_MS) {
            if (g->pwr_off_fired) {
                g->pwr_down = 0; g->pwr_open = 0;   /* the hold was consumed */
            } else if (out == GEST_NONE) {
                g->pwr_down = 0; g->pwr_open = 0;
                out = (g->pwr_open_t0 - g->pwr_t0 >= GEST_PWR_LONG_MS)
                      ? GEST_PWR_LONG : GEST_PWR_SHORT;
            }
        }
    }
    return out;
}
