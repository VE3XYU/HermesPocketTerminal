/* Shared I2C master bus for the codec (ES8311 @ 0x18/0x19) and the RTC
 * (PCF85063 @ 0x51). One bus, created once, never deleted -- see
 * board_priv.h for the ownership and rail policy. Pin map and init order
 * confirmed against the reference firmware (I2C_NUM_0, SDA 47, SCL 48,
 * internal pullups, codec rail gated on before first use). */
#include "board.h"
#include "board_priv.h"
#include "tick_ms.h"
#include "esp_log.h"

#define PIN_I2C_SDA 47
#define PIN_I2C_SCL 48
#define BUS_PORT I2C_NUM_0

/* Settle for *bus integrity* only (the just-powered codec must stop loading
 * SDA/SCL); the codec's own LDO settle before register access is
 * audio_init()'s longer AUDIO_RAIL_SETTLE_MS, applied on top of this. */
#define BUS_RAIL_SETTLE_MS 10

static const char *TAG = "i2c_bus";
static i2c_master_bus_handle_t s_bus;

i2c_master_bus_handle_t board_i2c_bus(void) {
    if (board_rail_audio_on_ms() < 0) board_rail_audio(1);
    int since = board_rail_audio_on_ms();
    if (since >= 0 && since < BUS_RAIL_SETTLE_MS)
        board_delay_ms((unsigned)(BUS_RAIL_SETTLE_MS - since));

    if (s_bus) return s_bus;

    i2c_master_bus_config_t cfg = {
        .i2c_port          = BUS_PORT,
        .sda_io_num        = PIN_I2C_SDA,
        .scl_io_num        = PIN_I2C_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags             = { .enable_internal_pullup = true },
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        s_bus = NULL;
    }
    return s_bus;
}
