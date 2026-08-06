#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "board.h"
#include "ui_fb.h"
#include "ports.h"
#include "idf_ports.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

static const char *TAG = "htp";

static int log_rec_entry(const char *name, void *u) {
    int *count = u;
    ESP_LOGI(TAG, "  /rec/%s", name);
    (*count)++;
    return 0;   /* keep enumerating */
}

/* ============================================================
 * Task 14 serial provisioning (C3 hardware reality: the operator's SD
 * card reads RAW on Windows and their card-reader route is unreliable).
 * Entered from app_main() only on one of three triggers -- SD mount
 * failure, a missing/invalid /config.json, or the Power button held at
 * boot -- never on the "card mounts fine, config parses" happy path.
 * ============================================================ */

#define PROVISION_TIMEOUT_MS 60000

/* USB-Serial-JTAG is already the primary console (CONFIG_ESP_CONSOLE_
 * USB_SERIAL_JTAG=y, sdkconfig.defaults) by the time app_main() runs --
 * ESP-IDF's startup code registers /dev/usbserjtag and remaps stdin/
 * stdout onto it before app_main() is called. That default path uses
 * the "no_driver" simple functions: non-blocking, ROM-FIFO-only RX with
 * no interrupt/ring-buffer backing. This installs the real interrupt-
 * driven driver and switches the VFS onto it, matching the sequence in
 * esp-idf's own console example (examples/system/console/advanced).
 *
 * Unlike that example, stdin is left non-blocking (O_NONBLOCK) rather
 * than switched to blocking: read_line() below needs to enforce a
 * timeout, and with the driver active, a blocking fgetc() would block
 * forever inside usb_serial_jtag_read_bytes(..., portMAX_DELAY) with no
 * way to time out. Non-blocking fgetc() returns EOF immediately when no
 * byte is buffered yet, which read_line()'s poll loop treats as "nothing
 * yet" rather than "stream closed".
 *
 * RX line-ending conversion is left at ESP_LINE_ENDINGS_LF (no
 * conversion) rather than the example's ESP_LINE_ENDINGS_CR: read_line()
 * (below) does its own line-ending handling -- both '\n' and bare '\r'
 * terminate a line, with '\r' peeking for and swallowing a following
 * '\n' -- so it needs raw, unconverted bytes to work with.
 */
static void console_init_usb_serial_jtag(void) {
    fflush(stdout);
    fsync(fileno(stdout));

    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);

    usb_serial_jtag_driver_config_t jtag_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&jtag_config));
    usb_serial_jtag_vfs_use_driver();

    fcntl(fileno(stdin), F_SETFL, O_NONBLOCK);
    setvbuf(stdin, NULL, _IONBF, 0);
}

/* Provisioning has no e-paper fallback screen (the operator is looking at
 * the serial console they're typing FORMAT/paste input into, not the
 * panel) -- "print an error and stop" IS the fatal screen here. */
static void provisioning_fatal(const char *msg) {
    ESP_LOGE(TAG, "%s", msg);
    printf("\n*** %s ***\nHalting.\n", msg);
    fflush(stdout);
    board_deep_sleep(0);
}

#define CRLF_PEEK_MS 5

/* Reads one line from stdin into buf (NUL-terminated; cap includes the
 * NUL). stdin is non-blocking (console_init_usb_serial_jtag()), so this
 * polls fgetc() and checks the deadline only when nothing is available
 * yet -- bytes that do arrive are consumed immediately, back-to-back,
 * with no artificial per-byte delay. Bytes beyond cap-1 are silently
 * dropped (but still consumed, so the stream stays in sync) rather than
 * overflowing the caller's buffer.
 *
 * Line endings: both '\n' and '\r' terminate a line. This matters because
 * miniterm's default Enter key sends a bare '\r' with no '\n' at all --
 * treating only '\n' as a terminator (as an earlier version of this
 * function did) means every prompt would silently time out under
 * miniterm. On '\r', a brief (CRLF_PEEK_MS) non-blocking peek swallows
 * exactly one immediately-following '\n' so a real "\r\n" pair still
 * consumes both bytes and behaves identically to before; a lone '\r'
 * (nothing follows within the peek window) terminates the line on its
 * own. A lone '\n' (Unix-style input) terminates immediately, no peek
 * needed -- this path, and CRLF's net effect, are unchanged from before.
 *
 * Returns the line length (>= 0) once a line end is seen, or -1 if
 * timeout_ms elapses with no line completed (a partial line already
 * typed is discarded). */
static int read_line(char *buf, size_t cap, unsigned timeout_ms) {
    size_t n = 0;
    int64_t deadline_ms = (esp_timer_get_time() / 1000) + timeout_ms;
    while (1) {
        int c = fgetc(stdin);
        if (c == EOF) {
            if ((esp_timer_get_time() / 1000) >= deadline_ms) return -1;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\n' || c == '\r') {
            buf[n] = 0;
            if (c == '\r') {
                int64_t peek_deadline_ms = (esp_timer_get_time() / 1000) + CRLF_PEEK_MS;
                while (1) {
                    int c2 = fgetc(stdin);
                    if (c2 == '\n') break;                        /* "\r\n": both consumed */
                    if (c2 != EOF) { ungetc(c2, stdin); break; }   /* bare '\r': push back, done */
                    if ((esp_timer_get_time() / 1000) >= peek_deadline_ms) break;  /* bare '\r', nothing followed */
                }
            }
            return (int)n;
        }
        if (n + 1 < cap) buf[n++] = (char)c;   /* else: drop, but keep reading for line end */
    }
}

/* Reads lines directly into `out` (no separate scratch buffer -- a
 * single-line minified-JSON paste can legitimately use the whole `cap`
 * budget) until a line that is exactly "EOF", concatenating with '\n'
 * separators, until "EOF" or a fresh timeout_ms-bounded read_line() call
 * times out. Returns the accumulated length (>= 0, "EOF" line itself
 * excluded, NUL-terminated at that length) on success, or -1 on timeout
 * or if the body would exceed `cap`. */
static int read_body_until_eof(char *out, size_t cap, unsigned timeout_ms) {
    size_t len = 0;
    while (1) {
        if (len >= cap) return -1;
        int n = read_line(out + len, cap - len, timeout_ms);
        if (n < 0) return -1;
        if (n == 3 && !memcmp(out + len, "EOF", 3)) { out[len] = 0; return (int)len; }
        len += (size_t)n;
        if (len + 1 >= cap) return -1;   /* no room for the '\n' separator + NUL */
        out[len++] = '\n';
    }
}

/* Only called when board_sd_mount() has already failed. Never returns
 * except on success (format + remount both succeeded): SKIP or a timeout
 * means "operator declined or wasn't there", and per the brief that's
 * fatal, same as an unrecoverable mount failure. */
static void provision_format(void) {
    printf("\nSD unreadable. Type FORMAT to erase and format the card, or SKIP:\n");
    fflush(stdout);
    char line[32];
    int n = read_line(line, sizeof line, PROVISION_TIMEOUT_MS);
    if (n < 0) provisioning_fatal("SD format prompt timed out");
    if (strcmp(line, "FORMAT") != 0) provisioning_fatal("SD format declined");
    printf("Formatting SD card...\n");
    fflush(stdout);
    if (board_sd_format_and_mount() != 0) provisioning_fatal("SD format failed");
    printf("SD card formatted and mounted.\n");
}

static int is_valid_config_json(const char *json, size_t len) {
    app_config_t tmp;
    return app_config_parse(json, len, &tmp) == 0;
}

static int is_valid_wifi_json(const char *json, size_t len) {
    wifi_profiles_t tmp;
    return wifi_profiles_parse(json, len, &tmp) == 0;
}

/* Prompts (exact text from `prompt`), accumulates a pasted JSON body,
 * validates it with `is_valid` BEFORE writing anything to the card, and
 * writes it via the storage port (atomic tmp+rename) only once validated.
 * Retries up to 3 attempts on a parse failure; a read timeout/overflow is
 * escalated immediately (not counted against the 3 attempts -- that's a
 * "nobody's there" failure, not a "pasted something wrong" one).
 * Returns 0 on success, -1 once 3 parse attempts are exhausted. */
static int provision_paste_and_write(port_storage_t *st, const char *path, const char *prompt,
                                      char *scratch, size_t scratch_cap,
                                      int (*is_valid)(const char *json, size_t len)) {
    for (int attempt = 1; attempt <= 3; attempt++) {
        printf("\n%s\n", prompt);
        fflush(stdout);
        int n = read_body_until_eof(scratch, scratch_cap, PROVISION_TIMEOUT_MS);
        if (n < 0) provisioning_fatal("provisioning input timed out or exceeded the size cap");
        if (is_valid(scratch, (size_t)n)) {
            if (st->write(st->ctx, path, scratch, (size_t)n) != 0)
                provisioning_fatal("failed to write provisioned file to SD card");
            printf("%s written and validated.\n", path);
            return 0;
        }
        printf("Could not parse %s as valid JSON (attempt %d/3).\n", path, attempt);
    }
    return -1;
}

#define PWR_HOLD_DEBOUNCE_MS 1500
#define PWR_HOLD_POLL_MS 20

/* "Power button held at boot" forces re-provisioning even when the SD
 * card and config are otherwise fine -- so it needs real debounce, not a
 * single instantaneous GPIO sample. Two guards:
 *   - wake-cause gate: only WAKE_COLD (fresh power-on) or WAKE_PWR_BUTTON
 *     (woke specifically because of this button) are eligible. A timer
 *     wake or a rec-button wake can never trigger this, no matter what
 *     the Power GPIO happens to read at that instant.
 *   - continuous-press debounce: the button must read pressed on every
 *     20ms poll for a full 1500ms. On a latch-on-press power circuit, a
 *     normal power-on can leave the button transiently/momentarily
 *     pressed (that's how the board powers on at all); requiring it to
 *     *stay* pressed for 1.5s of continuous re-sampling is what tells
 *     that apart from an operator deliberately holding it down.
 * Returns as soon as the button releases early (not held) or once the
 * debounce window is satisfied (held). */
static int power_button_held_at_boot(wake_cause_t wc) {
    if (wc != WAKE_COLD && wc != WAKE_PWR_BUTTON) return 0;
    int elapsed_ms = 0;
    while (board_btn_pwr()) {
        elapsed_ms += PWR_HOLD_POLL_MS;
        if (elapsed_ms >= PWR_HOLD_DEBOUNCE_MS) return 1;
        vTaskDelay(pdMS_TO_TICKS(PWR_HOLD_POLL_MS));
    }
    return 0;   /* released before the debounce window elapsed */
}

void app_main(void) {
    board_early_init();
    console_init_usb_serial_jtag();

    wake_cause_t wc = board_wake_cause();
    const char *cause[] = { "cold", "rec-button", "pwr-button", "timer" };
    ESP_LOGI(TAG, "HTP terminal bring-up C1, wake=%s", cause[wc]);

    ESP_LOGI(TAG, "C3 SD/NVS/config test");

    port_storage_t st; port_kv_t kv; port_clock_t ck; port_rng_t rng;
    idf_ports_init(&st, &kv, &ck, &rng);

    int mounted = (board_sd_mount() == 0);
    int pwr_held = power_button_held_at_boot(wc);

    char buf[2048]; size_t len;
    app_config_t cfg;
    int config_ok = mounted &&
                     st.read(st.ctx, "/config.json", buf, sizeof buf, &len) == 0 &&
                     app_config_parse(buf, len, &cfg) == 0;

    if (!mounted || !config_ok || pwr_held) {
        ESP_LOGW(TAG, "entering serial provisioning (mounted=%d config_ok=%d pwr_held=%d)",
                 mounted, config_ok, pwr_held);
        printf("\n=== HTP serial provisioning ===\n");

        /* Escape hatch: only offered when Power-held is the *sole* reason
         * we're here (card mounts, config already parses) -- if the card
         * genuinely needs fixing, "boot normally" isn't a safe option
         * regardless of what's typed here, so we skip straight past this
         * prompt into the mandatory format/paste flow below. */
        int proceed = 1;
        if (mounted && config_ok && pwr_held) {
            printf("\nRe-provision requested. Type YES to continue, anything else boots normally:\n");
            fflush(stdout);
            char line[8];
            int n = read_line(line, sizeof line, PROVISION_TIMEOUT_MS);
            proceed = (n >= 0 && strcmp(line, "YES") == 0);
            if (!proceed) { printf("Booting normally.\n\n"); fflush(stdout); }
        }

        if (proceed) {
            if (!mounted) {
                provision_format();   /* fatal on decline/timeout/failure; SD is mounted on return */
                mounted = 1;
            }

            if (provision_paste_and_write(&st, "/config.json",
                    "Paste config.json, end with a line containing only EOF:",
                    buf, sizeof buf, is_valid_config_json) != 0)
                provisioning_fatal("config.json provisioning failed after 3 attempts");

            if (provision_paste_and_write(&st, "/wifi.json",
                    "Paste wifi.json, end with a line containing only EOF:",
                    buf, sizeof buf, is_valid_wifi_json) != 0)
                provisioning_fatal("wifi.json provisioning failed after 3 attempts");

            printf("\nProvisioning complete.\n\n");
            fflush(stdout);
        }
    }

    /* ---- existing C3 checks (unchanged) ---- */
    if (st.read(st.ctx, "/config.json", buf, sizeof buf, &len) != 0 ||
        app_config_parse(buf, len, &cfg) != 0) {
        ESP_LOGE(TAG, "config.json missing or invalid"); board_deep_sleep(0);
    }
    ESP_LOGI(TAG, "bridge=%s token=%.4s...(%d) sync=%d",
             cfg.bridge_url, cfg.token, (int)strlen(cfg.token), cfg.sync_interval_s);

    wifi_profiles_t wp;
    int have_wifi = (st.read(st.ctx, "/wifi.json", buf, sizeof buf, &len) == 0 &&
                      wifi_profiles_parse(buf, len, &wp) == 0);
    if (have_wifi)
        ESP_LOGI(TAG, "wifi profiles: %d (first: %s)", wp.count, wp.nets[0].ssid);
    else
        ESP_LOGE(TAG, "wifi.json missing or invalid");

    long long free_bytes = st.free_bytes(st.ctx);
    ESP_LOGI(TAG, "sd free: %lld bytes", free_bytes);

    int rec_files = 0;
    ESP_LOGI(TAG, "/rec contents:");
    st.list(st.ctx, "/rec", log_rec_entry, &rec_files);
    ESP_LOGI(TAG, "/rec entries: %d", rec_files);

    kv.set(kv.ctx, "c3", "ok");
    char v[8];
    int kv_ok = (kv.get(kv.ctx, "c3", v, sizeof v) == 0) && !strcmp(v, "ok");
    ESP_LOGI(TAG, "nvs roundtrip: %s", kv_ok ? v : "FAIL");

    static ui_fb_t fb;
    if (epd_init() != 0) { ESP_LOGE(TAG, "epd_init failed"); board_deep_sleep(0); }
    fb_clear(&fb);
    fb_text(&fb, 10, 15, "Config OK", 2, 1);
    char line[40];
    snprintf(line, sizeof line, "sync=%ds", cfg.sync_interval_s);
    fb_text(&fb, 10, 55, line, 1, 1);
    snprintf(line, sizeof line, "wifi nets=%d", have_wifi ? wp.count : 0);
    fb_text(&fb, 10, 75, line, 1, 1);
    snprintf(line, sizeof line, "sd free=%lldK", free_bytes / 1024);
    fb_text(&fb, 10, 95, line, 1, 1);
    fb_text(&fb, 10, 115, kv_ok ? "nvs=ok" : "nvs=FAIL", 1, 1);
    fb_rect(&fb, 5, 5, 190, 190, 1);
    epd_full(fb.px);
    epd_sleep();

    ESP_LOGI(TAG, "C3 done, sleeping");
    board_deep_sleep(0);
}
