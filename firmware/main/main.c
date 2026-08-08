/* HTP terminal firmware — wake dispatch, capture/UI/sync sessions (Task 18).
 *
 * app_main() dispatches on the wake cause (design §5.1):
 *   REC button  -> capture_session()   hold-to-talk, upload, reply, sync
 *   PWR button  -> ui_session()        cached dashboard instantly, background
 *                                      join + sync, gesture loop, 30 s idle
 *   timer/cold  -> sync_session()      silent: renders only what changed
 *
 * An awake watchdog (design §8) caps every session: a one-shot esp_timer
 * armed before dispatch and re-armed at phase transitions (recording,
 * upload/poll, playback, each operator gesture), so a wedged session deep-
 * sleeps instead of draining the battery while every legitimate phase
 * finishes ahead of its cap.
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
#include <time.h>
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
#include "esp_mac.h"
#include "esp_app_desc.h"
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
 * stays awake with the USB-Serial-JTAG console alive. C7 finding C:
 * buttons ACT during the linger instead of being swallowed -- a REC
 * press starts a new capture session, a PWR tap opens a UI session
 * (both end back in the linger); 'sleep' + Enter, PWR held ~2 s, or
 * DEV_IDLE_TIMEOUT_MS of idleness drop into the real deep sleep with
 * the sync timer armed, so the full sleep/wake cycle stays verifiable.
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
static int s_pending_cache = -1;    /* memoized capture_pending_count(); -1 = recount.
                                       Invalidated wherever an upload state changes, so
                                       cursor-move redraws don't re-walk the SD index. */
static int64_t s_linger_rec_press_ms = -1;  /* C7 finding C: ms-since-boot of the REC
                                               press that exited the dev linger, so
                                               record_capture can log press-to-record-
                                               start latency. -1 = not a linger launch. */

/* Dashboard snapshot (/dash.bin): the last rendered htp_dashboard_t, so a
 * PWR wake paints the dashboard instantly from SD while Wi-Fi joins in the
 * background, and a sync-only notification render has a real list to stamp
 * the banner onto. Binary struct dump rather than the re-serialized JSON:
 * htp_client has a parser but no serializer, and the magic + size header
 * discards a snapshot written by any other firmware layout. */
#define DASH_CACHE_PATH  "/dash.bin"
#define DASH_CACHE_MAGIC 0x48445348u
typedef struct {
    uint32_t magic;
    uint32_t size;              /* sizeof(htp_dashboard_t) layout guard */
    htp_dashboard_t d;
} dash_cache_t;
static dash_cache_t s_dash_cache;   /* ~3.7 KB: static per the stack ruling */

#define REPLY_LOGICAL "/reply.tmp.wav"
#define REPLY_FS      "/sdcard" REPLY_LOGICAL

#define CAPTURE_WIFI_WAIT_MS 8000    /* post-recording wait; a stale fast join can
                                        roughly double this via the scan fallback */
#define SYNC_WIFI_TIMEOUT_MS 20000   /* sync sessions aren't latency-critical */
#define MAX_RECORD_MS        120000  /* hard cap on one recording (design §5.2) */
#define MIN_WAV_BYTES        8000    /* < 250 ms of 16 kHz mono: discard */
#define FOLLOW_UP_WINDOW_MS  30000   /* hold-to-talk window after a reply */
#define UI_IDLE_TIMEOUT_MS   30000   /* UI session: idle this long -> sleep */
#define PWR_COUNTDOWN_MS     2000    /* PWR held this long -> power-off warning */

/* Awake-watchdog phase caps (design §8). Armed before dispatch and re-armed
 * at phase transitions; each value bounds one phase, not the session sum:
 *   BASE  UI gestures / sync sessions (join 20 s + fetches ~60 s worst)
 *   REC   phases that may contain one full recording or playback (120 s max)
 *         plus interaction margin (follow-up window, boot-to-record)
 *   NET   upload + poll: 3 upload attempts x 60 s timeout + backoff (~185 s)
 *         + 60 s poll window of 15 s-timeout polls + reply download */
#define AWAKE_CAP_BASE_S 90
#define AWAKE_CAP_REC_S  210
#define AWAKE_CAP_NET_S  300

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
        /* Every ACTUAL full clears all ghosting, so the ghost-clear budget
         * restarts here rather than at the (several) call sites that ask
         * for one. C7 round 2: the base gate above promotes to full without
         * any caller asking, so the counter used to carry a session's
         * pre-sleep partials into the fresh screen and fire the first
         * ghost-clear early. ui_flow_wants_full(UIF_REDRAW_FULL) is just
         * the reset accessor for that counter; its return is the constant
         * 1 and means nothing here. s_uif is zero-initialized, so this is
         * safe on the pre-ensure_uif() paths (screen_fatal at entry). */
        (void)ui_flow_wants_full(&s_uif, UIF_REDRAW_FULL);
    } else {
        epd_partial(s_fb.px);
    }
}

/* Pending-upload count for the status header, memoized because every
 * redraw calls it and the raw count re-reads the rec index (16 KB SD
 * read) plus every sidecar. Everything that changes an upload state
 * resets s_pending_cache to -1. */
static int pending_uploads(void) {
    if (s_pending_cache < 0) s_pending_cache = capture_pending_count(&s_st);
    return s_pending_cache;
}

static void status_line_fill(ui_status_t *stt) {
    memset(stt, 0, sizeof *stt);
    stt->battery_pct = board_battery_pct();
    stt->wifi_ok = s_wifi_ok;
    stt->pending_uploads = pending_uploads();
    /* Clock: local wall time from the RTC-backed epoch. "Local" is the
     * config.json "timezone" POSIX TZ string applied in load_config()
     * (C7 finding D); with no field TZ is pinned to UTC0. Rendered
     * 12-hour with no AM/PM and no leading zero -- "3:14", "11:17" --
     * per the operator's C7 round-3 finding 1: a glanceable pocket clock,
     * not a timestamp. Hidden until the clock is set. */
    long long e = s_ck.epoch_s(s_ck.ctx);
    if (e > 0) {
        time_t t = (time_t)e;
        struct tm tmv;
        localtime_r(&t, &tmv);
        int h12 = tmv.tm_hour % 12;
        if (h12 == 0) h12 = 12;
        /* unsigned + modulo keep the compiler's format-truncation proof
         * inside the 6-byte field ("12:59" worst case) */
        snprintf(stt->clock_hhmm, sizeof stt->clock_hhmm, "%u:%02u",
                 (unsigned)h12 % 13u, (unsigned)tmv.tm_min % 60u);
    }
}

/* Word-wraps msg into the shared framebuffer at `cols` chars/line, in the
 * body font (Spleen 8x16 via fb_text16). */
static void draw_wrapped(const char *msg, int x, int y0, int dy,
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
        fb_text16(&s_fb, x, y, line, 1);
        p += take;
        y += dy;
        lines++;
    }
}

/* Status line + body-font message (UI_LINE_CHARS cols), partial refresh. */
static void screen_status(const char *msg) {
    ESP_LOGI(TAG, "status: %s", msg);
    if (screen_ready() != 0) return;
    ui_status_t stt;
    status_line_fill(&stt);
    fb_clear(&s_fb);
    widget_status_line(&s_fb, &stt);
    draw_wrapped(msg, 4, 44, UI_TEXT_LINE_H, UI_LINE_CHARS, 6);
    present(0);
}

/* capture_ctx_t.on_status adapter */
static void screen_status_cb(void *ui_ctx, const char *line) {
    (void)ui_ctx;
    screen_status(line);
}

/* Primary status word at UI_HEAD_SCALE (24 px -- "Noted"/"Done" must be
 * readable at arm's length) + the opening of the transcript in the body
 * font below. */
static void screen_status_transcript(const char *status, const char *transcript) {
    ESP_LOGI(TAG, "status: %s transcript=%.80s", status, transcript);
    if (screen_ready() != 0) return;
    ui_status_t stt;
    status_line_fill(&stt);
    fb_clear(&s_fb);
    widget_status_line(&s_fb, &stt);
    fb_text(&s_fb, 4, 26, status, UI_HEAD_SCALE, 1);
    draw_wrapped(transcript[0] ? transcript : "(no transcript)", 4, 60,
                 UI_TEXT_LINE_H, UI_LINE_CHARS, 5);
    present(0);
}

/* Terminal screen for a session that cannot continue; the panel is put to
 * sleep immediately (the image persists without power) so the common
 * teardown has nothing left to do. C7 finding A: no longer forces a full --
 * nearly every fatal is the session's first draw (SD/config errors at
 * entry), which present()'s base gate makes a full anyway; a mid-session
 * fatal ("SD full") is a within-session update like any other. */
static void screen_fatal(const char *msg) {
    ESP_LOGE(TAG, "fatal: %s", msg);
    if (screen_ready() != 0) return;
    ui_status_t stt;
    status_line_fill(&stt);
    fb_clear(&s_fb);
    widget_status_line(&s_fb, &stt);
    draw_wrapped(msg, 4, 44, UI_TEXT_LINE_H, UI_LINE_CHARS, 6);
    present(0);
    epd_sleep();
    s_epd_up = 0;
    s_base_drawn = 0;
}

/* Redraws only the status strip over whatever screen the panel already
 * shows (partial refresh): the "upload states changed but the content on
 * screen is still right" case -- e.g. the after-capture sync retried an
 * older pending upload while the outcome screen is up. */
static void refresh_status_strip(void) {
    if (screen_ready() != 0 || !s_base_drawn) return;
    ui_status_t stt;
    status_line_fill(&stt);
    fb_fill(&s_fb, 0, 0, UI_W, UI_STATUS_H, 0);   /* clear the strip to white */
    widget_status_line(&s_fb, &stt);
    present(0);
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
    ESP_LOGI(TAG, "bridge=%s token=%.4s...(%d) sync=%d tz=%s",
             s_cfg.bridge_url, s_cfg.token, (int)strlen(s_cfg.token), s_cfg.sync_interval_s,
             s_cfg.timezone[0] ? s_cfg.timezone : "(UTC)");
    /* C7 finding D: apply the config timezone once per config load -- every
     * session path funnels through here before it draws a clock. newlib
     * picks it up via TZ/tzset; an empty field pins UTC explicitly so a
     * stale environment can never leak into the clock. Applied on the IDF
     * side only: app_core just parses the string. */
    setenv("TZ", s_cfg.timezone[0] ? s_cfg.timezone : "UTC0", 1);
    tzset();
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
 * never arm a zero/absurd timer. Battery policy (design §8): below 15%
 * the interval quadruples; -1 (ADC unavailable) must not trip it. */
static unsigned next_sync_interval(void) {
    unsigned s = 600;
    char v[16];
    int got = 0;
    if (s_kv.get(s_kv.ctx, "sync_s", v, sizeof v) == 0) {
        int n = atoi(v);
        if (n >= 60 && n <= 86400) { s = (unsigned)n; got = 1; }
    }
    if (!got && s_have_cfg && s_cfg.sync_interval_s >= 60 && s_cfg.sync_interval_s <= 86400)
        s = (unsigned)s_cfg.sync_interval_s;

    int batt = board_battery_pct();
    if (batt >= 0 && batt < 15) {
        s *= 4;
        if (s > 86400) s = 86400;
    }
    return s;
}

/* ============================================================
 * Awake watchdog (design §8): a one-shot esp_timer whose only job is to
 * turn a wedged session into a deep sleep instead of a drained battery.
 * Armed before dispatch, re-armed at phase transitions (see the
 * AWAKE_CAP_* table), cancelled once the session returns -- the dev
 * linger has its own idle timeout and must not be capped.
 * ============================================================ */

static esp_timer_handle_t s_awake_cap;

static void awake_cap_cb(void *arg) {
    (void)arg;
    ESP_LOGE(TAG, "awake cap hit");
    board_deep_sleep(next_sync_interval());
}

static void awake_cap_arm(unsigned seconds) {
    if (!s_awake_cap) return;
    esp_timer_stop(s_awake_cap);   /* harmless when not running */
    esp_timer_start_once(s_awake_cap, (uint64_t)seconds * 1000000ULL);
}

static void awake_cap_cancel(void) {
    if (s_awake_cap) esp_timer_stop(s_awake_cap);
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
    /* headline size (24 px), centered: 3 chars * 8 * UI_HEAD_SCALE = 72 px */
    fb_text(&s_fb, (UI_W - 3 * 8 * UI_HEAD_SCALE) / 2, 104, "REC", UI_HEAD_SCALE, 1);
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
    long long t_rec_ms = esp_timer_get_time() / 1000;
    if (s_linger_rec_press_ms >= 0) {
        ESP_LOGI(TAG, "record start %lld ms after the linger REC press",
                 t_rec_ms - (long long)s_linger_rec_press_ms);
        s_linger_rec_press_ms = -1;
    }
    ESP_LOGI(TAG, "record start at %lld ms since boot, id=%s", t_rec_ms, id);
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
    s_pending_cache = -1;   /* one more not_uploaded sidecar on the card */
    return 0;
}

/* ============================================================
 * Sync (design §5.4: after every session) and the shared ui_flow model.
 * Rendering reuses the host-tested ui_flow renderer; since Task 18's
 * scale-2 layout the list and the banner strip partition the panel
 * exactly, so neither ever paints over the other.
 * ============================================================ */

static void ensure_uif(void) {
    static int inited;
    if (inited) return;
    ui_flow_init(&s_uif, &s_cl, &s_st, &s_kv);
    inited = 1;
}

static void save_dashboard_cache(const htp_dashboard_t *d) {
    s_dash_cache.magic = DASH_CACHE_MAGIC;
    s_dash_cache.size = (uint32_t)sizeof(htp_dashboard_t);
    s_dash_cache.d = *d;
    if (s_st.write(s_st.ctx, DASH_CACHE_PATH, &s_dash_cache, sizeof s_dash_cache) != 0)
        ESP_LOGW(TAG, "dashboard snapshot write failed");   /* cosmetic loss only */
}

static void load_cached_dashboard(void) {
    size_t len = 0;
    if (s_st.read(s_st.ctx, DASH_CACHE_PATH, &s_dash_cache, sizeof s_dash_cache, &len) != 0)
        return;
    if (len != sizeof s_dash_cache || s_dash_cache.magic != DASH_CACHE_MAGIC ||
        s_dash_cache.size != (uint32_t)sizeof(htp_dashboard_t)) {
        ESP_LOGW(TAG, "dashboard snapshot ignored (other layout or torn write)");
        return;
    }
    /* The header proves the blob has the right shape, not that the SD card
     * didn't bit-rot the body. item_count feeds unchecked array indexing
     * throughout ui_flow (cursor navigation, the complete gesture's
     * items[idx] write) and widget_list's scroll window -- a corrupt count
     * must never reach s_uif.dash. Reject the whole cache rather than clamp
     * it: this is only a cosmetic preload, so a rejection just means the
     * first screen paints after Wi-Fi joins instead of instantly. */
    htp_dashboard_t *d = &s_dash_cache.d;
    if (d->item_count < 0 || d->item_count > 32 ||
        d->sync_interval < 60 || d->sync_interval > 86400) {
        ESP_LOGW(TAG, "dashboard snapshot rejected (corrupt payload)");
        return;
    }
    /* Fixed char fields are raw bytes off the card, not guaranteed
     * NUL-terminated by whatever corrupted them -- force it before
     * anything downstream (str_copy/strlen via ui_flow's renderers) reads
     * past the field into adjacent struct memory. */
    d->rev[sizeof d->rev - 1] = '\0';
    d->title[sizeof d->title - 1] = '\0';
    for (int i = 0; i < d->item_count; i++) {
        d->items[i].id[sizeof d->items[i].id - 1] = '\0';
        d->items[i].text[sizeof d->items[i].text - 1] = '\0';
        d->items[i].style[sizeof d->items[i].style - 1] = '\0';
    }
    s_uif.dash = s_dash_cache.d;
}

/* Renders ui_flow's current screen with a live status header. An explicit
 * full also resets the ghost-clear budget (a full clears all ghosting, so
 * the partial count restarts from zero -- C7 finding A). */
static void flow_redraw(int want_full) {
    if (screen_ready() != 0) return;
    if (want_full) ui_flow_wants_full(&s_uif, UIF_REDRAW_FULL);
    status_line_fill(&s_uif.status);
    ui_flow_render(&s_uif, &s_fb);
    present(want_full);
}

/* 1 when render_dashboard_cb actually put pixels on the panel during the
 * sync_cycle that just ran. Reset in run_sync() before every cycle. C7
 * round 2: the callers below used to guard their status-strip refresh on
 * sync_report_t.dashboard_changed, i.e. on "the fetch returned a new rev",
 * which stopped implying "the screen was repainted" the moment finding B
 * added content-diff suppression -- a new rev carrying identical content
 * paints nothing, so guarding on it re-opened C6's stale "Saved, will
 * upload later" header in exactly that corner. Guard on what was painted,
 * not on what changed. */
static int s_dash_painted;

static int render_dashboard_cb(void *ui_ctx, const htp_dashboard_t *d) {
    (void)ui_ctx;
    /* C7 finding B: the bridge's rev is a content hash that the agent's
     * republish bumps even when nothing the panel renders differs -- most
     * visibly seconds after a complete gesture, whose _notify_completion
     * makes the agent republish the very list the strike partial already
     * shows. Adopt the fresh model and snapshot it (ids/rev may have
     * changed and the complete gesture posts ids), but skip the repaint
     * when no pixel would change. sync.c stores the new rev regardless,
     * so the next fetch reads "unchanged". */
    int same = ui_dash_content_equal(&s_uif.dash, d);
    s_uif.dash = *d;
    save_dashboard_cache(d);   /* next PWR wake paints this instantly */
    if (same) {
        ESP_LOGI(TAG, "dashboard rev changed, content identical: no repaint");
        return 0;
    }
    if (screen_ready() != 0) return -1;
    status_line_fill(&s_uif.status);
    ui_flow_render(&s_uif, &s_fb);
    present(ui_flow_wants_full(&s_uif, UIF_REDRAW_PARTIAL));
    s_dash_painted = 1;   /* this paint carried a fresh status header */
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

static void run_sync(sync_report_t *out) {
    ensure_uif();

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

    /* Invalidate before AND after: step 1 (retry) changes upload states
     * before the render callbacks run, so their status headers must
     * recount; step 4 (backfill) can re-mark an unknown capture as
     * not_uploaded after the renders. */
    s_pending_cache = -1;
    s_dash_painted = 0;   /* set by render_dashboard_cb only if it paints */
    int r = sync_cycle(&scx, out);
    s_pending_cache = -1;
    ESP_LOGI(TAG, "sync r=%d uploads=%d notifs=%d acked=%d dash_changed=%d painted=%d next=%ds",
             r, out->uploads_retried, out->notifs_fetched, out->notifs_acked,
             out->dashboard_changed, s_dash_painted, out->sync_interval_s);
    ESP_LOGI(TAG, "stack hwm after sync_cycle: %u bytes min free",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
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
        /* One conversation turn: playback + 30 s window + one recording. */
        awake_cap_arm(AWAKE_CAP_REC_S);
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
            /* Resting screen: without this the panel keeps whatever the
             * conversation left up (REC glyph / "Uploaded") forever. */
            screen_status_transcript("Done", s_sc.transcript);
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

        awake_cap_arm(AWAKE_CAP_NET_S);   /* upload retries + poll window */
        capture_outcome_t out = capture_run(cx, &s_sc);
        s_pending_cache = -1;
        if (out == CAPTURE_REPLY_READY) continue;
        show_outcome(out);
        break;
    }
}

/* from_ui = 1 when entered from ui_session's UIF_START_CAPTURE: the card
 * is mounted, config and wifi.json are loaded, and the Wi-Fi join may
 * already be running or up (s_bg_wifi_started) -- starting a second join
 * would disrupt the first (Task 17 double-start concern). */
static void capture_session_run(int from_ui) {
    if (!from_ui) {
        /* Rail first: its settle overlaps the SD mount (audio_init() only
         * sleeps whatever part of the settle window hasn't already passed). */
        board_rail_audio(1);

        /* 1. capture-critical init only: SD, config, audio. */
        if (board_sd_mount() != 0) { screen_fatal("SD card error"); return; }
        if (load_config() != 0)    { screen_fatal("Config error"); return; }
        load_wifi();   /* soft-fail: capture works offline, upload waits for a sync */
    }
    if (audio_init() != 0) { screen_fatal("Audio error"); return; }

    /* 2. record, with the glyph -- and, unless a join is already in
     * flight, the Wi-Fi kickoff -- overlapped. */
    long bytes = 0;
    int rr = record_capture(&s_sc, NULL, !s_bg_wifi_started, &bytes);
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
    awake_cap_arm(AWAKE_CAP_NET_S);   /* upload retries + poll window */
    capture_outcome_t out = capture_run(&cx, &s_sc);
    s_pending_cache = -1;             /* capture_run moved the upload state */
    if (out == CAPTURE_REPLY_READY)
        play_reply_and_follow_up(&cx);
    else
        show_outcome(out);

    awake_cap_arm(AWAKE_CAP_BASE_S);
    sync_report_t rep;
    run_sync(&rep);   /* design §5.4: sync after every session */
    /* The outcome screen is the content here; if the sync also landed
     * older pending uploads, only the status header needs refreshing. */
    if (rep.uploads_retried > 0)
        refresh_status_strip();
}

/* Wrapper so the linger's press timestamp is dropped on EVERY exit from
 * the capture path, not only on the one where record_capture() got far
 * enough to consume it (C7 round 2): an abort before that -- SD, config or
 * audio failure -- used to leave it set, and the NEXT capture's
 * press-to-record log line was then measured from a press that belonged to
 * the aborted session. */
static void capture_session(int from_ui) {
    capture_session_run(from_ui);
    s_linger_rec_press_ms = -1;
}

/* ============================================================
 * Sync / UI sessions
 * ============================================================ */

/* Shared session front half: mount, provisioning triggers, config,
 * wifi.json. Returns 0 with everything loaded, -1 after a fatal screen. */
static int common_init(wake_cause_t wc) {
    int mounted = (board_sd_mount() == 0);
    int pwr_held = power_button_held_at_boot(wc);
    int config_ok = mounted && load_config() == 0;

    if (!mounted || !config_ok || pwr_held) {
        maybe_provision(mounted, config_ok, pwr_held);   /* may never return */
        if (load_config() != 0) { screen_fatal("Config error"); return -1; }
    }
    if (load_wifi() != 0) { screen_fatal("WiFi config error"); return -1; }
    return 0;
}

/* Timer/cold wake (design §7.3): silent -- the render callbacks only
 * touch the panel when something actually changed, and an upload-state
 * change counts as a change (C6 hardware finding: a quiet sync that
 * landed pending uploads used to leave "Saved, will upload later" up). */
static void sync_session(wake_cause_t wc) {
    if (common_init(wc) != 0) return;
    ensure_uif();
    load_cached_dashboard();   /* a first-draw notification render stamps the
                                  banner onto the cached list, not onto a blank */

    /* Battery policy (design §8): below 5%, timer syncs are skipped before
     * the radio ever powers up. -1 (ADC unavailable) must not trip this. */
    int batt = board_battery_pct();
    if (batt >= 0 && batt < 5 && wc == WAKE_TIMER) {
        ESP_LOGW(TAG, "battery %d%%: skipping timer sync", batt);
        return;
    }

    if (idf_wifi_connect(&s_wp, &s_kv, SYNC_WIFI_TIMEOUT_MS) != 0) {
        /* Timer wakes stay silent even offline (nothing changed, never
         * flash); a cold/PWR-triggered boot tells the operator. */
        if (wc != WAKE_TIMER) screen_status("No network");
        return;   /* the timer wake retries on its own */
    }
    s_wifi_ok = 1;
    idf_transport_init(&s_tr, s_cfg.bridge_url);
    htp_client_init(&s_cl, &s_tr, s_cfg.token);
    s_cl.battery_pct = batt;
    ESP_LOGI(TAG, "battery=%d%% (X-Battery header)", s_cl.battery_pct);
    /* C5 fix round 1: the overflow died before the teardown's high-water
     * line could ever print, so log it at session milestones too. */
    ESP_LOGI(TAG, "stack hwm after wifi: %u bytes min free",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    int pend_before = pending_uploads();
    sync_report_t rep;
    run_sync(&rep);
    /* Guarded on what was painted, not on rep.dashboard_changed: a changed
     * rev whose content is identical repaints nothing (C7 finding B), and
     * that case still needs this screen. */
    if ((rep.uploads_retried > 0 || pending_uploads() != pend_before) &&
        !s_dash_painted)
        screen_status(pending_uploads() == 0 ? "Uploaded" : "Upload retried");
}

/* Settings screen data: factory MAC (never spoofed, by design -- some
 * networks need it registered), app version, the host[:port] of the
 * bridge URL (the port tells the mock bridge from the real one), and the
 * effective sync interval. */
static void populate_settings_info(ui_settings_info_t *si) {
    memset(si, 0, sizeof *si);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(si->mac, sizeof si->mac, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    str_copy(si->fw_version, sizeof si->fw_version, esp_app_get_description()->version);
    const char *h = strstr(s_cfg.bridge_url, "://");
    h = h ? h + 3 : s_cfg.bridge_url;
    size_t i = 0;
    while (h[i] && h[i] != '/' && i < sizeof si->bridge_host - 1) {
        si->bridge_host[i] = h[i];
        i++;
    }
    si->bridge_host[i] = '\0';
    si->sync_interval_s = (int)next_sync_interval();
}

/* PWR wake: paint the cached dashboard immediately, join Wi-Fi in the
 * background, sync once it lands, and serve gestures until 30 s of
 * idleness. Single-threaded display throughout: the only other panel
 * writer (cap_bg) runs inside capture_session(), which joins it before
 * this task touches the framebuffer again. */
static void ui_session(wake_cause_t wc) {
    if (common_init(wc) != 0) return;
    ensure_uif();

    /* Client up before the network is: a complete-item tap before the
     * join lands fails with a network error and leaves the item unmarked
     * -- better than gating every gesture on Wi-Fi. */
    idf_transport_init(&s_tr, s_cfg.bridge_url);
    htp_client_init(&s_cl, &s_tr, s_cfg.token);
    s_cl.battery_pct = board_battery_pct();
    populate_settings_info(&s_uif.info);
    load_cached_dashboard();

    flow_redraw(1);   /* instant dashboard from the SD snapshot */

    /* 0 = up, 1 = joining, -1 = down/none. Second entry point for the
     * async join: s_bg_wifi_started tells capture_session not to start
     * another one (Task 17 double-start concern). */
    int wifi_state = -1;
    if (s_have_wifi && idf_wifi_start_connect_async(&s_wp, &s_kv) == 0) {
        s_bg_wifi_started = 1;
        wifi_state = 1;
    }

    int synced = 0, countdown_shown = 0;
    gesture_fsm_t g;
    gesture_init(&g);
    unsigned idle_t0 = s_ck.mono_ms(s_ck.ctx);
    while (s_ck.mono_ms(s_ck.ctx) - idle_t0 < UI_IDLE_TIMEOUT_MS) {
        if (!synced && wifi_state > 0) {
            wifi_state = idf_wifi_poll_connected();   /* non-blocking probe */
            if (wifi_state == 0) {
                s_wifi_ok = 1;
                s_cl.battery_pct = board_battery_pct();
                ESP_LOGI(TAG, "battery=%d%% (X-Battery header)", s_cl.battery_pct);
                int pend_before = pending_uploads();
                sync_report_t rep;
                run_sync(&rep);
                synced = 1;
                s_uif.info.sync_interval_s = (int)next_sync_interval();
                /* Upload-state changes count as changes: refresh the
                 * header's pending count even when nothing else redrew.
                 * Guarded like sync_session's twin -- when
                 * render_dashboard_cb actually painted during run_sync()
                 * the fresh screen already carries the fresh header, so
                 * this would be a redundant partial refresh. A changed rev
                 * that painted NOTHING (identical content, finding B) does
                 * still need it. */
                if ((rep.uploads_retried > 0 || pending_uploads() != pend_before) &&
                    !s_dash_painted)
                    flow_redraw(ui_flow_wants_full(&s_uif, UIF_REDRAW_PARTIAL));
                idle_t0 = s_ck.mono_ms(s_ck.ctx);   /* sync time isn't idle time */
            }
        }

        unsigned now = s_ck.mono_ms(s_ck.ctx);
        gesture_t ge = gesture_feed(&g, board_btn_rec(), board_btn_pwr(), now);

        /* Power-off countdown: PWR held past 2 s warns once (partial
         * refresh); the GEST_PWR_OFF at 5 s then executes it. */
        if (g.pwr_down && !countdown_shown && now - g.pwr_t0 > PWR_COUNTDOWN_MS) {
            screen_status("Hold to power off...");
            countdown_shown = 1;
        }

        if (ge == GEST_NONE) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        idle_t0 = s_ck.mono_ms(s_ck.ctx);
        awake_cap_arm(AWAKE_CAP_BASE_S);   /* operator present: the cap bounds
                                              wedges, not interaction */

        if (countdown_shown && ge != GEST_PWR_OFF) {
            /* Released short of 5 s: the hold was power-off intent (the
             * release resolves as PWR_LONG) -- swallow it, restore the
             * screen. */
            countdown_shown = 0;
            flow_redraw(ui_flow_wants_full(&s_uif, UIF_REDRAW_PARTIAL));
            continue;
        }

        char path[96];
        ui_action_t a = ui_flow_gesture(&s_uif, ge, path);
        switch (a) {
        case UIF_START_CAPTURE:
            awake_cap_arm(AWAKE_CAP_REC_S);
            capture_session(1);
            return;
        case UIF_PLAY_WAV: {
            char fs[IDF_SD_PATH_MAX];
            awake_cap_arm(AWAKE_CAP_REC_S);   /* a recording can run 120 s */
            if (idf_ports_sd_path(path, fs, sizeof fs) == 0 && audio_init() == 0) {
                audio_play_wav(fs, rec_held, NULL);   /* REC press stops it */
                audio_deinit();
            }
            break;
        }
        case UIF_POWER_OFF:
            screen_status("Powering off");
            board_power_off();   /* releases the VBAT latch; on USB power the
                                    awake cap turns this into a deep sleep */
            break;
        case UIF_REDRAW_PARTIAL:
        case UIF_REDRAW_FULL: {
            int dash_tap = (ge == GEST_REC_SHORT && s_uif.screen == SCR_DASHBOARD);
            flow_redraw(ui_flow_wants_full(&s_uif, a));
            if (dash_tap)
                save_dashboard_cache(&s_uif.dash);   /* keep the strike-through
                                                        across sleeps: the new rev
                                                        will read "unchanged" */
            break;
        }
        default:
            break;
        }
    }
    ESP_LOGI(TAG, "ui idle %d s; sleeping", UI_IDLE_TIMEOUT_MS / 1000);
}

/* ============================================================
 * Development stay-awake tail (see HTP_DEV_LINGER above).
 * ============================================================ */

#if HTP_DEV_LINGER

#define DEV_HEARTBEAT_MS    30000               /* heartbeat cadence while staying awake */
#define DEV_IDLE_TIMEOUT_MS (10u * 60u * 1000u) /* ~10 min without input -> deep sleep */
#define DEV_PWR_HOLD_MS     2000                /* PWR held this long -> deep sleep */
#define DEV_BTN_DEBOUNCE_MS 60                  /* PWR-tap debounce: the same continuous
                                                   re-sampling pattern as power_button_
                                                   held_at_boot, at tap scale (3 polls).
                                                   REC uses GEST_REC_HOLD_MS instead --
                                                   it starts a recording, not a menu */

/* End-of-session behavior for development builds: stay awake with
 * USB-Serial-JTAG alive so the operator can keep iterating (flash, read
 * logs, poke serial), instead of dropping into deep sleep immediately and
 * killing the port.
 *
 * C7 finding C: buttons act here now (the operator could not record a
 * second capture without typing 'sleep' first). Outcomes:
 *   - REC held GEST_REC_HOLD_MS of continuous samples (the same sustained
 *     hold the UI session's gesture FSM demands, so a stray tap does not
 *     spin up a capture that can only end "Too short"): returns
 *     WAKE_REC_BUTTON -- the caller dispatches a capture session while the
 *     operator is still holding the button (hold-to-talk from this very
 *     press; s_linger_rec_press_ms carries the press time, so the
 *     press-to-record-start line record_capture logs includes this hold).
 *   - PWR tapped (>= debounce, released before DEV_PWR_HOLD_MS): returns
 *     WAKE_PWR_BUTTON -- the caller dispatches a UI session.
 *   - "sleep" + Enter, PWR held ~2 s (release waited so EXT1 doesn't
 *     bounce straight back into a boot), or DEV_IDLE_TIMEOUT_MS of
 *     idleness: deep sleep, never returns. The deep sleep arms the same
 *     sync-interval timer the release build would.
 * All timing is wall-clock (esp_timer), never iteration-counted. */
static wake_cause_t dev_linger(unsigned sleep_s) {
    ESP_LOGI(TAG, "staying awake for development; buttons live: hold REC = record, "
                  "tap PWR = menu; 'sleep' + Enter or hold PWR ~2s = deep sleep now");
    ESP_LOGI(TAG, "auto deep-sleep after %u min idle", (unsigned)(DEV_IDLE_TIMEOUT_MS / 60000u));

    char line[16];
    size_t n = 0;
    int64_t t_last_input = esp_timer_get_time() / 1000;
    int64_t t_next_heartbeat = t_last_input + DEV_HEARTBEAT_MS;
    int64_t t_pwr_down_since = -1;   /* -1 = button not currently pressed */
    int64_t t_rec_down_since = -1;

    for (;;) {
        /* console_getc() blocks in the driver up to CONSOLE_POLL_MS, so
         * this loop wakes ~50x/s when idle -- that's also the button
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

        /* REC: a sustained hold exits straight into a capture session.
         * No wait for release -- the hold IS the recording gesture. The
         * threshold is the UI session's own hold-to-talk threshold
         * (GEST_REC_HOLD_MS, 350 ms) rather than the tap-scale debounce:
         * C7 round 2 -- at 60 ms a stray brush of the button span up a
         * whole capture session that could only end "Too short". */
        if (board_btn_rec()) {
            t_last_input = now;
            if (t_rec_down_since < 0) t_rec_down_since = now;
            else if (now - t_rec_down_since >= GEST_REC_HOLD_MS) {
                s_linger_rec_press_ms = t_rec_down_since;
                ESP_LOGI(TAG, "REC pressed in linger: starting a capture session");
                return WAKE_REC_BUTTON;
            }
        } else {
            t_rec_down_since = -1;
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
            if (t_pwr_down_since >= 0 && now - t_pwr_down_since >= DEV_BTN_DEBOUNCE_MS) {
                /* released short of the 2 s hold: a tap = UI session (the
                 * button is already up, so ui_session's held-at-boot
                 * provisioning probe cannot trigger) */
                ESP_LOGI(TAG, "PWR tapped in linger: starting a UI session");
                return WAKE_PWR_BUTTON;
            }
            t_pwr_down_since = -1;   /* sub-debounce blip: ignore */
        }

        if (now >= t_next_heartbeat) {
            ESP_LOGI(TAG, "awake (dev hold), up %lld min; buttons live; 'sleep' + Enter or hold PWR ~2s to deep-sleep",
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
    return WAKE_COLD;   /* unreachable: board_deep_sleep never returns */
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

    /* Awake watchdog: armed before dispatch, re-armed at phase transitions
     * inside the sessions, cancelled once the session returns (the dev
     * linger has its own idle timeout). A REC wake's first phase includes
     * the recording itself, hence the bigger initial cap. */
    const esp_timer_create_args_t cap_args = { .callback = awake_cap_cb,
                                               .name = "awake_cap" };
    if (esp_timer_create(&cap_args, &s_awake_cap) != ESP_OK) {
        s_awake_cap = NULL;   /* run uncapped rather than not at all */
        ESP_LOGE(TAG, "awake watchdog unavailable");
    }

    /* Dispatch loop (C7 finding C): one pass per session. In the release
     * build the tail deep-sleeps and the loop body runs exactly once; in
     * the dev build the linger can hand back a pseudo wake cause (REC
     * press / PWR tap), and the next pass dispatches it like a fresh
     * button wake -- including re-arming the watchdog. */
    for (;;) {
        awake_cap_arm(wc == WAKE_REC_BUTTON ? AWAKE_CAP_REC_S : AWAKE_CAP_BASE_S);

        if (wc == WAKE_REC_BUTTON)      capture_session(0);   /* fast path first */
        else if (wc == WAKE_PWR_BUTTON) ui_session(wc);
        else                            sync_session(wc);     /* timer + cold */

        awake_cap_cancel();

        /* Common teardown: radio off, codec + rail off, panel asleep. All
         * of these are safe no-ops when the session never brought them up. */
        idf_wifi_stop();
        s_bg_wifi_started = 0;   /* the radio is down: a linger-launched session
                                    must start (and wait on) its own join, not
                                    trust a flag from the stopped one */
        s_wifi_ok = 0;           /* keep the status header's "W" honest */
        audio_deinit();
        if (s_epd_up && !s_panel_lost) {
            epd_sleep();
            s_epd_up = 0;
        }
        ESP_LOGI(TAG, "session done; main task min free stack %u bytes",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));

        unsigned sleep_s = next_sync_interval();
#if HTP_DEV_LINGER
        wc = dev_linger(sleep_s);   /* deep-sleeps (never returns), or hands back
                                       a button press to dispatch as a session */
#else
        ESP_LOGI(TAG, "entering deep sleep for %u s (wake: REC/PWR button or timer)", sleep_s);
        board_deep_sleep(sleep_s);
#endif
    }
}
