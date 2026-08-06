/* SD-MMC storage (design §2, pinout proven at C1): 1-bit SD-MMC mode,
 * CLK 39, CMD 41, D0 40. Mounts FAT at /sdcard and makes sure /sdcard/rec
 * exists so recordings have a home from the very first boot. */
#include "board.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include <sys/stat.h>
#include <errno.h>

#define PIN_SD_CLK 39
#define PIN_SD_CMD 41
#define PIN_SD_D0  40

static const char *TAG = "sdcard";
static sdmmc_card_t *s_card;

int board_sd_mount(void) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;         /* only CLK/CMD/D0 are wired */

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = PIN_SD_CLK;
    slot.cmd = PIN_SD_CMD;
    slot.d0  = PIN_SD_D0;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,       /* never silently wipe the operator's card */
        .max_files = 8,
        .allocation_unit_size = 0,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return -1;
    }

    struct stat st;
    if (stat("/sdcard/rec", &st) != 0) {
        if (mkdir("/sdcard/rec", 0777) != 0 && errno != EEXIST) {
            ESP_LOGE(TAG, "mkdir /sdcard/rec failed: errno=%d", errno);
            return -1;
        }
    }
    return 0;
}
