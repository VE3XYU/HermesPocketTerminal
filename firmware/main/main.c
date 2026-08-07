/* HTP terminal firmware — wake dispatch + full capture session (Task 17).
 *
 * app_main() dispatches on the wake cause (design §5.1):
 *   REC button  -> capture_session()      hold-to-talk, upload, reply, sync
 *   PWR button  -> ui_session_stub()      sync-only until Task 18
 *   timer/cold  -> sync_session_stub()    mount, config, Wi-Fi, sync_cycle
 *
 * Task 14's serial-provisioning boot path survives inside the sync/UI
 * sessions: SD mount failure, an unparseable /config.json, or the Power
 * button held through boot (with the YES escape) all still drop into the
 * console provisioning flow before anything else runs.
 *
 * Capture fast path (design §5.2): everything before audio_record_to() is
 * capture-critical only. The audio rail is gated on first so its settle
 * overlaps the SD mount; the Wi-Fi join kickoff and the recording glyph
 * are pushed to a short-lived background task so the first I2S read isn't
 * delayed by esp_wifi bring-up (~100 ms) or an e-paper refresh (a partial
 * blocks 300-500 ms, a first full ~2 s -- either would blow the 250 ms
 * record-start target and the DMA ring only covers ~256 ms).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "board.h"
#include "tick_ms.h"
#include "ui_fb.h"
#include "ui_widgets.h"
#include "ui_flow.h"
#include "ports.h"
#include "idf_ports.h"
#include "idf_wifi.h"
#include "idf_transport.h"
#include "app_config.h"
#include "htp_client.h"
#include "htp_ids.h"
#include "capture_flow.h"
#include "sidecar.h"
#include "rec_index.h"
#include "sync.h"
#include "gesture.h"
#include "util.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

static const char *TAG = "htp";

/* ============================================================
 * Development-linger switch.
 *
 * 1 (development, the default): after a session completes, the device
 * stays awake with the USB-Serial-JTAG console alive -- heartbeat,
 * 'sleep' + Enter, PWR held ~2 s, or DEV_IDLE_TIMEOUT_MS of idleness
 * drop it into the real deep sleep, with the sync timer armed, so the
 * full sleep/wake cycle stays verifiable (C6).
 *
 * 0 (release, Task 19 flips this): every session ends directly in
 * board_deep_sleep(next_sync_interval()), no console hold.
 * ============================================================ */
#define HTP_DEV_LINGER 1

/* ============================================================
 * Session state. One main task runs everything sequentially, and the
 * large objects are static (not stack) per the project ruling that big
 * buffers live in BSS -- the stack budget in sdkconfig.defaults is sized
 * for call frames, not for multi-KB models.
 * ============================================================ */
static port_storage_t s_st;
static port_kv_t      s_kv;
static port_clock_t   s_ck;
static port_rng_t     s_rng;
static app_config_t   s_cfg;
static wifi_profiles_t s_wp;
static int s_have_cfg, s_have_wifi, s_wifi_ok;
static htp_transport_t s_tr;
static htp_client_t    s_cl;
static ui_fb_t   s_fb;
static ui_flow_t s_uif;
static int s_epd_up;       /* epd_init() has run and the panel is awake */
static int s_base_drawn;   /* a full refresh established a partial base */
static int s_panel_lost;   /* a hung background task may still own fb + EPD SPI */
static char s_json[2048];  /* config/wifi read + provisioning paste scratch */
static sidecar_t s_sc, s_sc_next;

#define REPLY_LOGICAL "/reply.tmp.wav"
#define REPLY_FS      "/sdcard" REPLY_LOGICAL

#define CAPTURE_WIFI_WAIT_MS 8000    /* post-recording wait; a stale fast join can
                                        roughly double this via the scan fallback */
#define SYNC_WIFI_TIMEOUT_MS 20000   /* sync sessions aren't latency-critical */
#define MAX_RECORD_MS        120000  /* hard cap on one recording (design §5.2) */
#define MIN_WAV_BYTES        8000    /* < 250 ms of 16 kHz mono: discard */
#define FOLLOW_UP_WINDOW_MS  30000   /* hold-to-talk window after a reply */

/* ============================================================
 * Task 14 serial provisioning (C3 hardware reality: the operator's SD
 * card reads RAW on Windows and their card-reader route is unreliable).
 * Entered from the sync/UI sessions only on one of three triggers -- SD
 * mount failure, a missing/invalid /config.json, or the Power button held
 * at boot -- never on the "card mounts fine, config parses" happy path.
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

/* Task 14's provisioning driver, verbatim behavior: the YES escape is
 * offered only when Power-held is the *sole* trigger (card mounts, config
 * parses); otherwise the format/paste flow is mandatory. May never return
 * (provisioning_fatal). On return the card is mounted and any requested
 * provisioning finished. */
static void maybe_provision(int mounted, int config_ok, int pwr_held) {
    ESP_LOGW(TAG, "entering serial provisioning (mounted=%d config_ok=%d pwr_held=%d)",
             mounted, config_ok, pwr_held);
    printf("\n=== HTP serial provisioning ===\n");

    int proceed = 1;
    if (mounted && config_ok && pwr_held) {
        printf("\nRe-provision requested. Type YES to continue, anything else boots normally:\n");
        fflush(stdout);
        char line[8];
        int n = read_line(line, sizeof line, PROVISION_TIMEOUT_MS);
        proceed = (n >= 0 && strcmp(line, "YES") == 0);
        if (!proceed) { printf("Booting normally.\n\n"); fflush(stdout); }
    }
    if (!proceed) return;

    if (!mounted)
        provision_format();   /* fatal on decline/timeout/failure; SD is mounted on return */

    /* Loops back to the top (re-prompting config.json, discarding any
     * previous paste) only when a write failure got recovered by
     * reformatting mid-flow -- see provision_paste_and_write()'s *restart
     * out-param and handle_write_failure(). Every other exit from either
     * call below is either success (falls through) or fatal
     * (provisioning_fatal() inside, never returns). */
    for (;;) {
        int restart = 0;

        if (provision_paste_and_write(&s_st, "/config.json",
                "Paste config.json, end with a line containing only EOF:",
                s_json, sizeof s_json, is_valid_config_json, &restart) != 0) {
            if (restart) continue;
            provisioning_fatal("config.json provisioning failed after 3 attempts");
        }

        if (provision_paste_and_write(&s_st, "/wifi.json",
                "Paste wifi.json, end with a line containing only EOF:",
                s_json, sizeof s_json, is_valid_wifi_json, &restart) != 0) {
            if (restart) continue;
            provisioning_fatal("wifi.json provisioning failed after 3 attempts");
        }

        break;
    }

    printf("\nProvisioning complete.\n\n");
    fflush(stdout);
}

/* ============================================================
 * Screens. One shared framebuffer, one refresh-discipline gate: the
 * first draw after epd_init() is always a FULL refresh (it also seeds
 * the controller's previous-image RAM, without which a partial diffs
 * against garbage), everything after that is partial unless the caller
 * asks otherwise.
 * ============================================================ */

static int screen_ready(void) {
    /* Single gate for the whole display path: every screen and render
     * function funnels through here, so once a background task has missed
     * its join -- it may still own the framebuffer and the EPD SPI bus --
     * one flag stops all of them for the rest of the session (see
     * capture_bg_join). */
    if (s_panel_lost) return -1;
    if (!s_epd_up) {
        if (epd_init() != 0) return -1;
        s_epd_up = 1;
        s_base_drawn = 0;
    }
    return 0;
}

static void present(int want_full) {
    if (!s_base_drawn || want_full) {
        epd_full(s_fb.px);
        s_base_drawn = 1;
    } else {
        epd_partial(s_fb.px);
    }
}

static void status_line_fill(ui_status_t *stt) {
    memset(stt, 0, sizeof *stt);
    stt->battery_pct = board_battery_pct();
    stt->wifi_ok = s_wifi_ok;
    /* pending_uploads and the clock stay blank until Task 18's UI pass */
}

/* Word-wraps msg into the shared framebuffer at `cols` chars/line. */
static void draw_wrapped(const char *msg, int x, int y0, int dy, int scale,
                         int cols, int max_lines) {
    char line[32];
    if (cols > (int)sizeof line - 1) cols = (int)sizeof line - 1;
    int y = y0, lines = 0;
    const char *p = msg;
    while (*p && lines < max_lines) {
        while (*p == ' ') p++;
        if (!*p) break;
        int take = (int)strlen(p);
        if (take > cols) {
            int k = cols;
            while (k > 0 && p[k] != ' ') k--;   /* break at the last space that fits */
            take = (k > 0) ? k : cols;          /* overlong word: hard split */
        }
        memcpy(line, p, (size_t)take);
        line[take] = 0;
        fb_text(&s_fb, x, y, line, scale, 1);
        p += take;
        y += dy;
        lines++;
    }
}

/* Status line + big scale-2 message (12 cols/line), partial refresh. */
static void screen_status(const char *msg) {
    ESP_LOGI(TAG, "status: %s", msg);
    if (screen_ready() != 0) return;
    ui_status_t stt;
    status_line_fill(&stt);
    fb_clear(&s_fb);
    widget_status_line(&s_fb, &stt);
    draw_wrapped(msg, 4, 48, 20, 2, 12, 6);
    present(0);
}

/* capture_ctx_t.on_status adapter */
static void screen_status_cb(void *ui_ctx, const char *line) {
    (void)ui_ctx;
    screen_status(line);
}

/* Big status word + the opening of the transcript below it. */
static void screen_status_transcript(const char *status, const char *transcript) {
    ESP_LOGI(TAG, "status: %s transcript=%.80s", status, transcript);
    if (screen_ready() != 0) return;
    ui_status_t stt;
    status_line_fill(&stt);
    fb_clear(&s_fb);
    widget_status_line(&s_fb, &stt);
    fb_text(&s_fb, 4, 22, status, 2, 1);
    draw_wrapped(transcript[0] ? transcript : "(no transcript)", 2, 48, 12, 1, 24, 12);
    present(0);
}

/* Terminal screen for a session that cannot continue: full refresh, then
 * the panel is put to sleep immediately (the image persists without
 * power) so the common teardown has nothing left to do. */
static void screen_fatal(const char *msg) {
    ESP_LOGE(TAG, "fatal: %s", msg);
    if (screen_ready() != 0) return;
    ui_status_t stt;
    status_line_fill(&stt);
    fb_clear(&s_fb);
    widget_status_line(&s_fb, &stt);
    draw_wrapped(msg, 4, 48, 20, 2, 12, 6);
    present(1);
    epd_sleep();
    s_epd_up = 0;
    s_base_drawn = 0;
}

/* ============================================================
 * Config / boot bookkeeping
 * ============================================================ */

static int load_config(void) {
    size_t len;
    if (s_st.read(s_st.ctx, "/config.json", s_json, sizeof s_json, &len) != 0 ||
        app_config_parse(s_json, len, &s_cfg) != 0) {
        s_have_cfg = 0;
        ESP_LOGE(TAG, "config.json missing or invalid");
        return -1;
    }
    s_have_cfg = 1;
    ESP_LOGI(TAG, "bridge=%s token=%.4s...(%d) sync=%d",
             s_cfg.bridge_url, s_cfg.token, (int)strlen(s_cfg.token), s_cfg.sync_interval_s);
    return 0;
}

static int load_wifi(void) {
    size_t len;
    if (s_st.read(s_st.ctx, "/wifi.json", s_json, sizeof s_json, &len) != 0 ||
        wifi_profiles_parse(s_json, len, &s_wp) != 0) {
        s_have_wifi = 0;
        ESP_LOGE(TAG, "wifi.json missing or invalid");
        return -1;
    }
    s_have_wifi = 1;
    ESP_LOGI(TAG, "wifi profiles: %d (first: %s)", s_wp.count, s_wp.nets[0].ssid);
    return 0;
}

/* Monotone boot counter in NVS; feeds htp_make_capture_id's clockless
 * fallback ("c-b<bootcount>-<mono_ms>-xxxx"). */
static uint32_t boot_count_bump(void) {
    char v[16];
    unsigned long n = 0;
    if (s_kv.get(s_kv.ctx, "bootcnt", v, sizeof v) == 0) n = strtoul(v, NULL, 10);
    n++;
    snprintf(v, sizeof v, "%lu", n);
    s_kv.set(s_kv.ctx, "bootcnt", v);
    return (uint32_t)n;
}

/* kv "sync_s" (written by sync_cycle from the dashboard), else the card
 * config, else the design default of 600 s. Clamped so a bad value can
 * never arm a zero/absurd timer. */
static unsigned next_sync_interval(void) {
    char v[16];
    if (s_kv.get(s_kv.ctx, "sync_s", v, sizeof v) == 0) {
        int s = atoi(v);
        if (s >= 60 && s <= 86400) return (unsigned)s;
    }
    if (s_have_cfg && s_cfg.sync_interval_s >= 60 && s_cfg.sync_interval_s <= 86400)
        return (unsigned)s_cfg.sync_interval_s;
    return 600;
}

/* ============================================================
 * Capture background helper: the Wi-Fi join kickoff and the recording
 * glyph both cost real time (esp_wifi bring-up ~100 ms; the glyph's
 * e-paper refresh 300 ms-2 s), so they run on a short-lived task while
 * the main task gets audio_record_to() streaming as early as possible.
 * The main task joins (semaphore) right after the recording stops and
 * only then touches the framebuffer or idf_wifi again, so both stay
 * effectively single-threaded.
 * ============================================================ */

#define CAP_BG_STACK 6144
#define CAP_BG_JOIN_TIMEOUT_MS 15000   /* first full refresh ~2 s; wide margin */

static SemaphoreHandle_t s_bg_sem;
static volatile int s_bg_wifi_started;

static int rec_held(void *ctx) { (void)ctx; return board_btn_rec(); }

static void draw_rec_glyph(void) {
    if (screen_ready() != 0) return;
    fb_clear(&s_fb);
    fb_fill(&s_fb, 92, 72, 16, 16, 1);      /* small centered dot */
    fb_text(&s_fb, 76, 104, "REC", 2, 1);
    present(0);
}

static void capture_bg_work(int start_wifi) {
    if (start_wifi && s_have_wifi &&
        idf_wifi_start_connect_async(&s_wp, &s_kv) == 0)
        s_bg_wifi_started = 1;
    draw_rec_glyph();
}

static void capture_bg_task(void *arg) {
    capture_bg_work((int)(intptr_t)arg);
    xSemaphoreGive(s_bg_sem);
    vTaskDelete(NULL);
}

static void capture_bg_start(int start_wifi) {
    if (start_wifi) s_bg_wifi_started = 0;
    if (!s_bg_sem) s_bg_sem = xSemaphoreCreateBinary();
    /* Drain a give left behind by a task that finished after its join timed
     * out: without this, the next join returns immediately on the corpse's
     * signal while a fresh background task is still drawing. */
    if (s_bg_sem) xSemaphoreTake(s_bg_sem, 0);
    if (s_bg_sem &&
        xTaskCreate(capture_bg_task, "cap_bg", CAP_BG_STACK,
                    (void *)(intptr_t)start_wifi, 1, NULL) == pdPASS)
        return;
    /* No task available: do the work inline (the brief's original order). */
    ESP_LOGW(TAG, "capture background task unavailable; running inline");
    capture_bg_work(start_wifi);
    if (s_bg_sem) xSemaphoreGive(s_bg_sem);
}

static void capture_bg_join(void) {
    if (!s_bg_sem) return;
    if (xSemaphoreTake(s_bg_sem, ms_to_ticks_min1(CAP_BG_JOIN_TIMEOUT_MS)) == pdTRUE)
        return;
    /* The task is still running and still owns the framebuffer and the EPD
     * SPI bus. The single-threaded-display invariant is broken for the rest
     * of this session, so the display is written off entirely: screen_ready()
     * now fails for every screen and render callback, and the teardown skips
     * epd_sleep() -- pushing more SPI traffic at a controller that is mid-
     * transaction is exactly the wrong move. The next boot's panel reset
     * recovers it, and the e-paper keeps whatever image it last latched. */
    s_panel_lost = 1;
    ESP_LOGE(TAG, "capture background task did not finish within %d ms; "
                  "display disabled for the rest of this session",
             CAP_BG_JOIN_TIMEOUT_MS);
}

/* Records one capture: id, WAV to SD, sidecar + rec index (the durable
 * step), with the glyph (and optionally the Wi-Fi kickoff) overlapped.
 * Returns 0 = captured (sc filled, durable on card), 1 = too short
 * (discarded), -1 = recording/SD failure (WAV removed). */
static int record_capture(sidecar_t *sc, const char *conversation_id,
                          int start_wifi, long *bytes_out) {
    uint8_t r2[2];
    s_rng.fill(s_rng.ctx, r2, 2);
    long long now = s_ck.epoch_s(s_ck.ctx);   /* system clock, else PCF85063, else 0 */
    char id[64];
    htp_make_capture_id(id, now, boot_count_bump(), s_ck.mono_ms(s_ck.ctx), r2);

    char wavl[96];
    sidecar_wav_path(wavl, id);
    char wavfs[IDF_SD_PATH_MAX];
    if (idf_ports_sd_path(wavl, wavfs, sizeof wavfs) != 0) return -1;

    capture_bg_start(start_wifi);
    /* C6 timing line: this is the moment recording begins; the wake-to-
     * record budget (design §5.2 250 ms target, checkpoint line 400 ms)
     * is measured against it. */
    ESP_LOGI(TAG, "record start at %lld ms since boot, id=%s",
             (long long)(esp_timer_get_time() / 1000), id);
    long bytes = audio_record_to(wavfs, rec_held, NULL, MAX_RECORD_MS);
    capture_bg_join();

    if (bytes_out) *bytes_out = bytes;
    if (bytes < 0) {
        s_st.remove(s_st.ctx, wavl);
        return -1;
    }
    if (bytes < MIN_WAV_BYTES) {
        ESP_LOGW(TAG, "recording too short (%ld bytes), discarded", bytes);
        s_st.remove(s_st.ctx, wavl);
        return 1;
    }

    sidecar_init(sc, id);
    sc->recorded_at = now;
    if (conversation_id && conversation_id[0])
        str_copy(sc->conversation_id, sizeof sc->conversation_id, conversation_id);
    sidecar_save(&s_st, sc);
    rec_index_append(&s_st, id);
    return 0;
}

/* ============================================================
 * Sync (design §5.4: after every session). Rendering reuses the
 * host-tested ui_flow dashboard renderer, which draws the list first and
 * the banner strip last -- the banner covering the list's bottom rows
 * (including Task 9's cursor-invert overdraw) is load-bearing.
 * ============================================================ */

static int render_dashboard_cb(void *ui_ctx, const htp_dashboard_t *d) {
    (void)ui_ctx;
    if (screen_ready() != 0) return -1;
    s_uif.dash = *d;
    status_line_fill(&s_uif.status);
    ui_flow_render(&s_uif, &s_fb);
    present(ui_flow_wants_full(&s_uif, UIF_REDRAW_PARTIAL));
    return 0;
}

/* Called on every successful notifications fetch, before the dashboard step
 * and (on a capture wake) right after the outcome screen was drawn. The
 * dashboard model is deliberately empty whenever the server answers
 * "unchanged" -- the panel's retained image is the content cache -- so a
 * clear-and-reflow here would blank real content to draw nothing. */
static int render_notifications_cb(void *ui_ctx, const htp_notifications_t *n) {
    (void)ui_ctx;
    /* Nothing to show: leave the panel exactly as it is. sync.c's ack gate
     * is count > 0, so returning success acks nothing. */
    if (n->count == 0) {
        s_uif.banner[0] = 0;
        return 0;
    }
    if (screen_ready() != 0) return -1;

    int pick = 0;
    for (int i = 0; i < n->count; i++)
        if (n->items[i].urgent) { pick = i; break; }
    str_copy(s_uif.banner, sizeof s_uif.banner, n->items[pick].text);

    if (s_base_drawn) {
        /* The framebuffer already holds this session's screen (the capture
         * outcome, or a dashboard drawn earlier): stamp the banner strip over
         * it instead of clearing. Same pixels a full ui_flow_render would put
         * there -- widget_banner is the last thing it draws -- minus the wipe. */
        widget_banner(&s_fb, s_uif.banner);
    } else {
        /* First draw of the session: the framebuffer is blank and the panel's
         * previous-image RAM was lost to epd_init(), so there is nothing to
         * stamp onto and the full flow render is the only complete screen we
         * can produce. */
        status_line_fill(&s_uif.status);
        ui_flow_render(&s_uif, &s_fb);
    }
    present(ui_flow_wants_full(&s_uif, UIF_REDRAW_PARTIAL));
    return 0;
}

/* Urgent-notification chime: audio may not be up in a sync-only session,
 * so bring it up lazily (idempotent); the rail is torn down in the common
 * teardown either way. */
static void chime_cb(void *ui_ctx) {
    (void)ui_ctx;
    if (audio_init() == 0) audio_beep();
}

static void set_rtc_cb(void *rtc_ctx, long long epoch) {
    (void)rtc_ctx;
    board_rtc_set(epoch);
}

static void run_sync(void) {
    static int uif_inited;
    if (!uif_inited) {
        ui_flow_init(&s_uif, &s_cl, &s_st, &s_kv);
        uif_inited = 1;
    }

    static capture_ctx_t ccx;
    memset(&ccx, 0, sizeof ccx);
    ccx.client = &s_cl;
    ccx.storage = &s_st;
    ccx.clock = &s_ck;
    ccx.poll_interval_ms = 1000;
    ccx.poll_window_ms = 60000;
    ccx.reply_path = REPLY_LOGICAL;

    static sync_ctx_t scx;
    memset(&scx, 0, sizeof scx);
    scx.client = &s_cl;
    scx.storage = &s_st;
    scx.kv = &s_kv;
    scx.clock = &s_ck;
    scx.capture = &ccx;
    scx.render_dashboard = render_dashboard_cb;
    scx.render_notifications = render_notifications_cb;
    scx.chime = chime_cb;
    scx.set_rtc = set_rtc_cb;

    sync_report_t rep;
    int r = sync_cycle(&scx, &rep);
    ESP_LOGI(TAG, "sync r=%d uploads=%d notifs=%d acked=%d dash_changed=%d next=%ds",
             r, rep.uploads_retried, rep.notifs_fetched, rep.notifs_acked,
             rep.dashboard_changed, rep.sync_interval_s);
}

/* ============================================================
 * Capture session (design §5.2)
 * ============================================================ */

static void show_outcome(capture_outcome_t out) {
    switch (out) {
    case CAPTURE_DONE:        screen_status_transcript("Noted", s_sc.transcript); break;
    case CAPTURE_FAILED:      screen_status("Failed - saved on card"); break;
    case CAPTURE_OFFLINE:     screen_status("Saved, will upload later"); break;
    case CAPTURE_TIMEOUT:     screen_status("Still working - check later"); break;
    case CAPTURE_AUTH_ERROR:  screen_status("Auth error - check token"); break;
    case CAPTURE_REPLY_READY: break;   /* handled by play_reply_and_follow_up */
    }
}

/* Plays the downloaded reply (REC press stops it), deletes the temp file,
 * then holds a 30 s hold-to-talk window; GEST_REC_HOLD_START records a
 * follow-up into the same conversation (sc.conversation_id carried into
 * the new sidecar before capture_run) and loops while replies keep
 * coming. */
static void play_reply_and_follow_up(capture_ctx_t *cx) {
    for (;;) {
        ESP_LOGI(TAG, "playing reply %s", REPLY_FS);
        if (audio_play_wav(REPLY_FS, rec_held, NULL) != 0)
            ESP_LOGE(TAG, "reply playback failed");
        s_st.remove(s_st.ctx, REPLY_LOGICAL);

        gesture_fsm_t g;
        gesture_init(&g);
        unsigned t0 = s_ck.mono_ms(s_ck.ctx);
        int follow = 0;
        while (s_ck.mono_ms(s_ck.ctx) - t0 < FOLLOW_UP_WINDOW_MS) {
            gesture_t ev = gesture_feed(&g, board_btn_rec(), board_btn_pwr(),
                                        s_ck.mono_ms(s_ck.ctx));
            if (ev == GEST_REC_HOLD_START) { follow = 1; break; }
            board_delay_ms(20);
        }
        if (!follow) {
            ESP_LOGI(TAG, "follow-up window closed");
            break;
        }

        long bytes = 0;
        int rr = record_capture(&s_sc_next, s_sc.conversation_id, 0, &bytes);
        if (rr != 0) {
            screen_status(rr > 0 ? "Too short" : "SD full");
            break;
        }
        ESP_LOGI(TAG, "follow-up recorded %ld bytes (~%ld ms)", bytes, bytes / 32);
        s_sc = s_sc_next;   /* the follow-up is now the current capture */

        capture_outcome_t out = capture_run(cx, &s_sc);
        if (out == CAPTURE_REPLY_READY) continue;
        show_outcome(out);
        break;
    }
}

static void capture_session(void) {
    /* Rail first: its settle overlaps the SD mount (audio_init() only
     * sleeps whatever part of the settle window hasn't already passed). */
    board_rail_audio(1);

    /* 1. capture-critical init only: SD, config, audio. */
    if (board_sd_mount() != 0) { screen_fatal("SD card error"); return; }
    if (load_config() != 0)    { screen_fatal("Config error"); return; }
    if (audio_init() != 0)     { screen_fatal("Audio error"); return; }
    load_wifi();   /* soft-fail: capture works offline, upload waits for a sync */

    /* 2. record, with the Wi-Fi kickoff + glyph overlapped. */
    long bytes = 0;
    int rr = record_capture(&s_sc, NULL, 1, &bytes);
    if (rr < 0) { screen_fatal("SD full"); return; }
    if (rr > 0) { screen_status("Too short"); return; }
    ESP_LOGI(TAG, "recorded %ld bytes (~%ld ms of 16 kHz mono)", bytes, bytes / 32);

    /* 3. network path. The recording is already durable on the card, so
     * every failure from here lands on "Saved, will upload later" and the
     * next sync retries it -- no retry loops in the session itself. */
    if (!s_bg_wifi_started || idf_wifi_wait_connected(CAPTURE_WIFI_WAIT_MS) != 0) {
        screen_status("Saved, will upload later");
        return;
    }
    s_wifi_ok = 1;
    idf_transport_init(&s_tr, s_cfg.bridge_url);
    htp_client_init(&s_cl, &s_tr, s_cfg.token);
    s_cl.battery_pct = board_battery_pct();
    ESP_LOGI(TAG, "battery=%d%% (X-Battery header)", s_cl.battery_pct);

    capture_ctx_t cx = {
        .client = &s_cl, .storage = &s_st, .clock = &s_ck,
        .on_status = screen_status_cb, .ui_ctx = NULL,
        .poll_interval_ms = 1000, .poll_window_ms = 60000,
        .reply_path = REPLY_LOGICAL,
    };
    capture_outcome_t out = capture_run(&cx, &s_sc);
    if (out == CAPTURE_REPLY_READY)
        play_reply_and_follow_up(&cx);
    else
        show_outcome(out);

    run_sync();   /* design §5.4: sync after every session */
}

/* ============================================================
 * Sync / UI sessions (UI becomes real in Task 18)
 * ============================================================ */

static void session_sync_common(wake_cause_t wc) {
    int mounted = (board_sd_mount() == 0);
    int pwr_held = power_button_held_at_boot(wc);
    int config_ok = mounted && load_config() == 0;

    if (!mounted || !config_ok || pwr_held) {
        maybe_provision(mounted, config_ok, pwr_held);   /* may never return */
        if (load_config() != 0) { screen_fatal("Config error"); return; }
    }
    if (load_wifi() != 0) { screen_fatal("WiFi config error"); return; }

    if (idf_wifi_connect(&s_wp, &s_kv, SYNC_WIFI_TIMEOUT_MS) != 0) {
        screen_status("No network");
        return;   /* the timer wake retries on its own */
    }
    s_wifi_ok = 1;
    idf_transport_init(&s_tr, s_cfg.bridge_url);
    htp_client_init(&s_cl, &s_tr, s_cfg.token);
    s_cl.battery_pct = board_battery_pct();
    ESP_LOGI(TAG, "battery=%d%% (X-Battery header)", s_cl.battery_pct);
    run_sync();
}

static void sync_session_stub(wake_cause_t wc) { session_sync_common(wc); }
static void ui_session_stub(wake_cause_t wc)   { session_sync_common(wc); }

/* ============================================================
 * Development stay-awake tail (see HTP_DEV_LINGER above).
 * ============================================================ */

#if HTP_DEV_LINGER

#define DEV_HEARTBEAT_MS    30000               /* heartbeat cadence while staying awake */
#define DEV_IDLE_TIMEOUT_MS (10u * 60u * 1000u) /* ~10 min without input -> deep sleep */
#define DEV_PWR_HOLD_MS     2000                /* PWR held this long -> deep sleep */

/* End-of-session behavior for development builds: stay awake with
 * USB-Serial-JTAG alive so the operator can keep iterating (flash, read
 * logs, poke serial), instead of dropping into deep sleep immediately and
 * killing the port. Deep sleep happens only on:
 *   - "sleep" + Enter on the serial console,
 *   - the Power button held ~2 s (then released -- we wait for the release
 *     so board_deep_sleep()'s EXT1 any-low wake doesn't fire the instant
 *     we go down and bounce straight back into a boot),
 *   - DEV_IDLE_TIMEOUT_MS with no serial byte and no button press.
 * All timing is wall-clock (esp_timer), never iteration-counted. Never
 * returns. The eventual deep sleep arms the same sync-interval timer the
 * release build would, so timer wakes stay exercisable. */
static void dev_stay_awake_then_sleep(unsigned sleep_s) {
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

    ESP_LOGI(TAG, "entering deep sleep for %u s (wake: REC/PWR button or timer)", sleep_s);
    board_deep_sleep(sleep_s);
}

#endif /* HTP_DEV_LINGER */

/* ============================================================
 * Entry
 * ============================================================ */

void app_main(void) {
    int64_t t_entry_ms = esp_timer_get_time() / 1000;
    board_early_init();
    console_init_usb_serial_jtag();
    idf_ports_init(&s_st, &s_kv, &s_ck, &s_rng);   /* pointer wiring only, no hardware */

    wake_cause_t wc = board_wake_cause();
    static const char *cause[] = { "cold", "rec-button", "pwr-button", "timer" };
    ESP_LOGI(TAG, "HTP terminal, wake=%s (app_main entry at %lld ms since boot)",
             cause[wc], (long long)t_entry_ms);

    if (wc == WAKE_REC_BUTTON)      capture_session();   /* fast path first */
    else if (wc == WAKE_PWR_BUTTON) ui_session_stub(wc);
    else                            sync_session_stub(wc);   /* timer + cold */

    /* Common teardown: radio off, codec + rail off, panel asleep. All of
     * these are safe no-ops when the session never brought them up. */
    idf_wifi_stop();
    audio_deinit();
    if (s_epd_up && !s_panel_lost) {
        epd_sleep();
        s_epd_up = 0;
    }
    ESP_LOGI(TAG, "session done; main task min free stack %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    unsigned sleep_s = next_sync_interval();
#if HTP_DEV_LINGER
    dev_stay_awake_then_sleep(sleep_s);   /* never returns */
#else
    ESP_LOGI(TAG, "entering deep sleep for %u s (wake: REC/PWR button or timer)", sleep_s);
    board_deep_sleep(sleep_s);
#endif
}
