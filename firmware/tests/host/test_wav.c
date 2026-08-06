#include "harness.h"
#include "wav.h"
#include <string.h>

int main(void) {
    uint8_t h[44];
    /* Upload format: 16 kHz, 16-bit, mono, 32000 data bytes (1 s) */
    wav_write_header(h, 16000, 16, 1, 32000);
    CHECK(memcmp(h, "RIFF", 4) == 0);
    CHECK(memcmp(h + 8, "WAVEfmt ", 8) == 0);
    /* RIFF size = 36 + data */
    uint32_t riff = (uint32_t)h[4] | h[5] << 8 | h[6] << 16 | (uint32_t)h[7] << 24;
    CHECK_EQ_INT(riff, 32036);
    /* byte rate = rate * block_align = 16000 * 2 */
    uint32_t brate = (uint32_t)h[28] | h[29] << 8 | h[30] << 16 | (uint32_t)h[31] << 24;
    CHECK_EQ_INT(brate, 32000);
    CHECK_EQ_INT(h[32] | h[33] << 8, 2);   /* block align */
    CHECK_EQ_INT(h[34] | h[35] << 8, 16);  /* bits */

    /* Round-trip through the parser */
    wav_info_t inf;
    CHECK_EQ_INT(wav_parse_header(h, sizeof h, &inf), 0);
    CHECK_EQ_INT(inf.sample_rate, 16000);
    CHECK_EQ_INT(inf.channels, 1);
    CHECK_EQ_INT(inf.bits, 16);
    CHECK_EQ_INT(inf.data_bytes, 32000);
    CHECK_EQ_INT(inf.data_offset, 44);

    /* Reply format: 24 kHz mono parses too */
    wav_write_header(h, 24000, 16, 1, 4800);
    CHECK_EQ_INT(wav_parse_header(h, sizeof h, &inf), 0);
    CHECK_EQ_INT(inf.sample_rate, 24000);

    /* Extra chunk (e.g. LIST) between fmt and data is skipped */
    uint8_t x[64] = {0};
    memcpy(x, "RIFF", 4); uint32_t sz = 56; memcpy(x + 4, &sz, 4);
    memcpy(x + 8, "WAVEfmt ", 8); uint32_t fl = 16; memcpy(x + 16, &fl, 4);
    uint16_t fmt = 1, ch = 1, ba = 2, bits = 16; uint32_t rate = 16000, br = 32000;
    memcpy(x + 20, &fmt, 2); memcpy(x + 22, &ch, 2); memcpy(x + 24, &rate, 4);
    memcpy(x + 28, &br, 4); memcpy(x + 32, &ba, 2); memcpy(x + 34, &bits, 2);
    memcpy(x + 36, "LIST", 4); uint32_t ll = 4; memcpy(x + 40, &ll, 4);
    memcpy(x + 48, "data", 4); uint32_t db = 8; memcpy(x + 52, &db, 4);
    CHECK_EQ_INT(wav_parse_header(x, sizeof x, &inf), 0);
    CHECK_EQ_INT(inf.data_offset, 56);
    CHECK_EQ_INT(inf.data_bytes, 8);

    /* Garbage and short buffers are rejected */
    CHECK_EQ_INT(wav_parse_header((const uint8_t *)"nope", 4, &inf), -1);
    CHECK_EQ_INT(wav_parse_header(h, 10, &inf), -1);
    return HARNESS_REPORT();
}
