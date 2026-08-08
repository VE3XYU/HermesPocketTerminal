/* SD-MMC storage (design §2, pinout proven at C1): 1-bit SD-MMC mode,
 * CLK 39, CMD 41, D0 40. Mounts FAT at /sdcard and makes sure /sdcard/rec
 * exists so recordings have a home from the very first boot. */
#include "board.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include <stdbool.h>
#include <sys/stat.h>
#include <errno.h>

#define PIN_SD_CLK 39
#define PIN_SD_CMD 41
#define PIN_SD_D0  40

static const char *TAG = "sdcard";
static sdmmc_card_t *s_card;

static int mount(bool format_if_mount_failed) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;         /* only CLK/CMD/D0 are wired */

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = PIN_SD_CLK;
    slot.cmd = PIN_SD_CMD;
    slot.d0  = PIN_SD_D0;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = format_if_mount_failed,   /* never true unless the operator asked for it */
        .max_files = 8,
        .allocation_unit_size = 0,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        s_card = NULL;   /* a failed mount never writes out_card, but the
                            invariant below must hold unconditionally */
        return -1;
    }

    struct stat st;
    if (stat("/sdcard/rec", &st) != 0) {
        if (mkdir("/sdcard/rec", 0777) != 0 && errno != EEXIST) {
            ESP_LOGE(TAG, "mkdir /sdcard/rec failed: errno=%d", errno);
            /* The mount itself succeeded, so s_card is set -- and
             * board_sd_mount()'s idempotent early-return would hand the
             * NEXT caller (a linger-launched session) a success for a card
             * that has no recordings directory, skipping the provisioning /
             * format offer this failure is supposed to trigger. Undo the
             * mount so s_card keeps meaning exactly "mounted and ready"
             * (same unmount pairing as board_sd_unmount_and_format(), and
             * likewise dropping the pointer whatever the unmount returns). */
            esp_err_t uerr = esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
            if (uerr != ESP_OK)
                ESP_LOGE(TAG, "unmount after mkdir failure: %s", esp_err_to_name(uerr));
            s_card = NULL;
            return -1;
        }
    }
    return 0;
}

/* Idempotent (C7 finding C): sessions launched from the dev linger re-run
 * their init path within one power cycle, and a second
 * esp_vfs_fat_sdmmc_mount() on a live mount fails ESP_ERR_INVALID_STATE.
 * s_card is exactly "mounted": set on mount success, cleared on unmount. */
int board_sd_mount(void) {
    if (s_card) return 0;
    return mount(false);
}

/* Erase-and-reformat path for Task 14's serial-provisioning fallback
 * (operator's card reads RAW / won't mount at all). Only meaningful after
 * board_sd_mount() has already failed once.
 *
 * This is deliberately NOT esp_vfs_fat_sdcard_format(): that API requires
 * a card handle from an *already-mounted* esp_vfs_fat_sdmmc_mount() call —
 * it looks the card up by pointer in FatFs's internal per-mount context
 * table, which is only populated once mount_to_vfs_fat() has succeeded
 * (see esp-idf's fatfs/vfs/vfs_fat_sdmmc.c: on a failed mount, that table
 * entry is never created, out_card is never written, and the SDMMC
 * host/slot/pdrv registration is fully unwound in the failure path). A
 * card that doesn't mount in the first place therefore has no valid
 * handle to hand esp_vfs_fat_sdcard_format() — there's nothing to look up.
 *
 * A second esp_vfs_fat_sdmmc_mount() call with format_if_mount_failed=true
 * reaches the same underlying f_fdisk()+f_mkfs() codepath (partition_card()
 * in the same source file) from exactly the starting state we're in: no
 * mount, corrupt/absent filesystem. Since the first failed mount attempt
 * fully unwinds its host/slot state on the way out, this second call is a
 * clean, independent attempt, not a retry on top of stale state. */
int board_sd_format_and_mount(void) { return mount(true); }

/* Erase-and-reformat path for a card that *is* currently mounted but
 * refuses writes (mount succeeded -- filesystem structures parse well
 * enough for that -- but something underneath is corrupt or failing).
 * Unlike board_sd_format_and_mount(), there's a live mount to tear down
 * first: esp_vfs_fat_sdcard_unmount() is the correct pairing for how this
 * file mounts (esp_vfs_fat_sdmmc_mount(), not sdspi) -- it's the
 * non-deprecated unmount that takes the same base_path + sdmmc_card_t*
 * esp_vfs_fat_sdmmc_mount() handed back via s_card, as opposed to the
 * deprecated esp_vfs_fat_sdmmc_unmount() (no arguments, tracks "the last
 * mounted card" internally). Confirmed in esp-idf's
 * fatfs/vfs/vfs_fat_sdmmc.c: unmount_card_core() (which this reaches)
 * calls call_host_deinit() on the card's host before freeing it -- the
 * same SDMMC host/slot teardown a failed mount unwinds through, so the
 * mount(true) that follows starts from the same clean state
 * board_sd_format_and_mount() relies on, not a second mount stacked on
 * top of a live one. s_card is dropped (set NULL) unconditionally after
 * the unmount attempt: unmount_card_core() frees the sdmmc_card_t on
 * every path that gets past its one early-return (an unregistered pdrv,
 * which can't happen for a card we just successfully mounted), so
 * holding onto the pointer past this point risks a dangling read even if
 * the unmount's own return code was an error. */
int board_sd_unmount_and_format(void) {
    if (s_card) {
        esp_err_t err = esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
        if (err != ESP_OK)
            ESP_LOGE(TAG, "unmount before reformat failed: %s", esp_err_to_name(err));
        s_card = NULL;
    }
    return mount(true);
}
