#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SETTINGS_LOCATION_MAX 48   // longest location we keep, in bytes, including the ending 0
#define SETTINGS_FONT_MAX     24   // longest font name we keep, including the ending 0

/**
 * What goes on the header line at the top of every ticket:
 *   TikPrint | LOCATION                NOV 20, 2026 | 10:05 PM EST
 * "TikPrint" is always there. The other three parts can each be switched off.
 */
typedef struct {
    char location[SETTINGS_LOCATION_MAX];   // e.g. "Los Angeles". "" = the town of the weather ZIP code, else the city of the timezone
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

/* ---- Automations: weather ---- */

#define SETTINGS_ZIP_MAX 6   // a US ZIP code is five digits, plus the ending 0

/** The ZIP code weather tickets are for, e.g. "48104". "" = none chosen yet. */
void settings_get_weather_zip(char out[SETTINGS_ZIP_MAX]);

/** Change it and remember it across reboots. "" clears it.
 *  ESP_ERR_INVALID_ARG if it is neither "" nor five digits. (Whether such a ZIP code exists is for zip_table.h.) */
esp_err_t settings_set_weather_zip(const char *zip);

#define SETTINGS_WEATHER_SLOTS 3   // scheduled weather prints a day

/** When weather tickets print by themselves. Slot 0 prints "today", slot 1 "rest of today", slot 2 "tomorrow". */
typedef struct {
    bool on;                          // the whole schedule; off = nothing prints by itself
    struct {
        uint16_t minute;              // time of day in minutes after midnight, local time (7:30 AM = 450)
        bool on;
    } slot[SETTINGS_WEATHER_SLOTS];
} settings_weather_schedule_t;

void settings_get_weather_schedule(settings_weather_schedule_t *out);

/** Change it and remember it across reboots. ESP_ERR_INVALID_ARG if a time is not within a day. */
esp_err_t settings_set_weather_schedule(const settings_weather_schedule_t *in);
