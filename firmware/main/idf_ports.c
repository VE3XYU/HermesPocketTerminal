/* Concrete port_storage_t/port_kv_t/port_clock_t/port_rng_t bindings for
 * ESP-IDF (Task 1's app_core/ports.h interfaces). See idf_ports.h for the
 * per-port contract summary. */
#include "idf_ports.h"
#include "board.h"       /* board_rtc_get(): external RTC feeds epoch_s */
#include "tick_ms.h"
#include "esp_vfs_fat.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

static const char *TAG = "idf_ports";

/* ---------------------------------------------------------------- storage */

#define SD_PREFIX "/sdcard"
#define SD_PATH_MAX IDF_SD_PATH_MAX

/* Logical "/x" -> "/sdcard/x". Every app_core call site passes an
 * absolute logical path (ports.h's contract), so plain concatenation is
 * enough; truncation is treated as a hard failure rather than silently
 * operating on a mangled path. Exported (idf_ports.h) so idf_transport.c's
 * streaming file access uses the same rule rather than a second copy. */
int idf_ports_sd_path(const char *logical, char *out, size_t cap) {
    int n = snprintf(out, cap, "%s%s", SD_PREFIX, logical);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/* Diagnostic side channel for st_write() failures -- see idf_ports.h.
 * Single static instance: main-task-only, single-threaded, and only ever
 * meaningful for the caller's *immediately preceding* write() call, so
 * there's nothing to gain from anything fancier than "last one wins". */
static idf_write_fail_t s_last_write_fail = { "unknown", 0 };

static void note_write_fail(const char *step) {
    s_last_write_fail.step = step;
    s_last_write_fail.err = errno;
    ESP_LOGE(TAG, "write failed: step=%s errno=%d", step, errno);
}

void idf_ports_last_write_fail(idf_write_fail_t *out) {
    if (out) *out = s_last_write_fail;
}

/* RECOVERY CONTRACT for the tmp-file-then-rename write pattern below.
 *
 * FatFs's f_rename() (unlike POSIX rename()) refuses to replace an
 * existing destination — it returns FR_EXIST instead of swapping in the
 * new file. Sidecar writes (sidecar_save) hit this on every re-save of the
 * same capture_id, so a bare rename() would break state persistence after
 * the first write. rename_replacing() works around this by removing the
 * stale target and retrying once — but that means there is a window
 * (between the remove() and the retry rename()) where power loss leaves
 * only "<path>.tmp" on disk, with "<path>" gone.
 *
 * We never treat that as data loss: st_write() leaves "<path>.tmp" in
 * place on any rename failure (it does NOT remove(tmp) — the tmp file is
 * the one surviving durable copy at that point, written and fclose()'d
 * before any rename was attempted). st_read() then falls back to
 * "<path>.tmp" whenever "<path>" can't be read, and promotes it back to
 * "<path>" so the recovery is permanent (one promotion, not a fallback on
 * every subsequent read).
 *
 * Reachable on-disk states for a given logical path, and what st_read()
 * does in each:
 *   - fp only (no write in flight, or a prior write fully promoted):
 *     read fp directly.
 *   - fp (old, untouched) + tmp (new, durable) — reachable if a crash or
 *     failure lands anywhere from write_whole_file()'s success up through
 *     a failed first rename() inside rename_replacing(), before remove(fp)
 *     runs: fp is still readable, so st_read() returns the OLD value
 *     (equivalent to the write never having been attempted) without even
 *     looking at tmp. The orphaned tmp is harmless: the next st_write() to
 *     this path reopens and overwrites it in "wb" mode.
 *   - tmp only, fp gone — reachable once remove(fp) has run but the
 *     replacement rename() hasn't landed (crash in that window, or the
 *     retry itself fails): st_read() falls back to tmp, returns the NEW
 *     value, and promotes tmp to fp so later reads skip the fallback.
 *   - neither: path was never written (or was explicitly removed); a
 *     genuine miss.
 * fp and tmp are never both "new" data — tmp only outlives a successful
 * promotion when fp still holds the un-replaced old value.
 */
static int rename_replacing(const char *tmp, const char *fp) {
    if (rename(tmp, fp) == 0) return 0;
    /* This first failure is the routine/expected FR_EXIST case (see
     * above) -- not diagnostic-worthy on its own, only the final one is. */
    remove(fp);
    if (rename(tmp, fp) == 0) return 0;
    note_write_fail("rename");
    return -1;
}

static int read_whole_file(const char *fp, void *buf, size_t cap, size_t *len) {
    FILE *f = fopen(fp, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap, f);
    int rd_err = ferror(f);
    fclose(f);
    if (rd_err) return -1;
    if (len) *len = n;
    return 0;
}

static int st_read(void *ctx, const char *path, void *buf, size_t cap, size_t *len) {
    (void)ctx;
    char fp[SD_PATH_MAX];
    if (idf_ports_sd_path(path, fp, sizeof fp) != 0) return -1;

    if (read_whole_file(fp, buf, cap, len) == 0) return 0;

    /* "<path>" is missing or unreadable. Per the recovery contract above,
     * an orphaned "<path>.tmp" may hold the durable copy of the last
     * write that never got promoted — try it before giving up. */
    char tmp[SD_PATH_MAX + 4];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", fp) >= sizeof tmp) return -1;
    if (read_whole_file(tmp, buf, cap, len) != 0) return -1;   /* genuinely missing */

    ESP_LOGW(TAG, "recovered orphaned tmp file for %s", path);
    if (rename_replacing(tmp, fp) != 0)
        ESP_LOGW(TAG, "promotion of %s failed; will retry recovery on next read", path);
    /* Promotion is best-effort tidying, not required for this read to
     * succeed — the data is already in buf either way. */
    return 0;
}

static int write_whole_file(const char *fp, const void *data, size_t len) {
    FILE *f = fopen(fp, "wb");
    if (!f) { note_write_fail("fopen"); return -1; }

    size_t n = fwrite(data, 1, len, f);
    int wr_err = ferror(f);
    if (wr_err || n != len) {
        note_write_fail((n != len && !wr_err) ? "fwrite short" : "fwrite");
        fclose(f);   /* best-effort; already failing, nothing more to report */
        return -1;
    }

    if (fclose(f) != 0) { note_write_fail("fclose"); return -1; }
    return 0;
}

static int st_write(void *ctx, const char *path, const void *data, size_t len) {
    (void)ctx;
    char fp[SD_PATH_MAX];
    char tmp[SD_PATH_MAX + 4];
    if (idf_ports_sd_path(path, fp, sizeof fp) != 0) { note_write_fail("path"); return -1; }
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", fp) >= sizeof tmp) { note_write_fail("path"); return -1; }

    if (write_whole_file(tmp, data, len) != 0) { remove(tmp); return -1; }   /* step already noted */

    /* tmp now holds the one durable copy of this write. From here on,
     * never remove(tmp) on a failure path — only a successful promotion
     * retires it. See the recovery contract above. */
    if (rename_replacing(tmp, fp) != 0) return -1;   /* tmp intentionally left in place; step already noted */
    return 0;
}

static int st_append(void *ctx, const char *path, const void *data, size_t len) {
    (void)ctx;
    char fp[SD_PATH_MAX];
    if (idf_ports_sd_path(path, fp, sizeof fp) != 0) return -1;
    FILE *f = fopen(fp, "ab");
    if (!f) return -1;
    size_t n = fwrite(data, 1, len, f);
    int wr_err = ferror(f);
    int close_err = fclose(f);
    return (wr_err || close_err != 0 || n != len) ? -1 : 0;
}

static int st_remove(void *ctx, const char *path) {
    (void)ctx;
    char fp[SD_PATH_MAX];
    if (idf_ports_sd_path(path, fp, sizeof fp) != 0) return -1;
    return remove(fp) == 0 ? 0 : -1;
}

static int st_exists(void *ctx, const char *path) {
    (void)ctx;
    char fp[SD_PATH_MAX];
    if (idf_ports_sd_path(path, fp, sizeof fp) != 0) return 0;
    struct stat s;
    return stat(fp, &s) == 0;
}

static long long st_free_bytes(void *ctx) {
    (void)ctx;
    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info(SD_PREFIX, &total, &free_b) != ESP_OK) return -1;
    return (long long)free_b;
}

static int st_list(void *ctx, const char *dir, int (*cb)(const char *name, void *u), void *u) {
    (void)ctx;
    char fp[SD_PATH_MAX];
    if (idf_ports_sd_path(dir, fp, sizeof fp) != 0) return -1;
    DIR *d = opendir(fp);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (cb(e->d_name, u)) break;   /* nonzero from cb stops enumeration early */
    }
    closedir(d);
    return 0;
}

/* --------------------------------------------------------------------- kv */

#define NVS_NAMESPACE "htp"

static int ensure_nvs_ready(void) {
    static bool inited;
    if (inited) return 0;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Partition needs erasing (first boot / layout change); the data
         * inside is not user-recoverable in either case. */
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return -1;
    }
    inited = true;
    return 0;
}

static int kv_get(void *ctx, const char *key, char *buf, size_t cap) {
    (void)ctx;
    if (ensure_nvs_ready() != 0) return -1;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return -1;
    size_t len = cap;
    esp_err_t err = nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    return err == ESP_OK ? 0 : -1;
}

static int kv_set(void *ctx, const char *key, const char *val) {
    (void)ctx;
    if (ensure_nvs_ready() != 0) return -1;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t err = nvs_set_str(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK ? 0 : -1;
}

/* ------------------------------------------------------------------- clock */

/* Guards against an unset clock reporting a bogus "epoch": anything at or
 * before ~2020-09-13 is treated as "no time available". */
#define EPOCH_VALID_THRESHOLD 1600000000

/* Task 17: real time comes from the PCF85063 (board_rtc_get()). The system
 * clock is preferred when it already holds a valid time -- it survives deep
 * sleep on the S3 and is refreshed by board_rtc_set() on server drift
 * correction -- so the external RTC costs one I2C read per cold boot, after
 * which the value is cached into the system clock via settimeofday(). */
static long long ck_epoch_s(void *ctx) {
    (void)ctx;
    time_t now = time(NULL);
    if (now > EPOCH_VALID_THRESHOLD) return (long long)now;
    long long rtc = board_rtc_get();
    if (rtc > EPOCH_VALID_THRESHOLD) {
        struct timeval tv = { .tv_sec = (time_t)rtc, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "system clock set from the external RTC: %lld", rtc);
        return rtc;
    }
    return 0;
}

static unsigned ck_mono_ms(void *ctx) {
    (void)ctx;
    return (unsigned)(esp_timer_get_time() / 1000);
}

/* board_delay_ms(), not vTaskDelay(pdMS_TO_TICKS(ms)): the latter truncates
 * towards zero, so at CONFIG_FREERTOS_HZ=100 every request from 1-9 ms
 * became vTaskDelay(0) -- a bare yield. app_core's retry/backoff paths call
 * sleep_ms() with exactly those small values, and a sleep that doesn't
 * sleep turns a paced retry loop into a spin. Same truncation class as the
 * SSD1681 BUSY poll that blocked checkpoint C2; see tick_ms.h. */
static void ck_sleep_ms(void *ctx, unsigned ms) {
    (void)ctx;
    board_delay_ms(ms);
}

/* --------------------------------------------------------------------- rng */

static void rng_fill(void *ctx, uint8_t *buf, size_t n) {
    (void)ctx;
    esp_fill_random(buf, n);
}

/* ------------------------------------------------------------------- setup */

void idf_ports_init(port_storage_t *st, port_kv_t *kv, port_clock_t *ck, port_rng_t *rng) {
    st->ctx = NULL;
    st->read = st_read;
    st->write = st_write;
    st->append = st_append;
    st->remove = st_remove;
    st->exists = st_exists;
    st->free_bytes = st_free_bytes;
    st->list = st_list;

    kv->ctx = NULL;
    kv->get = kv_get;
    kv->set = kv_set;

    ck->ctx = NULL;
    ck->epoch_s = ck_epoch_s;
    ck->mono_ms = ck_mono_ms;
    ck->sleep_ms = ck_sleep_ms;

    rng->ctx = NULL;
    rng->fill = rng_fill;
}
