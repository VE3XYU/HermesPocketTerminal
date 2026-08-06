#include <stdio.h>
#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "htp";

void app_main(void) {
    board_early_init();
    const char *cause[] = { "cold", "rec-button", "pwr-button", "timer" };
    ESP_LOGI(TAG, "HTP terminal bring-up C1, wake=%s", cause[board_wake_cause()]);
    for (int i = 0; i < 100; i++) {            /* 10 s of button echo */
        ESP_LOGI(TAG, "rec=%d pwr=%d", board_btn_rec(), board_btn_pwr());
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "sleeping; press either button to wake (timer in 30 s)");
    board_deep_sleep(30);
}
