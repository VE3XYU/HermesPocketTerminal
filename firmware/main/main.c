#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "board.h"
#include "tick_ms.h"
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
 * driven driver, matching the sequence in esp-idf's own console example
 * (examples/system/console/advanced).
 *
 * Output (printf/ESP_LOG, both go through stdout) still goes through the
 * VFS/stdio path, and usb_serial_jtag_vfs_use_driver() is still required
 * for it: stdout's write() ends up at s_ctx.tx_func inside
 * usb_serial_jtag_vfs.c, which this call switches from the raw ROM-FIFO
 * "no_driver" writer to the driver-backed one. That path is confirmed
 * working on real hardware (C3: "prompts print fine").
 *
 * Input is a different story -- see console_getc()/read_line() below.
 * stdin is intentionally left untouched here (no fcntl, no setvbuf): C3
 * on real hardware showed every provisioning prompt running to its full
 * 60s timeout with zero bytes ever registering, even though the operator
 * was typing/pasting into a live session. Root cause: read_line() used
 * to read via fgetc(stdin) with O_NONBLOCK set. usb_serial_jtag_read()
 * (the VFS read function under stdin) returns -1/EWOULDBLOCK on every
 * empty poll by design (see usb_serial_jtag_vfs.c) -- but newlib's stdio
 * layer sets the stream's error/EOF indicator on that first -1 return and
 * does not clear it before the next call. Confirmed by grep: no
 * clearerr(stdin) existed anywhere in this file, so every fgetc() after
 * the very first empty poll short-circuited straight to EOF without the
 * driver ever being touched again -- input dead from the first poll,
 * exactly matching the field symptom (output fine, input never
 * registers). Rather than papering over this with clearerr() and staying
 * exposed to whatever else stdio buffering does over this VFS that can't
 * be debugged remotely, input now bypasses stdio (and the VFS read path)
 * entirely: usb_serial_jtag_read_bytes() is called directly.
 */
static void console_init_usb_serial_jtag(void) {
    fflush(stdout);
    fsync(fileno(stdout));

    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);

    usb_serial_jtag_driver_config_t jtag_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&jtag_config));
    usb_serial_jtag_vfs_use_driver();
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
#define CONSOLE_POLL_MS 20

/* Single-byte pushback slot, standing in for ungetc() now that input
 * bypasses stdio entirely (see console_init_usb_serial_jtag()'s comment).
 * Only ever holds at most one byte: read_line()'s CRLF handling pushes
 * back a byte it peeked and didn't want, and the very next console_getc()
 * call drains it before touching the driver again. -1 = empty. */
static int s_pushed_back = -1;

/* ms_to_ticks_min1() (pdMS_TO_TICKS() with a one-tick floor, so a "wait a
 * few ms" request can't truncate to "don't wait at all" at
 * CONFIG_FREERTOS_HZ=100) now lives in board/tick_ms.h -- the device side
 * has several consumers of it and one definition beats four copies. */

/* Reads one byte straight from the usb_serial_jtag driver (already
 * installed by console_init_usb_serial_jtag()), bypassing stdio and the
 * VFS read path entirely. usb_serial_jtag_read_bytes() blocks up to
 * timeout_ms waiting for a byte and returns 0 (not negative, not EOF) on
 * a timeout with nothing received -- no error/EOF indicator to latch, so
 * every call after a timeout tries again exactly as fresh as the first.
 * A previously pushed-back byte (console_ungetc()) is returned first,
 * without touching the driver. Returns 0-255, or -1 on timeout. */
static int console_getc(unsigned timeout_ms) {
    if (s_pushed_back >= 0) {
        int c = s_pushed_back;
        s_pushed_back = -1;
        return c;
    }
    uint8_t ch;
    int n = usb_serial_jtag_read_bytes(&ch, 1, ms_to_ticks_min1(timeout_ms));
    return (n > 0) ? (int)ch : -1;
}

static void console_ungetc(int c) {
    s_pushed_back = c;
}

/* Reads one line into buf (NUL-terminated; cap includes the NUL) via
 * console_getc(). Each poll already blocks up to CONSOLE_POLL_MS inside
 * the driver (no separate vTaskDelay needed); the deadline is checked
 * only when a poll comes back empty. Bytes beyond cap-1 are silently
 * dropped (but still consumed, so the stream stays in sync) rather than
 * overflowing the caller's buffer.
 *
 * Line endings: both '\n' and '\r' terminate a line. This matters because
 * miniterm's default Enter key sends a bare '\r' with no '\n' at all --
 * treating only '\n' as a terminator means every prompt times out under
 * miniterm. On '\r', a brief (CRLF_PEEK_MS, rounded up to one FreeRTOS
 * tick -- 10ms at this project's 100Hz tick rate, see ms_to_ticks_min1())
 * peek swallows exactly one immediately-following '\n' so a real "\r\n"
 * pair still consumes both bytes; a lone '\r' (nothing follows within the
 * peek window) terminates the line on its own. A lone '\n' (Unix-style
 * input) terminates immediately, no peek needed.
 *
 * Returns the line length (>= 0) once a line end is seen, or -1 if
 * timeout_ms elapses with no line completed (a partial line already
 * typed is discarded). */
static int read_line(char *buf, size_t cap, unsigned timeout_ms) {
    size_t n = 0;
    int64_t deadline_ms = (esp_timer_get_time() / 1000) + timeout_ms;
    while (1) {
        int c = console_getc(CONSOLE_POLL_MS);
        if (c < 0) {
            if ((esp_timer_get_time() / 1000) >= deadline_ms) return -1;
            continue;   /* console_getc() already waited up to CONSOLE_POLL_MS */
        }
        if (c == '\n' || c == '\r') {
            buf[n] = 0;
            if (c == '\r') {
                int c2 = console_getc(CRLF_PEEK_MS);
                if (c2 != '\n' && c2 >= 0) console_ungetc(c2);   /* not LF: not ours, push back */
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

/* Guards the write-failure format offer below to at most once per boot
 * (independent of provision_format()'s own mount-failure offer above --
 * different trigger, different point in the flow). Without this, a card
 * that reformats but still can't take writes (bad flash, not just a
 * corrupt filesystem) would loop: format "succeeds", provisioning
 * restarts, the very next write fails again, offer FORMAT again... */
static int s_format_offered_for_write_failure = 0;

/* Handles an st->write() failure during provisioning: at most once per
 * boot, offers to erase+rebuild the filesystem (unmount + format +
 * remount); a second write failure after that offer has already been
 * used, or a declined/timed-out prompt, is fatal. Never returns except
 * after a successful reformat -- the caller must then restart the whole
 * paste sequence from config.json (the just-written file, if any, is
 * gone; the card was just wiped). */
static void handle_write_failure(void) {
    idf_write_fail_t f;
    idf_ports_last_write_fail(&f);

    if (s_format_offered_for_write_failure) {
        provisioning_fatal("Card appears unusable — replace the SD card.");
        return;   /* unreachable: provisioning_fatal() never returns */
    }
    s_format_offered_for_write_failure = 1;

    printf("\nWrite failed (%s, errno %d). Type FORMAT to erase and rebuild the filesystem, or SKIP:\n",
           f.step, f.err);
    fflush(stdout);
    char line[32];
    int n = read_line(line, sizeof line, PROVISION_TIMEOUT_MS);
    if (n < 0) provisioning_fatal("write-failure format prompt timed out");
    if (strcmp(line, "FORMAT") != 0) provisioning_fatal("write-failure format declined");

    printf("Formatting SD card...\n");
    fflush(stdout);
    if (board_sd_unmount_and_format() != 0) provisioning_fatal("SD format failed");
    printf("SD card formatted and mounted. Restarting provisioning from config.json...\n");
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
 *
 * *restart is set to 1 when a write failure was recovered by reformatting
 * (handle_write_failure() -- at most once per boot); the caller must then
 * restart the whole paste sequence from config.json, since the card was
 * just wiped. It's 0 on every other outcome, including a genuine 3-attempt
 * exhaustion (which stays fatal at the call site, not a restart).
 *
 * Returns 0 on success, -1 otherwise (check *restart to tell "reformatted,
 * try again from the top" apart from "genuinely out of attempts"). */
static int provision_paste_and_write(port_storage_t *st, const char *path, const char *prompt,
                                      char *scratch, size_t scratch_cap,
                                      int (*is_valid)(const char *json, size_t len),
                                      int *restart) {
    *restart = 0;
    for (int attempt = 1; attempt <= 3; attempt++) {
        printf("\n%s\n", prompt);
        fflush(stdout);
        int n = read_body_until_eof(scratch, scratch_cap, PROVISION_TIMEOUT_MS);
        if (n < 0) provisioning_fatal("provisioning input timed out or exceeded the size cap");
        if (is_valid(scratch, (size_t)n)) {
            if (st->write(st->ctx, path, scratch, (size_t)n) != 0) {
                handle_write_failure();   /* never returns except after a successful reformat */
                *restart = 1;
                return -1;
            }
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

#define DEV_HEARTBEAT_MS    30000               /* heartbeat cadence while staying awake */
#define DEV_IDLE_TIMEOUT_MS (30u * 60u * 1000u) /* ~30 min without input -> deep sleep;
                                                   untuned, dev-iteration convenience */
#define DEV_PWR_HOLD_MS     2000                /* PWR held this long -> deep sleep;
                                                   untuned, chosen > boot-transient bounce */

/* End-of-harness behavior: stay awake with USB-Serial-JTAG alive so the
 * operator can keep iterating (flash, read logs, poke serial), instead of
 * dropping into deep sleep immediately and killing the port. Deep sleep
 * happens only on:
 *   - "sleep" + Enter on the serial console,
 *   - the Power button held ~2 s (then released -- we wait for the release
 *     so board_deep_sleep()'s EXT1 any-low wake doesn't fire the instant
 *     we go down and bounce straight back into a boot),
 *   - DEV_IDLE_TIMEOUT_MS with no serial byte and no button press.
 * All timing is wall-clock (esp_timer), never iteration-counted. Never
 * returns. Wake-cause labeling on the next boot is unchanged: sleep still
 * goes through board_deep_sleep(0), same as before. */
static void dev_stay_awake_then_sleep(void) {
    ESP_LOGI(TAG, "staying awake for development; type 'sleep' + Enter or hold PWR ~2s to deep-sleep now");
    ESP_LOGI(TAG, "auto deep-sleep after %u min idle", (unsigned)(DEV_IDLE_TIMEOUT_MS / 60000u));

    char line[16];
    size_t n = 0;
    int64_t t_last_input = esp_timer_get_time() / 1000;
    int64_t t_next_heartbeat = t_last_input + DEV_HEARTBEAT_MS;
    int64_t t_pwr_down_since = -1;   /* -1 = PWR not currently pressed */

    for (;;) {
        /* console_getc() blocks in the driver up to CONSOLE_POLL_MS, so
         * this loop wakes ~50x/s when idle -- that's also the PWR-button
         * sampling rate. */
        int c = console_getc(CONSOLE_POLL_MS);
        int64_t now = esp_timer_get_time() / 1000;

        if (c >= 0) {
            t_last_input = now;
            if (c == '\n' || c == '\r') {
                line[n] = 0;
                n = 0;
                if (strcmp(line, "sleep") == 0) {
                    ESP_LOGI(TAG, "sleep command received");
                    break;
                }
                if (line[0]) ESP_LOGI(TAG, "unknown command '%s' (commands: sleep)", line);
            } else if (n + 1 < sizeof line) {
                line[n++] = (char)c;
            } else {
                n = 0;   /* overlong line: discard, resync at the next line end */
            }
        }

        if (board_btn_pwr()) {
            t_last_input = now;
            if (t_pwr_down_since < 0) t_pwr_down_since = now;
            else if (now - t_pwr_down_since >= DEV_PWR_HOLD_MS) {
                ESP_LOGI(TAG, "PWR held %d ms -- release the button to deep-sleep", DEV_PWR_HOLD_MS);
                while (board_btn_pwr()) vTaskDelay(pdMS_TO_TICKS(50));
                vTaskDelay(pdMS_TO_TICKS(100));   /* release bounce margin before arming EXT1 */
                break;
            }
        } else {
            t_pwr_down_since = -1;
        }

        if (now >= t_next_heartbeat) {
            ESP_LOGI(TAG, "awake (dev hold), up %lld min; 'sleep' + Enter or hold PWR ~2s to deep-sleep",
                     (long long)(now / 60000));
            t_next_heartbeat = now + DEV_HEARTBEAT_MS;
        }

        if (now - t_last_input >= (int64_t)DEV_IDLE_TIMEOUT_MS) {
            ESP_LOGI(TAG, "idle %u min, auto deep-sleeping", (unsigned)(DEV_IDLE_TIMEOUT_MS / 60000u));
            break;
        }
    }

    ESP_LOGI(TAG, "entering deep sleep (wake: REC or PWR button)");
    board_deep_sleep(0);
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

            /* Loops back to the top (re-prompting config.json, discarding
             * any previous paste) only when a write failure got recovered
             * by reformatting mid-flow -- see provision_paste_and_write()'s
             * *restart out-param and handle_write_failure(). Every other
             * exit from either call below is either success (falls through)
             * or fatal (provisioning_fatal() inside, never returns). */
            for (;;) {
                int restart = 0;

                if (provision_paste_and_write(&st, "/config.json",
                        "Paste config.json, end with a line containing only EOF:",
                        buf, sizeof buf, is_valid_config_json, &restart) != 0) {
                    if (restart) continue;
                    provisioning_fatal("config.json provisioning failed after 3 attempts");
                }

                if (provision_paste_and_write(&st, "/wifi.json",
                        "Paste wifi.json, end with a line containing only EOF:",
                        buf, sizeof buf, is_valid_wifi_json, &restart) != 0) {
                    if (restart) continue;
                    provisioning_fatal("wifi.json provisioning failed after 3 attempts");
                }

                break;
            }

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
    if (epd_init() != 0) {
        /* Stay awake even on a display failure: the serial port (and its
         * diagnostic log lines above) is exactly what the operator needs
         * to keep while debugging the panel. */
        ESP_LOGE(TAG, "epd_init failed");
    } else {
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
    }

    ESP_LOGI(TAG, "C3 done");
    dev_stay_awake_then_sleep();   /* never returns */
}
