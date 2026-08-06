#include "wav.h"
#include <string.h>

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

void wav_write_header(uint8_t out[44], uint32_t sample_rate, uint16_t bits,
                      uint16_t channels, uint32_t data_bytes) {
    uint16_t block_align = (uint16_t)(channels * bits / 8);
    memcpy(out, "RIFF", 4);          wr32(out + 4, 36 + data_bytes);
    memcpy(out + 8, "WAVEfmt ", 8);  wr32(out + 16, 16);
    wr16(out + 20, 1);               wr16(out + 22, channels);
    wr32(out + 24, sample_rate);     wr32(out + 28, sample_rate * block_align);
    wr16(out + 32, block_align);     wr16(out + 34, bits);
    memcpy(out + 36, "data", 4);     wr32(out + 40, data_bytes);
}

int wav_parse_header(const uint8_t *buf, size_t len, wav_info_t *out) {
    if (len < 44 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) return -1;
    size_t pos = 12;
    int have_fmt = 0;
    while (pos + 8 <= len) {
        uint32_t csz = rd32(buf + pos + 4);
        if (!memcmp(buf + pos, "fmt ", 4)) {
            if (pos + 8 + 16 > len) return -1;
            const uint8_t *f = buf + pos + 8;
            out->channels = rd16(f + 2);
            out->sample_rate = rd32(f + 4);
            out->bits = rd16(f + 14);
            have_fmt = 1;
        } else if (!memcmp(buf + pos, "data", 4)) {
            if (!have_fmt) return -1;
            out->data_bytes = csz;
            out->data_offset = (uint32_t)(pos + 8);
            return 0;
        }
        pos += 8 + csz + (csz & 1);
    }
    return -1;
}
