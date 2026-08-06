#include <stdio.h>
#include <string.h>
#include "board.h"
#include "ui_fb.h"
#include "ports.h"
#include "idf_ports.h"
#include "app_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "htp";

static int log_rec_entry(const char *name, void *u) {
    int *count = u;
    ESP_LOGI(TAG, "  /rec/%s", name);
    (*count)++;
    return 0;   /* keep enumerating */
}

void app_main(void) {
    board_early_init();
    const char *cause[] = { "cold", "rec-button", "pwr-button", "timer" };
    ESP_LOGI(TAG, "HTP terminal bring-up C1, wake=%s", cause[board_wake_cause()]);

    ESP_LOGI(TAG, "C3 SD/NVS/config test");
    if (board_sd_mount() != 0) { ESP_LOGE(TAG, "SD mount failed"); board_deep_sleep(0); }

    port_storage_t st; port_kv_t kv; port_clock_t ck; port_rng_t rng;
    idf_ports_init(&st, &kv, &ck, &rng);

    char buf[2048]; size_t len;
    app_config_t cfg;
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
