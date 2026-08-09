#ifndef GESTURE_H
#define GESTURE_H

/* Two-button gesture vocabulary (C7 round 5). Four gestures per button
 * pair, no double-tap: the shipped product this hardware comes from runs
 * its entire flow on tap/hold alone, and retiring the double-tap lets a
 * PWR tap resolve ON ITS RELEASE EDGE instead of after a 250 ms
 * are-you-double waiting window -- every tap's feedback starts a quarter
 * second sooner.
 *
 *   GEST_REC_HOLD_START  REC held GEST_REC_HOLD_MS: hold-to-talk begins
 *   GEST_REC_SHORT       REC tapped (released before the hold threshold)
 *   GEST_PWR_SHORT       PWR tapped (released before GEST_PWR_LONG_MS)
 *   GEST_PWR_LONG        PWR released after >= GEST_PWR_LONG_MS ("back")
 *   GEST_PWR_OFF         PWR still held at GEST_PWR_OFF_MS (fires held)
 */
typedef enum { GEST_NONE, GEST_REC_HOLD_START, GEST_REC_SHORT,
               GEST_PWR_SHORT, GEST_PWR_LONG, GEST_PWR_OFF } gesture_t;

#define GEST_REC_HOLD_MS   350
#define GEST_PWR_LONG_MS   600
#define GEST_PWR_OFF_MS    5000

/* RELEASE debounce (C7 round 7, finding 1; widened for the parked C7
 * double-beep in Task 19). A press ends only once the button has read
 * open for this long; any closed sample inside the window cancels the
 * pending release and the press continues from its ORIGINAL press edge.
 * Without it a single soft ("lazy") press -- whose contact chatters
 * open/closed across the caller's ~20 ms poll -- was classified as two
 * gestures and fired two clicks. This is the same discipline the dev
 * linger has had since round 3 (DEV_BTN_DEBOUNCE_MS), which the gesture
 * FSM never got.
 *
 * Value history: round 7 shipped 30 ms, which classifies after 2 open
 * polls (~40 ms) -- and the bench still heard a double beep on releases
 * from LONGER holds, whose slower roll-off re-strikes the contact at
 * gaps of 50 ms and more; a re-strike sampled after classification
 * latched a new press and a second gesture (host repro: test_gesture's
 * chatter cases). 60 ms -- matching DEV_BTN_DEBOUNCE_MS -- waits 3 open
 * polls and absorbs sampled re-strike gaps up to ~70 ms. NOT verified on
 * hardware (the bench wrapped before this change); if the double beep
 * survives, the next lever is this same knob.
 *
 * It is NOT a minimum press duration: any latched press still classifies
 * on its debounced release, and the press length that decides SHORT vs
 * LONG is measured to the moment the button opened, never to the end of
 * the debounce window -- a 599 ms tap stays a tap. Cost, two-fold: (1)
 * classification lands one poll later (~60 ms after the open edge instead
 * of ~40), against the 250 ms window round 5 removed; (2) this is the
 * flip side of the chatter absorption -- TWO DELIBERATE presses (a real
 * double-tap, or PWR-tap-then-immediately-REC-tap on the same button)
 * separated by <= ~70 ms now read as one continuous press instead of two
 * gestures, because a re-close inside the debounce window cancels the
 * pending release unconditionally; the FSM cannot distinguish a chattering
 * contact from a fast second finger. Round 5 already establishes taps
 * this close together are not a supported input (no double-tap
 * vocabulary), so this was judged acceptable; test_gesture.c covers the
 * boundary (both the chatter-absorption side and the ~80-100 ms
 * genuinely-separate side). */
#define GEST_RELEASE_DEBOUNCE_MS 60

typedef struct {
    int rec_down, pwr_down;
    unsigned rec_t0, pwr_t0;
    int rec_hold_fired, pwr_off_fired;
    /* pending-release state: *_open = an open sample is waiting out the
     * debounce, *_open_t0 = the moment the button actually opened */
    int rec_open, pwr_open;
    unsigned rec_open_t0, pwr_open_t0;
} gesture_fsm_t;

void gesture_init(gesture_fsm_t *g);

/* Feed sampled button levels (1 = pressed) + monotonic ms; returns at most one gesture. */
gesture_t gesture_feed(gesture_fsm_t *g, int rec, int pwr, unsigned now_ms);

#endif
