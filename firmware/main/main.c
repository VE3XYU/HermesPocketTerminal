#include <stdio.h>
#include "board.h"
#include "ui_fb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "htp";

void app_main(void) {
    board_early_init();
    const char *cause[] = { "cold", "rec-button", "pwr-button", "timer" };
    ESP_LOGI(TAG, "HTP terminal bring-up C1, wake=%s", cause[board_wake_cause()]);
    ESP_LOGI(TAG, "C2 display test");
    static ui_fb_t fb;
    if (epd_init() != 0) { ESP_LOGE(TAG, "epd_init failed"); board_deep_sleep(0); }
    fb_clear(&fb);
    fb_text(&fb, 20, 20, "HTP C2", 2, 1);
    fb_rect(&fb, 5, 5, 190, 190, 1);
    for (int x = 0; x < 200; x += 10) for (int y = 100; y < 120; y += 4)
        fb_fill(&fb, x, y, 5, 2, 1);
    epd_full(fb.px);
    for (int i = 1; i <= 9; i++) {          /* 9 partials exercise the ghosting rule */
        char n[16]; snprintf(n, sizeof n, "partial %d", i);
        fb_fill(&fb, 20, 60, 160, 20, 0);
        fb_text(&fb, 20, 60, n, 1, 1);
        epd_partial(fb.px);
        vTaskDelay(pdMS_TO_TICKS(800));
    }
    epd_full(fb.px);                        /* ghost-clearing full */
    epd_sleep();
    ESP_LOGI(TAG, "C2 done, sleeping");
    board_deep_sleep(0);
}
