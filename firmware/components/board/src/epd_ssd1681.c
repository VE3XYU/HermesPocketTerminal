/* SSD1681 e-paper driver (1.54" 200x200 mono panel).
 * Pins (design §2, proven at C1): DC 10, CS 11, SCK 12, MOSI 13, RST 9,
 * BUSY 8, SPI2_HOST. CS is handed to the SPI driver (spics_io_num) so it
 * toggles automatically per transaction; DC/RST are plain GPIOs we drive
 * ourselves, BUSY is a plain GPIO input we poll.
 *
 * Uses the controller's OTP waveforms only: 0x22/0xF7 for full refresh,
 * 0x22/0xFF (display mode 2) for partial refresh. No custom LUT is
 * uploaded, so no LUT table ships in this file.
 */
#include "board.h"
#include "tick_ms.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stddef.h>

#define PIN_DC    10
#define PIN_CS    11
#define PIN_SCK   12
#define PIN_MOSI  13
#define PIN_RST   9
#define PIN_BUSY  8
#define EPD_HOST  SPI2_HOST

#define EPD_FRAME_BYTES 5000   /* 25 bytes/row * 200 rows; matches ui_fb_t */

#define EPD_BUSY_POLL_MS         10    /* exactly one RTOS tick at this project's
                                          CONFIG_FREERTOS_HZ=100; see poll_delay() */
#define EPD_BUSY_TIMEOUT_MS      3000
#define EPD_BUSY_ASSERT_MS       50     /* time BUSY may take to rise after a trigger */
#define EPD_BUSY_TRIGGER_TIMEOUT_MS 5000 /* full-refresh headroom once BUSY is asserted */
#define EPD_SWRESET_GRACE_MS     10     /* let BUSY rise after SWRESET before polling it;
                                           untuned (order-of-datasheet SWRESET duration),
                                           guards the same pre-rise race busy_wait_after_
                                           trigger() closes for update triggers */

static const char *TAG = "epd";
static spi_device_handle_t s_spi;
/* The SPI bus + device are created once per POWER CYCLE and never freed;
 * epd_init() is called once per SESSION and there can be several sessions
 * per power cycle (the dev linger's dispatch loop; the common teardown's
 * epd_sleep() clears main.c's s_epd_up, so the next session re-inits).
 * See the latch in epd_init(). */
static bool s_bus_up;

/* Wall-clock milliseconds since boot. Every busy-wait timeout below is
 * measured against this, never by counting poll iterations. The previous
 * implementation summed a nominal EPD_BUSY_POLL_MS per loop pass -- but
 * with EPD_BUSY_POLL_MS=5 and CONFIG_FREERTOS_HZ=100 (10 ms/tick),
 * vTaskDelay(pdMS_TO_TICKS(5)) is vTaskDelay(0): a yield, not a delay.
 * So the "5 s" post-trigger timeout really expired after ~1000
 * back-to-back polls -- a few MILLISECONDS of wall time -- after which the
 * harness sent the panel deep-sleep command and cut its rail mid-refresh.
 * That aborted every refresh since the original C2 run (whose inter-
 * partial 800 ms harness delays were the only real wall time the panel
 * ever got). Same pdMS_TO_TICKS truncation tick_ms.h's ms_to_ticks_min1()
 * guards against, caught there but missed here. */
static int64_t uptime_ms(void) { return esp_timer_get_time() / 1000; }

/* Sleep at least one real tick between BUSY polls (pdMS_TO_TICKS()
 * truncates toward zero; never let it become a zero-tick no-op again). */
static void poll_delay(void) { board_delay_ms(EPD_BUSY_POLL_MS); }

/* BUSY is HIGH while the controller is busy, LOW when idle (confirmed
 * against reference/pala_note's read_busy(), which loops while HIGH). */
static bool busy_wait(void) {
    int64_t start = uptime_ms();
    while (gpio_get_level(PIN_BUSY) == 1) {
        if (uptime_ms() - start >= EPD_BUSY_TIMEOUT_MS) {
            ESP_LOGE(TAG, "BUSY timeout (still high %lld ms after wait began)",
                     (long long)(uptime_ms() - start));
            return false;
        }
        poll_delay();
    }
    return true;
}

/* Same as busy_wait(), but for use right after an update trigger (cmd 0x20).
 * BUSY takes a moment to rise once the controller starts driving the
 * waveform; polling for "clear" immediately can catch it still LOW from
 * before the trigger and return instantly, letting the caller (and then
 * epd_sleep's rail cut) run while the panel is mid-refresh. So: first wait
 * up to EPD_BUSY_ASSERT_MS for BUSY to assert (go HIGH), then wait for it
 * to clear, with the longer timeout a full refresh needs. If BUSY never
 * asserts within the window we still fall through to the clear-wait rather
 * than failing outright -- a real controller could plausibly finish inside
 * one poll tick and we'd rather not false-fail on a fast partial. */
static bool busy_wait_after_trigger(void) {
    int64_t start = uptime_ms();
    ESP_LOGI(TAG, "BUSY=%d just after trigger", gpio_get_level(PIN_BUSY));
    while (gpio_get_level(PIN_BUSY) == 0) {
        if (uptime_ms() - start >= EPD_BUSY_ASSERT_MS) {
            ESP_LOGW(TAG, "BUSY did not assert within %d ms of trigger", EPD_BUSY_ASSERT_MS);
            break;
        }
        poll_delay();
    }
    while (gpio_get_level(PIN_BUSY) == 1) {
        if (uptime_ms() - start >= EPD_BUSY_ASSERT_MS + EPD_BUSY_TRIGGER_TIMEOUT_MS) {
            ESP_LOGE(TAG, "BUSY timeout after trigger (still high %lld ms after trigger)",
                     (long long)(uptime_ms() - start));
            return false;
        }
        poll_delay();
    }
    ESP_LOGI(TAG, "refresh done: BUSY cleared %lld ms after trigger",
             (long long)(uptime_ms() - start));
    return true;
}

static void spi_send(const uint8_t *buf, size_t len) {
    if (!len) return;
    spi_transaction_t t = { 0 };
    t.length = len * 8;
    t.tx_buffer = buf;
    esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    if (err != ESP_OK) ESP_LOGE(TAG, "spi tx failed: %d", err);
}

static void cmd(uint8_t c) {
    gpio_set_level(PIN_DC, 0);
    spi_send(&c, 1);
}
static void data1(uint8_t d) {
    gpio_set_level(PIN_DC, 1);
    spi_send(&d, 1);
}
static void data2(uint8_t a, uint8_t b) {
    uint8_t buf[2] = { a, b };
    gpio_set_level(PIN_DC, 1);
    spi_send(buf, sizeof buf);
}
static void data3(uint8_t a, uint8_t b, uint8_t c) {
    uint8_t buf[3] = { a, b, c };
    gpio_set_level(PIN_DC, 1);
    spi_send(buf, sizeof buf);
}
static void data4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    uint8_t buf[4] = { a, b, c, d };
    gpio_set_level(PIN_DC, 1);
    spi_send(buf, sizeof buf);
}
static void dataN(const uint8_t *buf, size_t len) {
    gpio_set_level(PIN_DC, 1);
    spi_send(buf, len);
}

/* Hardware reset. This is the ONLY way to bring the SSD1681 back out of the
 * controller's own Deep Sleep Mode 1 (entered by epd_sleep()'s 0x10/0x01) --
 * a short low pulse is enough to satisfy the datasheet's minimum pulse
 * width, but leaves too little settle time either side: BUSY can read back
 * a stale/floating LOW immediately after RST goes high (the controller
 * hasn't started driving it again yet), so a busy_wait() called too early
 * sails through thinking the controller is idle when it's still mid-wake.
 * That let epd_init() "succeed" (register-write commands don't need the
 * analog domain and ack fine) while the panel was still actually asleep;
 * the failure only showed up later as a permanently-stuck BUSY on the next
 * real display-update trigger. Timing below (50 ms high settle / 20 ms low
 * hold / 50 ms high settle) matches what reference/pala_note's
 * epaper_driver_bsp.cpp uses for this same panel -- consulted for the
 * intervals only, this implementation is our own. */
static bool reset_pulse(void) {
    ESP_LOGI(TAG, "BUSY=%d RST=%d before reset pulse",
             gpio_get_level(PIN_BUSY), gpio_get_level(PIN_RST));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));   /* settle high first, in case RST was left low/floating */
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));   /* hold low */
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));   /* let the controller actually start driving BUSY again
                                        before we trust a read of it */
    ESP_LOGI(TAG, "BUSY=%d after reset pulse", gpio_get_level(PIN_BUSY));
    return busy_wait();
}

/* Resets the RAM address counters to (0,0) and streams a full window's
 * worth of bytes into B/W RAM (0x24). Shared by full and partial refresh;
 * must be re-run before every window-sized RAM write since the counters
 * end up wrapped/advanced after a prior write. */
static void frame_write(const uint8_t *fb) {
    cmd(0x4E); data1(0x00);
    cmd(0x4F); data2(0x00, 0x00);
    cmd(0x24); dataN(fb, EPD_FRAME_BYTES);
}

int epd_init(void) {
    ESP_LOGI(TAG, "init: rail gate=%d (0=on, active-low) at entry", board_rail_epd_level());
    board_rail_epd(1);
    vTaskDelay(pdMS_TO_TICKS(100));  /* rail settle margin for a cold panel; not datasheet-mandated */

    /* INPUT_OUTPUT (not plain OUTPUT) so the diagnostic logs below read the
     * real pad level back, not the always-0 a disabled input buffer gives. */
    gpio_config_t dc_rst = {
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_DC) | (1ULL << PIN_RST),
    };
    gpio_config(&dc_rst);
    gpio_config_t busy_in = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PIN_BUSY),
    };
    gpio_config(&busy_in);
    ESP_LOGI(TAG, "init: rail gate=%d RST=%d BUSY=%d after rail-on settle",
             board_rail_epd_level(), gpio_get_level(PIN_RST), gpio_get_level(PIN_BUSY));

    /* Bus/device creation is latched; the panel-wake sequence below it is
     * not. A second epd_init() in the same power cycle (a linger-launched
     * session) would otherwise take ESP_ERR_INVALID_STATE from
     * spi_bus_initialize() on a bus that is still perfectly usable, return
     * -1, and fail screen_ready() for the whole session: no screens, no
     * notification renders (so no acks), and the EPD rail left powered
     * through the linger. Nothing here frees the bus -- deep sleep's reset
     * re-creates it from scratch on the next boot, and within one power
     * cycle epd_sleep() only sleeps the *panel* and drops its rail, so the
     * driver objects stay valid across sleep/init. */
    if (!s_bus_up) {
        spi_bus_config_t buscfg = {
            .mosi_io_num = PIN_MOSI,
            .miso_io_num = -1,
            .sclk_io_num = PIN_SCK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = EPD_FRAME_BYTES,
        };
        esp_err_t err = spi_bus_initialize(EPD_HOST, &buscfg, SPI_DMA_CH_AUTO);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "spi_bus_initialize failed: %d", err);
            return -1;
        }

        spi_device_interface_config_t devcfg = {
            .clock_speed_hz = 10 * 1000 * 1000,
            .mode = 0,
            .spics_io_num = PIN_CS,
            .queue_size = 1,
        };
        err = spi_bus_add_device(EPD_HOST, &devcfg, &s_spi);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "spi_bus_add_device failed: %d", err);
            /* Free the bus so the latch never claims more than what exists:
             * a retry must be able to run spi_bus_initialize() again. */
            spi_bus_free(EPD_HOST);
            return -1;
        }
        s_bus_up = true;
    }

    /* --- panel wake: rail already on and settled above; reset pulse,
     * SWRESET, window/waveform setup. Hardware-proven timing, re-run on
     * EVERY init because the panel lost all of it to the rail cut in
     * epd_sleep(). --- */
    if (!reset_pulse()) {
        ESP_LOGE(TAG, "epd_init: BUSY never cleared after hardware reset");
        return -1;
    }
    cmd(0x12);                                  /* SWRESET */
    vTaskDelay(pdMS_TO_TICKS(EPD_SWRESET_GRACE_MS));  /* BUSY needs a moment to rise; polling
                                                         "is it clear yet" too early passes on
                                                         the pre-rise LOW (same race as after a
                                                         trigger) and the next command lands
                                                         mid-reset */
    if (!busy_wait()) {
        ESP_LOGE(TAG, "epd_init: BUSY timeout after SWRESET (0x12)");
        return -1;
    }
    cmd(0x01); data3(0xC7, 0x00, 0x00);         /* driver output: 200 lines */
    cmd(0x11); data1(0x03);                     /* data entry: x+ y+ */
    cmd(0x44); data2(0x00, 0x18);                /* x window: 0..24 (25 bytes) */
    cmd(0x45); data4(0x00, 0x00, 0xC7, 0x00);    /* y window: 0..199 */
    cmd(0x3C); data1(0x05);                      /* border waveform */
    cmd(0x18); data1(0x80);                      /* temp sensor: internal */
    if (!busy_wait()) {
        ESP_LOGE(TAG, "epd_init: BUSY timeout after temp-sensor cmd (0x18)");
        return -1;
    }
    ESP_LOGI(TAG, "init: done, BUSY=%d", gpio_get_level(PIN_BUSY));

    return 0;
}

void epd_full(const uint8_t *fb5000) {
    frame_write(fb5000);
    /* Mirror the same frame into RAM 0x26 (the "previous image" buffer)
     * so the controller's ping-pong base is coherent for subsequent
     * partials; counters must be reset again since frame_write() left
     * them advanced past the window. */
    cmd(0x4E); data1(0x00);
    cmd(0x4F); data2(0x00, 0x00);
    cmd(0x26); dataN(fb5000, EPD_FRAME_BYTES);

    cmd(0x22); data1(0xF7);                      /* OTP full-update sequence */
    ESP_LOGI(TAG, "full: BUSY=%d before trigger", gpio_get_level(PIN_BUSY));
    cmd(0x20);
    busy_wait_after_trigger();
}

void epd_partial(const uint8_t *fb5000) {
    frame_write(fb5000);
    cmd(0x22); data1(0xFF);                      /* OTP mode-2 (ping-pong) partial */
    ESP_LOGI(TAG, "partial: BUSY=%d before trigger", gpio_get_level(PIN_BUSY));
    cmd(0x20);
    busy_wait_after_trigger();
}

void epd_sleep(void) {
    /* BUSY should be low here; a high reading means a caller is about to
     * put the panel to sleep and cut its rail mid-refresh -- the exact
     * failure the wall-clock busy waits above exist to prevent. */
    ESP_LOGI(TAG, "sleep: BUSY=%d at entry", gpio_get_level(PIN_BUSY));
    cmd(0x10); data1(0x01);                      /* deep sleep mode 1 */
    vTaskDelay(pdMS_TO_TICKS(10));                /* let the command latch before power cut */
    board_rail_epd(0);
}
