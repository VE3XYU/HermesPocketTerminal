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

int main(void) {
    gesture_fsm_t g;

    /* REC hold fires at 350 ms while still held */
    gesture_init(&g);
    const int hold[][3] = { {1,0,0}, {1,0,100}, {1,0,349}, {1,0,351} };
    CHECK_EQ_INT(run(&g, hold, 4), GEST_REC_HOLD_START);
    /* ...and does not fire again, nor emit SHORT on release */
    CHECK_EQ_INT(gesture_feed(&g, 1, 0, 400), GEST_NONE);
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 500), GEST_NONE);

    /* REC quick tap = SHORT on release */
    gesture_init(&g);
    const int tap[][3] = { {1,0,0}, {1,0,100}, {0,0,200} };
    CHECK_EQ_INT(run(&g, tap, 3), GEST_REC_SHORT);

    /* PWR short: release <600, no second press within 250 → SHORT after window */
    gesture_init(&g);
    const int pshort[][3] = { {0,1,0}, {0,1,100}, {0,0,200}, {0,0,300}, {0,0,460} };
    CHECK_EQ_INT(run(&g, pshort, 5), GEST_PWR_SHORT);

    /* PWR double: second press-down within 250 of release */
    gesture_init(&g);
    const int pdouble[][3] = { {0,1,0}, {0,0,150}, {0,1,300}, {0,0,380} };
    CHECK_EQ_INT(run(&g, pdouble, 4), GEST_PWR_DOUBLE);

    /* PWR long: release after 600 */
    gesture_init(&g);
    const int plong[][3] = { {0,1,0}, {0,1,650}, {0,0,700} };
    CHECK_EQ_INT(run(&g, plong, 3), GEST_PWR_LONG);

    /* PWR off: still held at 5000, fires while held */
    gesture_init(&g);
    const int poff[][3] = { {0,1,0}, {0,1,2000}, {0,1,4999}, {0,1,5001} };
    CHECK_EQ_INT(run(&g, poff, 4), GEST_PWR_OFF);
    /* release afterwards emits nothing */
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 5200), GEST_NONE);
    return HARNESS_REPORT();
}
