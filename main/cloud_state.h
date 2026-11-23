#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/*
 * The printer's link to TikPrint Cloud, kept in NVS.
 *
 * A printer is "linked" when it has a device token saved. The token is what the cloud hands
 * the printer when the two are paired; it is the printer's password for the cloud, so it never
 * leaves the board except in requests to the cloud itself (never in /api/..., never in a log).
 *
 * This file only stores and reads. It makes no network calls: a printer that is not linked
 * never talks to the cloud because of it.
 */

#define CLOUD_TOKEN_MAX      512   // longest device token, with its ending 0
#define CLOUD_PRINTER_ID_MAX 64    // longest printer id, with its ending 0

/** Read the link from NVS. Call once at startup, after nvs_flash_init(). */
esp_err_t cloud_state_init(void);

/** True when a device token is saved. */
bool cloud_state_is_linked(void);

/** Save a link: the device token and the printer id the cloud gave. printer_id may be "" or
 *  NULL. Refuses (ESP_ERR_INVALID_ARG) an empty token or one that is too long. */
esp_err_t cloud_state_save_link(const char *token, const char *printer_id);

/** Forget the link: erases the token and the printer id. */
esp_err_t cloud_state_clear(void);

/** Copy the device token into out. False (and out = "") when not linked or out is too small.
 *  Only for requests to the cloud. */
bool cloud_state_get_token(char *out, size_t size);

/** Copy the printer id into out ("" when there is none). False when not linked. */
bool cloud_state_get_printer_id(char *out, size_t size);

/** Where TikPrint Cloud lives, with no "/" at the end (CLOUD_URL in config.h). */
const char *cloud_state_get_cloud_url(void);
