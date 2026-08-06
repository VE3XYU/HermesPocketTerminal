#include "htp_ids.h"
#include <stdio.h>

static void civil_from_days(long long z, int *y, unsigned *m, unsigned *d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long yr = (long long)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yr + (*m <= 2));
}

void htp_make_capture_id(char out[64], long long epoch_s, uint32_t bootcount,
                         unsigned mono_ms, const uint8_t rand2[2]) {
    unsigned suffix = ((unsigned)rand2[0] << 8) | rand2[1];
    if (epoch_s > 0) {
        long long days = epoch_s / 86400;
        long long rem = epoch_s % 86400;
        int y; unsigned m, d;
        civil_from_days(days, &y, &m, &d);
        snprintf(out, 64, "c-%04d%02u%02u-%02lld%02lld%02lld-%04x",
                 y, m, d, rem / 3600, (rem % 3600) / 60, rem % 60, suffix);
    } else {
        snprintf(out, 64, "c-b%lu-%u-%04x", (unsigned long)bootcount, mono_ms, suffix);
    }
}
