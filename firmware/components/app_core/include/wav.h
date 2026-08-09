#ifndef WAV_H
#define WAV_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t sample_rate; uint16_t bits, channels;
    uint32_t data_bytes;  uint32_t data_offset;
} wav_info_t;

void wav_write_header(uint8_t out[44], uint32_t sample_rate, uint16_t bits,
                      uint16_t channels, uint32_t data_bytes);
int  wav_parse_header(const uint8_t *buf, size_t len, wav_info_t *out); // 0 ok, -1 bad

#endif
