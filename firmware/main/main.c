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
    for (int i = 0; i < 150; i++) {            /* 15 s of button echo; gives the USB
                                                   console time to re-enumerate after a
                                                   sleep-wake before the window closes */
        ESP_LOGI(TAG, "wake=%s rec=%d pwr=%d", cause[board_wake_cause()], board_btn_rec(), board_btn_pwr());
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "sleeping; press either button to wake (timer in 30 s)");
    board_deep_sleep(30);
}
