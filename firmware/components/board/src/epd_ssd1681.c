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
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
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

#define EPD_BUSY_POLL_MS    5
#define EPD_BUSY_TIMEOUT_MS 3000

static const char *TAG = "epd";
static spi_device_handle_t s_spi;

/* BUSY is HIGH while the controller is busy, LOW when idle (confirmed
 * against reference/pala_note's read_busy(), which loops while HIGH). */
static bool busy_wait(void) {
    int waited = 0;
    while (gpio_get_level(PIN_BUSY) == 1) {
        vTaskDelay(pdMS_TO_TICKS(EPD_BUSY_POLL_MS));
        waited += EPD_BUSY_POLL_MS;
        if (waited >= EPD_BUSY_TIMEOUT_MS) {
            ESP_LOGE(TAG, "BUSY timeout");
            return false;
        }
    }
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

static void reset_pulse(void) {
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 1);
    busy_wait();
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
    board_rail_epd(1);
    vTaskDelay(pdMS_TO_TICKS(10));   /* rail settle margin, not datasheet-mandated */

    gpio_config_t dc_rst = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_DC) | (1ULL << PIN_RST),
    };
    gpio_config(&dc_rst);
    gpio_config_t busy_in = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PIN_BUSY),
    };
    gpio_config(&busy_in);

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
        return -1;
    }

    reset_pulse();
    cmd(0x12); busy_wait();                    /* SWRESET */
    cmd(0x01); data3(0xC7, 0x00, 0x00);         /* driver output: 200 lines */
    cmd(0x11); data1(0x03);                     /* data entry: x+ y+ */
    cmd(0x44); data2(0x00, 0x18);                /* x window: 0..24 (25 bytes) */
    cmd(0x45); data4(0x00, 0x00, 0xC7, 0x00);    /* y window: 0..199 */
    cmd(0x3C); data1(0x05);                      /* border waveform */
    cmd(0x18); data1(0x80);                      /* temp sensor: internal */
    if (!busy_wait()) return -1;

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
    cmd(0x20);
    busy_wait();
}

void epd_partial(const uint8_t *fb5000) {
    frame_write(fb5000);
    cmd(0x22); data1(0xFF);                      /* OTP mode-2 (ping-pong) partial */
    cmd(0x20);
    busy_wait();
}

void epd_sleep(void) {
    cmd(0x10); data1(0x01);                      /* deep sleep mode 1 */
    vTaskDelay(pdMS_TO_TICKS(10));                /* let the command latch before power cut */
    board_rail_epd(0);
}
