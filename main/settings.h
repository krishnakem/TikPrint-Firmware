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

#define SETTINGS_PLACES_MAX     5    // places that can be saved
#define SETTINGS_PLACE_NAME_MAX 48   // room for a place name ("Ann Arbor, MI"), including the ending 0

/** A place weather tickets can be for. Added by ZIP code or by town name. */
typedef struct {
    char name[SETTINGS_PLACE_NAME_MAX];   // "Ann Arbor, MI". "" for a ZIP code until the weather service has named its town
    char zip[SETTINGS_ZIP_MAX];           // "48104", or "" if it was added by town name
    int16_t lat_e2, lon_e2;               // where it is, in 1/100 degree
} settings_place_t;

typedef struct {
    uint8_t count;                        // places saved, 0..SETTINGS_PLACES_MAX
    uint8_t selected;                     // the one weather tickets (and the ticket header) are for right now
    settings_place_t place[SETTINGS_PLACES_MAX];
} settings_places_t;

void settings_get_places(settings_places_t *out);

/** Change the list and remember it across reboots. ESP_ERR_INVALID_ARG if count or selected is out of range. */
esp_err_t settings_set_places(const settings_places_t *in);

/** The selected place. False if no place has been added yet. */
bool settings_get_place(settings_place_t *out);

/** The weather service told us the town of a place added by ZIP code: remember its name. */
void settings_name_place(const settings_place_t *which, const char *name);

/** How weather tickets look. */
typedef struct {
    char font[SETTINGS_FONT_MAX];   // "Inter", ... (one of FONTS in web/index.html); "Printer" = the printer's built-in font
    uint8_t size;                   // 0 = small, 1 = medium, 2 = large (not used by the built-in font)
    bool hours;                     // show the hour-by-hour list
} settings_weather_look_t;

void settings_get_weather_look(settings_weather_look_t *out);

/** ESP_ERR_INVALID_ARG if the size is not 0..2 or the font name is empty or too long. */
esp_err_t settings_set_weather_look(const settings_weather_look_t *in);

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
