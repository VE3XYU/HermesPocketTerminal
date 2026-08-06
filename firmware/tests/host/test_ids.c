#include "harness.h"
#include "htp_ids.h"
#include <ctype.h>

static int valid_alphabet(const char *s) {
    if (!*s) return 0;
    for (; *s; s++)
        if (!(isalnum((unsigned char)*s) || *s == '_' || *s == '-')) return 0;
    return 1;
}

int main(void) {
    char id[64];
    const uint8_t r[2] = {0x3f, 0xa9};

    /* RTC set: 1785838502 == 2026-08-04 10:15:02 UTC */
    htp_make_capture_id(id, 1785838502LL, 7, 1234, r);
    CHECK_EQ_STR(id, "c-20260804-101502-3fa9");
    CHECK(valid_alphabet(id));

    /* RTC never set (epoch <= 0): boot counter + monotonic ms */
    htp_make_capture_id(id, 0, 17, 4523, r);
    CHECK_EQ_STR(id, "c-b17-4523-3fa9");
    CHECK(valid_alphabet(id));

    /* Random suffix is zero-padded */
    const uint8_t r2[2] = {0x00, 0x0a};
    htp_make_capture_id(id, 1785838502LL, 0, 0, r2);
    CHECK_EQ_STR(id, "c-20260804-101502-000a");

    /* str_copy truncates safely */
    char small[8];
    extern size_t str_copy(char *, size_t, const char *);
    size_t n = str_copy(small, sizeof small, "abcdefghij");
    CHECK_EQ_INT(n, 7);
    CHECK_EQ_STR(small, "abcdefg");
    return HARNESS_REPORT();
}
