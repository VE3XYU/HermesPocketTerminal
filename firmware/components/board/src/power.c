#include "board.h"
#include "board_priv.h"   /* board_rail_audio_on_ms() prototype */
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "esp_timer.h"

#define PIN_EPD_PWR   6    /* active low  */
#define PIN_AUDIO_PWR 42   /* active low  */
#define PIN_VBAT_HOLD 17   /* active high */
#define PIN_BTN_REC   0
#define PIN_BTN_PWR   18

void board_early_init(void) {
    /* INPUT_OUTPUT (not plain OUTPUT): same push-pull drive, but the input
     * buffer stays enabled so board_rail_epd_level() reads the real pad
     * level back for display diagnostics (a disabled input buffer makes
     * gpio_get_level() on an output pin always return 0). */
    gpio_config_t out = { .mode = GPIO_MODE_INPUT_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_EPD_PWR) | (1ULL << PIN_AUDIO_PWR) | (1ULL << PIN_VBAT_HOLD) };
    gpio_config(&out);

    /* Levels first, holds released second. A pad that is held ignores both
     * the mode from gpio_config() above and these writes -- they land in
     * the output register and only reach the pad when the hold is dropped.
     * Releasing first would instead expose whatever the output register
     * happens to hold coming out of a deep-sleep reset (all zeros), which
     * for these active-low gates means "both rails momentarily ON" and for
     * the active-high VBAT latch means "let go of the power latch". */
    gpio_set_level(PIN_VBAT_HOLD, 1);          /* keep the board alive */
    gpio_set_level(PIN_EPD_PWR, 1);            /* rails off until needed */
    gpio_set_level(PIN_AUDIO_PWR, 1);
    gpio_deep_sleep_hold_dis();                /* clears the digital-pad autohold latch that
                                                  board_deep_sleep() armed; it lives in the RTC
                                                  domain and survives the wake reset */
    gpio_hold_dis(PIN_VBAT_HOLD);              /* release the holds armed before sleeping */
    gpio_hold_dis(PIN_EPD_PWR);
    gpio_hold_dis(PIN_AUDIO_PWR);

    gpio_config_t in = { .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
        .pin_bit_mask = (1ULL << PIN_BTN_REC) | (1ULL << PIN_BTN_PWR) };
    gpio_config(&in);
}

void board_rail_epd(int on)   { gpio_set_level(PIN_EPD_PWR, !on); }

/* Rail-on timestamp for board_rail_audio_on_ms() (board_priv.h): lets
 * audio_init() and the shared-I2C-bus accessor subtract settle time that
 * already elapsed instead of always sleeping the full window. The pad is
 * INPUT_OUTPUT (see board_early_init), so the current gate state can be
 * read back to make repeated "on" calls keep the original timestamp. */
static int64_t s_audio_rail_on_us = -1;

void board_rail_audio(int on) {
    if (on) {
        if (gpio_get_level(PIN_AUDIO_PWR) != 0 || s_audio_rail_on_us < 0)
            s_audio_rail_on_us = esp_timer_get_time();
    } else {
        s_audio_rail_on_us = -1;
    }
    gpio_set_level(PIN_AUDIO_PWR, !on);
}

int board_rail_audio_on_ms(void) {
    if (s_audio_rail_on_us < 0) return -1;
    int64_t ms = (esp_timer_get_time() - s_audio_rail_on_us) / 1000;
    return ms > 0x7fffffff ? 0x7fffffff : (int)ms;
}

int  board_rail_epd_level(void) { return gpio_get_level(PIN_EPD_PWR); }

wake_cause_t board_wake_cause(void) {
    switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT1: {
        uint64_t pins = esp_sleep_get_ext1_wakeup_status();
        if (pins & (1ULL << PIN_BTN_REC)) return WAKE_REC_BUTTON;
        return WAKE_PWR_BUTTON;
    }
    case ESP_SLEEP_WAKEUP_TIMER: return WAKE_TIMER;
    default: return WAKE_COLD;
    }
}

void board_deep_sleep(unsigned seconds) {
    gpio_set_level(PIN_VBAT_HOLD, 1);
    gpio_hold_en(PIN_VBAT_HOLD);               /* survives deep sleep */
    gpio_set_level(PIN_EPD_PWR, 1);            /* rail off (active-low) before sleeping ... */
    gpio_hold_en(PIN_EPD_PWR);                 /* ... else sleep_gpio isolation lets it float */
    gpio_set_level(PIN_AUDIO_PWR, 1);
    gpio_hold_en(PIN_AUDIO_PWR);
    /* GPIO 17 and 6 are RTC pins (RTC IO covers GPIO 0-21 on the S3), so
     * gpio_hold_en() routes them to rtc_gpio_hold_en() and the RTC domain
     * keeps them latched on its own. GPIO 42 is NOT: it is a digital-only
     * pad, and on the ESP32-S3 a digital pad's individual hold bit is
     * ignored during deep sleep unless the global autohold is also armed
     * (see esp-idf driver/gpio.h on gpio_hold_en: "on ESP32/S2/C3/S3/C2
     * this function cannot be used to hold the state of a digital GPIO
     * during Deep-sleep ... please call gpio_deep_sleep_hold_en"). Without
     * this line the audio rail gate floats through sleep, which on an
     * active-low gate can mean the analog rail powers back up and drains
     * the battery for the whole sleep window. Only pads whose own hold bit
     * is set are affected, i.e. exactly the three above. */
    gpio_deep_sleep_hold_en();
    esp_sleep_enable_ext1_wakeup((1ULL << PIN_BTN_REC) | (1ULL << PIN_BTN_PWR),
                                 ESP_EXT1_WAKEUP_ANY_LOW);
    if (seconds) esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
    esp_deep_sleep_start();
}

void board_power_off(void) {
    gpio_hold_dis(PIN_VBAT_HOLD);
    gpio_set_level(PIN_VBAT_HOLD, 0);          /* hardware powers down */
    while (1) { }
}
