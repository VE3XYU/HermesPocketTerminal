#ifndef TICK_MS_H
#define TICK_MS_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Non-truncating millisecond -> FreeRTOS tick conversion.
 *
 * pdMS_TO_TICKS() rounds *towards zero*. At this project's
 * CONFIG_FREERTOS_HZ=100 (10 ms/tick) every value below 10 ms becomes 0
 * ticks, and vTaskDelay(0) is a bare yield, not a delay -- so a "wait 5 ms"
 * request silently becomes "don't wait at all". That exact truncation is
 * what made the SSD1681 BUSY poll spin instead of wait (see
 * epd_ssd1681.c's busy-poll comment) and cost days on checkpoint C2.
 *
 * Every device-side delay and driver timeout goes through here so that a
 * nonzero millisecond request always means at least one real tick of
 * waiting. Header-only + static inline: the ESP side has several
 * independent consumers (board drivers, main/, the port layer) and one
 * shared definition beats four private copies drifting apart.
 */
static inline TickType_t ms_to_ticks_min1(unsigned ms)
{
    TickType_t t = pdMS_TO_TICKS(ms);
    return (t == 0 && ms > 0) ? 1 : t;
}

/* vTaskDelay() with the same floor. */
static inline void board_delay_ms(unsigned ms)
{
    vTaskDelay(ms_to_ticks_min1(ms));
}

#endif
