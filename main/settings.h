#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define SETTINGS_LOCATION_MAX 48   // longest location we keep, in bytes, including the ending 0
#define SETTINGS_FONT_MAX     24   // longest font name we keep, including the ending 0

/**
 * What goes on the header line at the top of every ticket:
 *   TikPrint | LOCATION                NOV 20, 2026 | 10:05 PM EST
 * "TikPrint" is always there. The other three parts can each be switched off.
 */
typedef struct {
    char location[SETTINGS_LOCATION_MAX];   // e.g. "Los Angeles". "" = use the city of the timezone
    bool show_location;
    bool show_date;
    bool show_time;                          // the time includes the timezone's short name
    char header_font[SETTINGS_FONT_MAX];     // font the page draws the header in, e.g. "Inter" (one of FONTS in web/index.html)
} settings_t;

/** Load the saved settings. Call once NVS is up. */
esp_err_t settings_init(void);

void settings_get(settings_t *out);

/** Change the settings and remember them across reboots. ESP_ERR_INVALID_ARG if the location or font name is not plain text. */
esp_err_t settings_set(const settings_t *in);
