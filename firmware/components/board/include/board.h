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

#endif
