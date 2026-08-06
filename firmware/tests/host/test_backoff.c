#include "harness.h"
#include "htp_backoff.h"

int main(void) {
    htp_backoff_t b;
    htp_backoff_init(&b, 1000, 30000);
    CHECK_EQ_INT(htp_backoff_next(&b), 1000);
    CHECK_EQ_INT(htp_backoff_next(&b), 2000);
    CHECK_EQ_INT(htp_backoff_next(&b), 4000);
    CHECK_EQ_INT(htp_backoff_next(&b), 8000);
    CHECK_EQ_INT(htp_backoff_next(&b), 16000);
    CHECK_EQ_INT(htp_backoff_next(&b), 30000);  /* capped */
    CHECK_EQ_INT(htp_backoff_next(&b), 30000);  /* stays capped */
    htp_backoff_init(&b, 1000, 30000);
    CHECK_EQ_INT(htp_backoff_next(&b), 1000);   /* re-init resets */
    return HARNESS_REPORT();
}
