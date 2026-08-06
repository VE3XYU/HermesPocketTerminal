#ifndef BOARD_H
#define BOARD_H
/* Board support: power rails, buttons, sleep/wake.
 * Pin map (design §2): Record GPIO 0, Power GPIO 18,
 * rails EPD 6 / audio 42 (active-low), VBAT latch 17 (active-high),
 * EXT1 any-low wake on both buttons.
 * Grows over Tasks 13-18; this is Task 12's slice. */

void board_early_init(void);       /* rail GPIO config + VBAT latch high; call first */
void board_rail_epd(int on);       /* active-low gate handled inside */
void board_rail_audio(int on);
int  board_btn_rec(void);          /* 1 = pressed (level low) */
int  board_btn_pwr(void);
typedef enum { WAKE_COLD, WAKE_REC_BUTTON, WAKE_PWR_BUTTON, WAKE_TIMER } wake_cause_t;
wake_cause_t board_wake_cause(void);
void board_deep_sleep(unsigned seconds);   /* arms EXT1 both-buttons + timer, latches VBAT, never returns */
void board_power_off(void);                /* releases VBAT latch */

#endif
