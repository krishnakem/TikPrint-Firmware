#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/** The first FW_UPDATE_HEADER_BYTES of a firmware file are enough to tell what it is. */
#define FW_UPDATE_HEADER_BYTES 288

/** Call once at boot. If this firmware has just been installed, it starts the countdown after which it is kept. */
esp_err_t fw_update_init(void);

/** The slot this firmware runs from: "factory", "ota_0" or "ota_1". */
const char *fw_update_slot(void);

/** A number picked at every boot. The page uses it to notice that the board has restarted. */
uint32_t fw_update_boot_id(void);

/** True while freshly installed firmware is still on trial (see fw_update.c). */
bool fw_update_on_trial(void);

/** The largest firmware file that fits in a slot, in bytes. */
size_t fw_update_max_size(void);

/** True if these first bytes of a file are TikPrint firmware for this board. len must be at least FW_UPDATE_HEADER_BYTES. */
bool fw_update_header_ok(const uint8_t *data, size_t len);

/**
 * Receiving a new firmware: begin, write the file in order from its first byte, then finish.
 * finish checks the whole file and, if it is good, tells the bootloader to start it next time.
 * After any error call fw_update_abort().
 *   ESP_ERR_OTA_ROLLBACK_INVALID_STATE (from begin): the running firmware is still on trial
 *   ESP_ERR_OTA_VALIDATE_FAILED (from write/finish): the file is damaged or not firmware
 */
esp_err_t fw_update_begin(void);
esp_err_t fw_update_write(const void *data, size_t len);
esp_err_t fw_update_finish(void);
void fw_update_abort(void);

/** Restart the board in about a second (long enough for the HTTP reply to get out). */
void fw_update_restart_soon(void);
