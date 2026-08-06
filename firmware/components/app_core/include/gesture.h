#ifndef GESTURE_H
#define GESTURE_H

typedef enum { GEST_NONE, GEST_REC_HOLD_START, GEST_REC_SHORT,
               GEST_PWR_SHORT, GEST_PWR_LONG, GEST_PWR_DOUBLE, GEST_PWR_OFF } gesture_t;

#define GEST_REC_HOLD_MS   350
#define GEST_PWR_LONG_MS   600
#define GEST_PWR_DOUBLE_MS 250
#define GEST_PWR_OFF_MS    5000

typedef struct {
    int rec_down, pwr_down;
    unsigned rec_t0, pwr_t0, pwr_up_t;
    int rec_hold_fired, pwr_off_fired, pwr_pending_short;
} gesture_fsm_t;

void gesture_init(gesture_fsm_t *g);

/* Feed sampled button levels (1 = pressed) + monotonic ms; returns at most one gesture. */
gesture_t gesture_feed(gesture_fsm_t *g, int rec, int pwr, unsigned now_ms);

#endif
