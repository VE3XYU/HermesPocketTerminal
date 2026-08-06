#include "board.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"

#define PIN_EPD_PWR   6    /* active low  */
#define PIN_AUDIO_PWR 42   /* active low  */
#define PIN_VBAT_HOLD 17   /* active high */
#define PIN_BTN_REC   0
#define PIN_BTN_PWR   18

void board_early_init(void) {
    gpio_config_t out = { .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_EPD_PWR) | (1ULL << PIN_AUDIO_PWR) | (1ULL << PIN_VBAT_HOLD) };
    gpio_config(&out);
    gpio_hold_dis(PIN_VBAT_HOLD);
    gpio_hold_dis(PIN_EPD_PWR);                /* release the hold armed before sleeping */
    gpio_hold_dis(PIN_AUDIO_PWR);
    gpio_set_level(PIN_VBAT_HOLD, 1);          /* keep the board alive */
    gpio_set_level(PIN_EPD_PWR, 1);            /* rails off until needed */
    gpio_set_level(PIN_AUDIO_PWR, 1);
    gpio_config_t in = { .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
        .pin_bit_mask = (1ULL << PIN_BTN_REC) | (1ULL << PIN_BTN_PWR) };
    gpio_config(&in);
}

void board_rail_epd(int on)   { gpio_set_level(PIN_EPD_PWR, !on); }
void board_rail_audio(int on) { gpio_set_level(PIN_AUDIO_PWR, !on); }

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
