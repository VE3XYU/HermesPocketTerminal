/* Battery percent from the GPIO 4 sense divider (design §2 pin map, §5.7).
 *
 * ADC1 channel 3 is GPIO 4 on the ESP32-S3. ADC1 (not ADC2) matters: ADC2
 * is shared with the Wi-Fi radio and reads fail while the station is up,
 * and every X-Battery header is produced during a network session.
 *
 * The divider ratio and the voltage->percent curve below are the reference
 * hardware's nominal values, NOT measured on this board: a 2:1 divider and
 * a straight line from 3300 mV (empty) to 4200 mV (full). Task 19 replaces
 * the curve with one measured against a real discharge (design §11.3), so
 * treat the number as "plausible", not accurate.
 */
#include "board.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

static const char *TAG = "battery";

#define BATT_UNIT      ADC_UNIT_1
#define BATT_CHANNEL   ADC_CHANNEL_3     /* GPIO 4 */
#define BATT_ATTEN     ADC_ATTEN_DB_12   /* full scale ~3.1 V at the pin */
#define BATT_BITWIDTH  ADC_BITWIDTH_12
#define BATT_SAMPLES   8
#define BATT_DIVIDER   2                 /* 2:1 sense divider on VBAT */
#define BATT_MV_EMPTY  3300
#define BATT_MV_FULL   4200

static adc_oneshot_unit_handle_t s_unit;
static adc_cali_handle_t s_cali;         /* NULL when eFuse calibration is unavailable */
static int s_ready;

/* Lazy one-time setup: the ADC stays configured for the life of the boot
 * (battery percent is read once per request, not once per session). */
static int battery_setup(void) {
    if (s_ready) return 0;

    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = BATT_UNIT };
    esp_err_t e = adc_oneshot_new_unit(&unit_cfg, &s_unit);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit: %s", esp_err_to_name(e));
        return -1;
    }
    adc_oneshot_chan_cfg_t chan_cfg = { .atten = BATT_ATTEN, .bitwidth = BATT_BITWIDTH };
    if ((e = adc_oneshot_config_channel(s_unit, BATT_CHANNEL, &chan_cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel: %s", esp_err_to_name(e));
        adc_oneshot_del_unit(s_unit);
        s_unit = NULL;
        return -1;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BATT_UNIT, .chan = BATT_CHANNEL,
        .atten = BATT_ATTEN, .bitwidth = BATT_BITWIDTH,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        /* Calibration eFuses unburnt: fall back to the nominal full-scale
         * conversion below rather than refusing to report a battery level. */
        ESP_LOGW(TAG, "no eFuse ADC calibration; using the nominal curve");
        s_cali = NULL;
    }
#endif

    s_ready = 1;
    return 0;
}

/* Nominal conversion for the uncalibrated case: ADC_ATTEN_DB_12 puts full
 * scale at about 3100 mV. */
#define BATT_NOMINAL_FULL_SCALE_MV 3100
#define BATT_RAW_MAX 4095   /* 12-bit */

int board_battery_pct(void) {
    if (battery_setup() != 0) return -1;

    int sum = 0, taken = 0;
    for (int i = 0; i < BATT_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_unit, BATT_CHANNEL, &raw) != ESP_OK) continue;
        sum += raw;
        taken++;
    }
    if (taken == 0) {
        ESP_LOGE(TAG, "no ADC samples");
        return -1;
    }
    int raw_avg = sum / taken;

    int pin_mv;
    if (!s_cali || adc_cali_raw_to_voltage(s_cali, raw_avg, &pin_mv) != ESP_OK)
        pin_mv = raw_avg * BATT_NOMINAL_FULL_SCALE_MV / BATT_RAW_MAX;

    int batt_mv = pin_mv * BATT_DIVIDER;
    int pct = (batt_mv - BATT_MV_EMPTY) * 100 / (BATT_MV_FULL - BATT_MV_EMPTY);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    ESP_LOGD(TAG, "raw=%d pin=%dmV batt=%dmV pct=%d", raw_avg, pin_mv, batt_mv, pct);
    return pct;
}
