#ifndef BOARD_PRIV_H
#define BOARD_PRIV_H
/* Board-component-internal helpers. Not installed (lives in src/, not
 * include/): only board/src files may include it, so the shared-bus and
 * rail-timing plumbing never leaks into main/ or the pure components. */
#include "driver/i2c_master.h"

/* The one I2C master bus (I2C_NUM_0, SDA 47 / SCL 48) shared by the ES8311
 * codec and the PCF85063 RTC, both proven on this bus by the reference
 * firmware's pin map. Created lazily on first call, never deleted -- the
 * RTC needs it in sessions where audio never comes up, and audio_deinit()
 * must not tear it out from under the RTC. Returns NULL on create failure.
 *
 * Rail policy (reference init order: the codec rail is always gated on
 * before the first bus transaction): an unpowered ES8311 sharing SDA/SCL
 * could load/clamp the bus, so this accessor gates the audio rail on and
 * gives it a short settle before handing the bus out. audio_init() still
 * applies its own longer codec-LDO settle on top. */
i2c_master_bus_handle_t board_i2c_bus(void);

/* Milliseconds since the audio rail was last gated on; -1 while it is off.
 * Lets audio_init() subtract rail-settle time that already elapsed (the
 * capture fast path gates the rail on before the SD mount so the settle
 * overlaps it) instead of always sleeping the full settle window. */
int board_rail_audio_on_ms(void);

/* Same for the EPD rail: milliseconds since it was last gated on; -1 while
 * off. epd_init() subtracts already-elapsed settle so a caller that
 * pre-gated the rail (main.c, button wakes) doesn't pay the full window
 * again on the first-frame path. */
int board_rail_epd_on_ms(void);

#endif
