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

typedef struct {
    int rec_down, pwr_down;
    unsigned rec_t0, pwr_t0;
    int rec_hold_fired, pwr_off_fired;
} gesture_fsm_t;

void gesture_init(gesture_fsm_t *g);

/* Feed sampled button levels (1 = pressed) + monotonic ms; returns at most one gesture. */
gesture_t gesture_feed(gesture_fsm_t *g, int rec, int pwr, unsigned now_ms);

#endif
