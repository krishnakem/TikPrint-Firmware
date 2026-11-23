#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Where new firmware comes from: the latest release of this GitHub repository.
 * A release is tagged with its version ("v0.2.0") and carries the firmware file as an asset.
 *
 * Beta builds are releases too, marked "pre-release" on GitHub and tagged like "v0.4.0-beta.7".
 * They are built from the "beta" branch: work that is being tried out before it is merged into main.
 * The board only looks at them when "Beta updates" is switched on in Settings. */
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
    char latest[32];        // version of the latest release, e.g. "0.2.0" or "0.4.0-beta.7". "" if unknown or there is none
    bool available;         // the latest release is newer than what is running
    bool beta;              // beta builds are offered too (the "Beta updates" switch)
    int progress;           // percent, while downloading
    char error[96];         // what went wrong last time, "" if nothing
} fw_github_status_t;

/** Start the background task. It checks by itself once a day, and when asked. */
esp_err_t fw_github_start(void);

void fw_github_get_status(fw_github_status_t *out);
const char *fw_github_state_name(fw_github_state_t state);

/** Ask GitHub now. Returns at once; follow the result with fw_github_get_status(). ESP_ERR_INVALID_STATE if busy. */
esp_err_t fw_github_check_now(void);

/** Switch beta builds on or off, remember it, and ask GitHub again straight away.
 *  Returns at once. ESP_ERR_INVALID_STATE if busy (nothing is changed then). */
esp_err_t fw_github_set_beta(bool on);

/** Download and install the latest release, then restart. Returns at once.
 *  ESP_ERR_INVALID_STATE if busy, ESP_ERR_NOT_FOUND if no newer release is known. */
esp_err_t fw_github_install(void);

/**
 * Download and install one named release, then restart: what "Update" on the TikPrint Cloud website
 * asks for (see cloud_update.c). Returns at once.
 *
 * Only a version number is taken, e.g. "0.4.2" or "0.4.2-beta.7". NEVER AN ADDRESS: the address is
 * built here, from FW_GITHUB_REPO and FW_GITHUB_ASSET, so the file can only come from that repository.
 *   ESP_ERR_INVALID_ARG      not a version number
 *   ESP_ERR_INVALID_VERSION  not newer than what is running (the board never goes back)
 *   ESP_ERR_INVALID_STATE    busy with a check or another install: ask again in a moment
 */
esp_err_t fw_github_install_version(const char *version);

/* How an install is going, for whoever asked for it to be told. */
typedef enum {
    FW_GITHUB_STEP_INSTALLING,   // the whole file has arrived: it is being checked and made the next to start
    FW_GITHUB_STEP_RESTARTING,   // checked and in place: the board restarts in a moment
    FW_GITHUB_STEP_FAILED,       // given up, nothing was changed: `error` says why
} fw_github_step_t;

typedef void (*fw_github_listener_t)(fw_github_step_t step, const char *error);

/** Have `listener` called at each step of every install (on the updater's task; it may take a
 *  few seconds over it, the install waits). One listener; call before fw_github_start(). */
void fw_github_set_listener(fw_github_listener_t listener);
