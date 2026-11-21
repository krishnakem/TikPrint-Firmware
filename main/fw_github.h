#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Where new firmware comes from: the latest release of this GitHub repository.
 * A release is tagged with its version ("v0.2.0") and carries the firmware file as an asset. */
#define FW_GITHUB_REPO  "krishnakem/TikPrint-Firmware"
#define FW_GITHUB_ASSET "tikprint_firmware.bin"

typedef enum {
    FW_GITHUB_IDLE,          // nothing going on
    FW_GITHUB_CHECKING,      // asking GitHub what the latest release is
    FW_GITHUB_DOWNLOADING,   // fetching the new firmware into the spare slot
    FW_GITHUB_RESTARTING,    // the new firmware is in place; the board is about to restart
} fw_github_state_t;

typedef struct {
    fw_github_state_t state;
    bool checked;           // GitHub has answered at least once since boot
    int checked_s_ago;      // seconds since that answer
    char latest[32];        // version of the latest release, e.g. "0.2.0". "" if unknown or there is none
    bool available;         // the latest release is newer than what is running
    int progress;           // percent, while downloading
    char error[96];         // what went wrong last time, "" if nothing
} fw_github_status_t;

/** Start the background task. It checks by itself once a day, and when asked. */
esp_err_t fw_github_start(void);

void fw_github_get_status(fw_github_status_t *out);
const char *fw_github_state_name(fw_github_state_t state);

/** Ask GitHub now. Returns at once; follow the result with fw_github_get_status(). ESP_ERR_INVALID_STATE if busy. */
esp_err_t fw_github_check_now(void);

/** Download and install the latest release, then restart. Returns at once.
 *  ESP_ERR_INVALID_STATE if busy, ESP_ERR_NOT_FOUND if no newer release is known. */
esp_err_t fw_github_install(void);
