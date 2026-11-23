#pragma once

#include <stdbool.h>
#include "esp_err.h"

/*
 * The board's own ID and secret for TikPrint Cloud, saved on the board (NVS namespace "cloud",
 * next to the link kept by cloud_state.c).
 *
 * They are how the board proves to the cloud site that it is this printer
 * and no other (see docs/device-api.md in the TikPrint Cloud repo). They are made once, the first
 * time the board starts, and kept for good. Making them calls nobody: a printer that is not
 * linked still never talks to the cloud.
 *
 * The secret must never be logged, shown on the page or sent anywhere but the cloud site.
 */

#define CLOUD_ID_MAX     24   // "tkp-" + 16 hex digits, plus the ending 0
#define CLOUD_SECRET_MAX 65   // 32 random bytes as 64 hex digits, plus the ending 0

/** Load the ID and secret, making them first if the board has none. Call after Wi-Fi has
 *  started: the board's random numbers are only truly random while the radio is on. */
esp_err_t cloud_identity_init(void);

/** "tkp-3fa9c2d17b40a1c2": sent as X-Device-Id. */
const char *cloud_identity_id(void);

/** Sent as X-Device-Secret. Never log it. */
const char *cloud_identity_secret(void);
