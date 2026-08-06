/* Concrete port_storage_t/port_kv_t/port_clock_t/port_rng_t bindings for
 * ESP-IDF (Task 1's app_core/ports.h interfaces). See idf_ports.h for the
 * per-port contract summary. */
#include "idf_ports.h"
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
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "idf_ports";

/* ---------------------------------------------------------------- storage */

#define SD_PREFIX "/sdcard"
#define SD_PATH_MAX 160

/* Logical "/x" -> "/sdcard/x". Every app_core call site passes an
 * absolute logical path (ports.h's contract), so plain concatenation is
 * enough; truncation is treated as a hard failure rather than silently
 * operating on a mangled path. */
static int full_path(const char *logical, char *out, size_t cap) {
    int n = snprintf(out, cap, "%s%s", SD_PREFIX, logical);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static int st_read(void *ctx, const char *path, void *buf, size_t cap, size_t *len) {
    (void)ctx;
    char fp[SD_PATH_MAX];
    if (full_path(path, fp, sizeof fp) != 0) return -1;
    FILE *f = fopen(fp, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap, f);
    int rd_err = ferror(f);
    fclose(f);
    if (rd_err) return -1;
    if (len) *len = n;
    return 0;
}

static int write_whole_file(const char *fp, const void *data, size_t len) {
    FILE *f = fopen(fp, "wb");
    if (!f) return -1;
    size_t n = fwrite(data, 1, len, f);
    int wr_err = ferror(f);
    int close_err = fclose(f);
    return (wr_err || close_err != 0 || n != len) ? -1 : 0;
}

static int st_write(void *ctx, const char *path, const void *data, size_t len) {
    (void)ctx;
    char fp[SD_PATH_MAX];
    char tmp[SD_PATH_MAX + 4];
    if (full_path(path, fp, sizeof fp) != 0) return -1;
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", fp) >= sizeof tmp) return -1;

    if (write_whole_file(tmp, data, len) != 0) { remove(tmp); return -1; }

    if (rename(tmp, fp) != 0) {
        /* FatFs's f_rename() (unlike POSIX rename()) refuses to replace an
         * existing destination — it returns FR_EXIST instead of swapping
         * in the new file. Sidecar writes (sidecar_save) hit this on every
         * re-save of the same capture_id, so a bare rename() would break
         * state persistence after the first write. The tmp file already
         * holds the durable copy at this point, so it's safe to drop the
         * stale target and retry once. */
        remove(fp);
        if (rename(tmp, fp) != 0) { remove(tmp); return -1; }
    }
    return 0;
}

static int st_append(void *ctx, const char *path, const void *data, size_t len) {
    (void)ctx;
    char fp[SD_PATH_MAX];
    if (full_path(path, fp, sizeof fp) != 0) return -1;
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
    if (full_path(path, fp, sizeof fp) != 0) return -1;
    return remove(fp) == 0 ? 0 : -1;
}

static int st_exists(void *ctx, const char *path) {
    (void)ctx;
    char fp[SD_PATH_MAX];
    if (full_path(path, fp, sizeof fp) != 0) return 0;
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
    if (full_path(dir, fp, sizeof fp) != 0) return -1;
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

/* Guards against an unset RTC reporting a bogus "epoch": anything at or
 * before ~2020-09-13 is treated as "no time available" (Task 17 wires the
 * RTC via settimeofday(); until then this always returns 0). */
#define EPOCH_VALID_THRESHOLD 1600000000

static long long ck_epoch_s(void *ctx) {
    (void)ctx;
    time_t now = time(NULL);
    return now > EPOCH_VALID_THRESHOLD ? (long long)now : 0;
}

static unsigned ck_mono_ms(void *ctx) {
    (void)ctx;
    return (unsigned)(esp_timer_get_time() / 1000);
}

static void ck_sleep_ms(void *ctx, unsigned ms) {
    (void)ctx;
    vTaskDelay(pdMS_TO_TICKS(ms));
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
