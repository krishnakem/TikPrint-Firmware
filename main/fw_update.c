/*
 * Firmware updates over the network ("OTA", over the air).
 *
 * A running program cannot overwrite itself, so the flash has more than one firmware slot
 * (see partitions.csv). A new firmware is written into a slot that is not running, checked,
 * and then the board restarts into it:
 *
 *   1. fw_update_begin()   pick the slot that is not in use
 *   2. fw_update_write()   write the file as it arrives (the flash is erased just ahead of it)
 *   3. fw_update_finish()  check the whole file (its built-in checksum), then leave a note
 *                          for the bootloader: "start that slot next time"
 *   4. restart
 *
 * Safety net ("rollback"): new firmware starts on trial. If it keeps running for
 * TRIAL_S seconds after everything has started, it is kept. If it crashes or restarts before
 * that, the bootloader goes back to the firmware that was running before.
 *
 * Flashing with the USB cable is separate from all this: it writes the "factory" slot and
 * wipes the bootloader's note, so the board starts from "factory" again.
 */
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "fw_update.h"

static const char *TAG = "fw_update";

#define TRIAL_S         15     // new firmware is kept once it has run this long
#define RESTART_DELAY_MS 1200  // pause between "update installed" and the restart

static uint32_t s_boot_id;
static bool s_on_trial;
static const esp_partition_t *s_target;   // the slot being written, NULL when no update is under way
static esp_ota_handle_t s_handle;

const char *fw_update_slot(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p ? p->label : "unknown";
}

uint32_t fw_update_boot_id(void)
{
    return s_boot_id;
}

bool fw_update_on_trial(void)
{
    return s_on_trial;
}

size_t fw_update_max_size(void)
{
    const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
    return p ? p->size : 0;
}

/* A firmware file starts with a small header, then a description of the program:
 * its project name, version and build time. The project name tells TikPrint firmware
 * apart from any other .bin file (a bootloader, another project). */
bool fw_update_header_ok(const uint8_t *data, size_t len)
{
    const size_t desc_at = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    _Static_assert(sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)
                   <= FW_UPDATE_HEADER_BYTES, "FW_UPDATE_HEADER_BYTES is too small");
    if (len < desc_at + sizeof(esp_app_desc_t) || data[0] != ESP_IMAGE_HEADER_MAGIC) {
        return false;
    }
    esp_app_desc_t desc;
    memcpy(&desc, data + desc_at, sizeof(desc));
    if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return false;
    }
    const esp_app_desc_t *mine = esp_app_get_description();
    if (strncmp(desc.project_name, mine->project_name, sizeof(desc.project_name)) != 0) {
        return false;
    }
    ESP_LOGI(TAG, "new firmware: v%.*s, built %.*s %.*s", (int)sizeof(desc.version), desc.version,
             (int)sizeof(desc.date), desc.date, (int)sizeof(desc.time), desc.time);
    return true;
}

esp_err_t fw_update_begin(void)
{
    if (s_target) {
        fw_update_abort();   // a left-over from an upload that broke off
    }
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        ESP_LOGE(TAG, "no free firmware slot: was the new partition table flashed by cable?");
        return ESP_ERR_NOT_FOUND;
    }
    // OTA_WITH_SEQUENTIAL_WRITES: erase the flash bit by bit as the file arrives, instead of
    // erasing the whole 4 MB slot first (which would stall the upload for several seconds).
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not start the update: %s", esp_err_to_name(err));
        return err;
    }
    s_target = target;
    ESP_LOGI(TAG, "running from \"%s\", writing the new firmware to \"%s\"", fw_update_slot(), target->label);
    return ESP_OK;
}

esp_err_t fw_update_write(const void *data, size_t len)
{
    if (!s_target) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_ota_write(s_handle, data, len);
}

esp_err_t fw_update_finish(void)
{
    if (!s_target) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_partition_t *target = s_target;
    s_target = NULL;
    esp_err_t err = esp_ota_end(s_handle);   // checks the whole file; frees the handle either way
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the new firmware did not pass its check: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not select the new firmware: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "new firmware is in \"%s\" and starts at the next restart", target->label);
    return ESP_OK;
}

void fw_update_abort(void)
{
    if (s_target) {
        esp_ota_abort(s_handle);
        s_target = NULL;
        ESP_LOGW(TAG, "update abandoned; the running firmware is untouched");
    }
}

static void restart_cb(void *arg)
{
    ESP_LOGI(TAG, "restarting into the new firmware");
    esp_restart();
}

void fw_update_restart_soon(void)
{
    const esp_timer_create_args_t args = { .callback = restart_cb, .name = "fw_restart" };
    esp_timer_handle_t timer;
    if (esp_timer_create(&args, &timer) != ESP_OK ||
        esp_timer_start_once(timer, (uint64_t)RESTART_DELAY_MS * 1000) != ESP_OK) {
        esp_restart();   // no timer to be had: restart right away rather than not at all
    }
}

/* The trial is over: tell the bootloader to keep this firmware. */
static void keep_cb(void *arg)
{
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        s_on_trial = false;
        ESP_LOGI(TAG, "new firmware has run for %d s: keeping it", TRIAL_S);
    } else {
        ESP_LOGE(TAG, "could not confirm the new firmware: %s", esp_err_to_name(err));
    }
}

esp_err_t fw_update_init(void)
{
    s_boot_id = esp_random();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t *desc = esp_app_get_description();
    ESP_LOGI(TAG, "firmware v%s (%s), built %s %s, running from \"%s\"", desc->version,
             esp_app_get_elf_sha256_str(), desc->date, desc->time, fw_update_slot());

    if (esp_ota_get_last_invalid_partition()) {
        ESP_LOGW(TAG, "an earlier update did not start properly and was undone");
    }

    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_on_trial = true;
        ESP_LOGI(TAG, "this firmware was just installed: on trial for %d s", TRIAL_S);
        const esp_timer_create_args_t args = { .callback = keep_cb, .name = "fw_keep" };
        esp_timer_handle_t timer;
        esp_err_t err = esp_timer_create(&args, &timer);
        if (err == ESP_OK) {
            err = esp_timer_start_once(timer, (uint64_t)TRIAL_S * 1000000);
        }
        return err;
    }
    return ESP_OK;
}
