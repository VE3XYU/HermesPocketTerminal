/* PCF85063 RTC @ I2C 0x51 on the shared codec bus (board_i2c_bus()).
 *
 * NOTE ON THE PART NUMBER: the plan says "PCF8563", but the board's
 * reference firmware (reference/pala_note/src/app/rtc.*, consulted for the
 * register map and address only) drives a PCF85063: time registers at
 * 0x04 (seconds, bit 7 = OS "oscillator stopped" flag) through 0x0A
 * (years), not the PCF8563's 0x02..0x08. Both parts sit at 0x51, so a
 * PCF8563-mapped driver would happily read the wrong registers and return
 * garbage; this file follows the silicon that is actually on the board.
 *
 * board_rtc_get(): reads seconds..years, rejects a set OS flag or any
 * out-of-range BCD field, converts UTC civil time to epoch seconds via
 * days_from_civil (the inverse of htp_ids' civil_from_days). Returns 0
 * when no valid time is available -- the documented "RTC never set"
 * signal htp_make_capture_id falls back on.
 *
 * board_rtc_set(): writes the civil fields back (writing the seconds
 * register with bit 7 clear also clears the OS flag) and mirrors the same
 * epoch into the system clock via settimeofday(), so idf_ports' epoch_s
 * (which prefers the system clock) picks the correction up immediately
 * instead of on the next boot. */
#include "board.h"
#include "board_priv.h"
#include "esp_log.h"
#include <string.h>
#include <sys/time.h>

#define RTC_ADDR7        0x51
#define RTC_SCL_HZ       300000   /* reference firmware's bus speed for this device */
#define RTC_XFER_TIMEOUT_MS 100

#define REG_SECONDS      0x04     /* bit 7 = OS: oscillator stopped, time invalid */
#define TIME_REG_COUNT   7        /* seconds..years */

/* Anything at or before ~2020-09-13 is "no time available" -- same
 * threshold idf_ports applies to the system clock. */
#define RTC_EPOCH_MIN 1600000000LL

static const char *TAG = "rtc";
static i2c_master_dev_handle_t s_dev;

static i2c_master_dev_handle_t rtc_dev(void) {
    if (s_dev) return s_dev;
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (!bus) return NULL;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = RTC_ADDR7,
        .scl_speed_hz    = RTC_SCL_HZ,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "cannot add PCF85063 (0x%02x) to the bus", RTC_ADDR7);
        s_dev = NULL;
    }
    return s_dev;
}

static int rtc_read(uint8_t reg, uint8_t *buf, size_t len) {
    i2c_master_dev_handle_t dev = rtc_dev();
    if (!dev) return -1;
    return i2c_master_transmit_receive(dev, &reg, 1, buf, len,
                                       RTC_XFER_TIMEOUT_MS) == ESP_OK ? 0 : -1;
}

static int rtc_write(uint8_t reg, const uint8_t *buf, size_t len) {
    i2c_master_dev_handle_t dev = rtc_dev();
    if (!dev) return -1;
    uint8_t out[1 + TIME_REG_COUNT];
    if (len > TIME_REG_COUNT) return -1;
    out[0] = reg;
    memcpy(out + 1, buf, len);
    return i2c_master_transmit(dev, out, len + 1, RTC_XFER_TIMEOUT_MS) == ESP_OK ? 0 : -1;
}

static unsigned bcd_dec(uint8_t v) { return ((v >> 4) * 10u) + (v & 0x0F); }
static uint8_t  dec_bcd(unsigned v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* Civil <-> days since 1970-01-01, both directions (Howard Hinnant's
 * public-domain algorithms, same family as htp_ids' civil_from_days --
 * the brief asks for the inverse to live here). */
static long long days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

static void civil_from_days(long long z, int *y, unsigned *m, unsigned *d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long yr = (long long)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yr + (*m <= 2));
}

long long board_rtc_get(void) {
    uint8_t b[TIME_REG_COUNT];
    if (rtc_read(REG_SECONDS, b, sizeof b) != 0) {
        ESP_LOGW(TAG, "read failed; treating the RTC as unset");
        return 0;
    }
    if (b[0] & 0x80) return 0;   /* OS flag: oscillator stopped since last set */

    unsigned sec   = bcd_dec(b[0] & 0x7F);
    unsigned min   = bcd_dec(b[1] & 0x7F);
    unsigned hour  = bcd_dec(b[2] & 0x3F);
    unsigned day   = bcd_dec(b[3] & 0x3F);
    /* b[4] = weekday: not needed for the epoch */
    unsigned month = bcd_dec(b[5] & 0x1F);
    int      year  = (int)bcd_dec(b[6]) + 2000;   /* chip stores 2 digits */

    if (sec > 59 || min > 59 || hour > 23 ||
        day < 1 || day > 31 || month < 1 || month > 12) return 0;

    long long epoch = days_from_civil(year, month, day) * 86400LL
                      + (long long)hour * 3600 + (long long)min * 60 + sec;
    return epoch > RTC_EPOCH_MIN ? epoch : 0;
}

void board_rtc_set(long long epoch) {
    if (epoch <= RTC_EPOCH_MIN) return;   /* never write an obviously bogus time */

    long long days = epoch / 86400;
    long long rem  = epoch % 86400;
    int y; unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    if (y < 2000 || y > 2099) return;     /* outside the chip's 2-digit year */

    uint8_t b[TIME_REG_COUNT];
    b[0] = dec_bcd((unsigned)(rem % 60));            /* bit 7 clear: clears OS */
    b[1] = dec_bcd((unsigned)((rem % 3600) / 60));
    b[2] = dec_bcd((unsigned)(rem / 3600));
    b[3] = dec_bcd(d);
    b[4] = dec_bcd((unsigned)((days + 4) % 7));      /* 1970-01-01 was a Thursday */
    b[5] = dec_bcd(m);
    b[6] = dec_bcd((unsigned)(y - 2000));
    if (rtc_write(REG_SECONDS, b, sizeof b) != 0) {
        ESP_LOGE(TAG, "write failed; chip keeps its old time");
        return;
    }

    struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "set to %lld (%04d-%02u-%02u %02u:%02u UTC)",
             epoch, y, m, d, (unsigned)(rem / 3600), (unsigned)((rem % 3600) / 60));
}
