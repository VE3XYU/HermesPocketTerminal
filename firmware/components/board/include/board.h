#ifndef BOARD_H
#define BOARD_H
#include <stdint.h>
/* Board support: power rails, buttons, sleep/wake.
 * Pin map (design §2): Record GPIO 0, Power GPIO 18,
 * rails EPD 6 / audio 42 (active-low), VBAT latch 17 (active-high),
 * EXT1 any-low wake on both buttons.
 * Grows over Tasks 13-18; this is Task 12's slice. */

void board_early_init(void);       /* rail GPIO config + VBAT latch high; call first */
void board_rail_epd(int on);       /* active-low gate handled inside */
void board_rail_audio(int on);
int  board_rail_epd_level(void);   /* raw EPD rail gate pad readback: 0 = rail on
                                      (active-low); for display diagnostics */
int  board_btn_rec(void);          /* 1 = pressed (level low) */
int  board_btn_pwr(void);
typedef enum { WAKE_COLD, WAKE_REC_BUTTON, WAKE_PWR_BUTTON, WAKE_TIMER } wake_cause_t;
wake_cause_t board_wake_cause(void);
void board_deep_sleep(unsigned seconds);   /* arms EXT1 both-buttons + timer, latches VBAT, never returns */
void board_power_off(void);                /* releases VBAT latch */

/* SSD1681 e-paper driver (Task 13). fb5000 buffers are already in SSD1681
 * RAM format (Task 8's ui_fb_t.px): bit set = white, MSB = leftmost,
 * 25-byte stride, 5000 bytes total. Send raw, no conversion. */
int  epd_init(void);                       /* rail on, HW init; 0 ok */
void epd_full(const uint8_t *fb5000);      /* full refresh (~1.5 s, flashes) */
void epd_partial(const uint8_t *fb5000);   /* partial refresh (fast, may ghost) */
void epd_sleep(void);                      /* deep-sleep cmd + rail off */

/* SD card storage (Task 14). SD-MMC 1-bit mode: CLK 39, CMD 41, D0 40. */
int board_sd_mount(void);                  /* mounts FAT at /sdcard, creates /sdcard/rec; 0 ok */
int board_sd_format_and_mount(void);       /* erase+format (only call after board_sd_mount() failed), then mount; 0 ok */
int board_sd_unmount_and_format(void);     /* unmount (if currently mounted) + erase+format + mount; for a mounted card that refuses writes; 0 ok */

/* ES8311 audio (Task 15). Pins: I2C SDA 47 / SCL 48 (codec control),
 * I2S MCLK 14 / BCLK 15 / WS 38 / DOUT 45 / DIN 16, PA enable GPIO 46.
 * Capture and playback share one full-duplex I2S port and one codec, so
 * only one of them runs at a time. */
int  audio_init(void);   /* rail on, I2C + I2S + esp_codec_dev up, 16 kHz ready; 0 ok.
                            Idempotent; cleans up after itself on failure. */
/* Record 16 kHz/16-bit/mono WAV to path until keep_going() returns 0 or max_ms
 * elapses. Streams to SD as it goes; patches the header on stop.
 * keep_going() is sampled once per ~128 ms capture chunk.
 * Returns data bytes written, or -1 (SD write failure -> explicit error). */
long audio_record_to(const char *path, int (*keep_going)(void *), void *ctx, unsigned max_ms);
/* Plays a WAV from SD; sample rate/channels come from ITS header (16 k uploads,
 * 24 k replies both work). stop_now() polled between chunks. 0 ok. */
int  audio_play_wav(const char *path, int (*stop_now)(void *), void *ctx);
void audio_beep(void);   /* short 1 kHz chime, generated, no asset */
/* UI click feedback (C7 round 5): a generated square blip, queued to the
 * DMA ring and returned from immediately (no drain wait) so a keypress
 * click never blocks the gesture loop. select=0 -> "next" (1 kHz, 30 ms),
 * select=1 -> "select" (2 kHz, 45 ms). Silent no-op unless audio_init()
 * has run -- callers degrade gracefully when the codec is down. */
void audio_click(int select);
void audio_deinit(void); /* PA off, codec closed, rail off */

/* Battery percent (Task 16). ADC1 channel 3 = GPIO 4, oneshot, 8 samples
 * averaged through a nominal 2:1 divider and a 3300-4200 mV line, clamped
 * to 0..100. Returns -1 when the ADC itself is unavailable, which is also
 * htp_client's "omit the X-Battery header" value. The curve is nominal
 * until Task 19 calibrates it against real hardware (design §11.3). */
int board_battery_pct(void);

/* PCF85063 RTC (Task 17) @ I2C 0x51 on the shared codec bus. (The plan
 * called the part a PCF8563; the board's reference firmware drives a
 * PCF85063 register map at the same address -- see rtc_pcf85063.c.)
 * get: UTC epoch seconds, or 0 when the chip has no valid time (OS flag /
 * never set). set: writes the chip and mirrors the same epoch into the
 * system clock via settimeofday(); ignores obviously bogus epochs. */
long long board_rtc_get(void);
void      board_rtc_set(long long epoch);

#endif
