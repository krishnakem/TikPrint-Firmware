#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#include "weather_ticket.h"

#define WEATHER_SOURCE_MAX 80   // room for a ticket's source line ("WEATHER · ANN ARBOR, MI"), including the ending 0
#define WEATHER_PLACE_MAX  48   // room for a place name ("Ann Arbor, MI"), including the ending 0

/** How getting the weather went. */
typedef enum {
    WEATHER_OK = 0,
    WEATHER_NO_ZIP,        // no place has been added yet
    WEATHER_NO_WIFI,       // the board is not on Wi-Fi, so it cannot reach the weather service
    WEATHER_UNREACHABLE,   // the weather service did not answer, or answered with an error
    WEATHER_REFUSED,       // it answered "403 Access Denied": it does not accept this app's requests (see APP_CONTACT)
    WEATHER_BAD_ANSWER,    // it answered, but not with a forecast we can read
} weather_result_t;

/** Load what was saved about the ZIP code's location. Call once NVS is up. */
esp_err_t weather_init(void);

/**
 * Make the text of a weather ticket for the saved ZIP code. Talks to the National Weather
 * Service (api.weather.gov) unless a fresh enough answer is still in memory, so it can take
 * several seconds and needs a deep stack: call it from the automations task only.
 *
 *   show_hours    include the hour-by-hour list (on "today" and "rest of today" tickets)
 *   source        gets "WEATHER · ANN ARBOR, MI"   (room for WEATHER_SOURCE_MAX)
 *   title, body   see weather_ticket.h             (room for WEATHER_TITLE_MAX / WEATHER_BODY_MAX)
 *   rich_body     NULL, or gets the body once more in the "rich" form for drawing it in a chosen font
 *                 (weather_ticket.h; room for WEATHER_BODY_MAX)
 *   icon          gets the symbol to print at the top (see weather_icons.h), or WEATHER_ICON_NONE
 */
weather_result_t weather_make_ticket(weather_kind_t kind, bool show_hours, char *source, char *title, char *body,
                                     char *rich_body, weather_icon_t *icon);

/**
 * Find out the forecast address (and for a ZIP code, the town) of the selected place, if not known yet.
 * Same rules as above: automations task only.
 */
weather_result_t weather_locate(void);

/** The selected place, e.g. "Ann Arbor, MI". "" if there is none, or for a ZIP code whose town is not known yet.
 *  Also the ticket header's location when none is typed in. */
void weather_get_place(char *out, size_t size);

const char *weather_result_name(weather_result_t result);      // "ok", "no_zip", "no_wifi", "unreachable", "refused", "bad_answer"
const char *weather_result_message(weather_result_t result);   // the same, as a sentence for people
