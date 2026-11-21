#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/** Which part of the forecast a weather ticket shows. */
typedef enum {
    WEATHER_TODAY = 0,       // morning: the next few hours, then today and tonight
    WEATHER_REST_OF_TODAY,   // midday: the next few hours, then what is left of today, and tonight
    WEATHER_TOMORROW,        // evening: tomorrow and tomorrow night
} weather_kind_t;

/** The symbol printed at the top of a weather ticket. The order must match ICONS in tools/make_weather_icons.py. */
typedef enum {
    WEATHER_ICON_NONE = -1,
    WEATHER_ICON_CLEAR_DAY = 0,   // sun
    WEATHER_ICON_CLEAR_NIGHT,     // moon
    WEATHER_ICON_PARTLY_DAY,      // sun behind a cloud
    WEATHER_ICON_PARTLY_NIGHT,    // moon behind a cloud
    WEATHER_ICON_CLOUDY,
    WEATHER_ICON_RAIN,
    WEATHER_ICON_SNOW,
    WEATHER_ICON_THUNDER,
    WEATHER_ICON_FOG,
} weather_icon_t;

/* Marks in the "rich" form of a ticket body (rich = true below). It is for drawing the ticket in a
 * font whose letters have different widths, where padding with spaces cannot line things up:
 *   a line starting with WEATHER_MARK_LABEL is a heading ("NEXT HOURS", "TONIGHT"), drawn bold. After a
 *     tab comes text for the right edge of the same line ("46 F  Precip 49%").
 *   a line starting with WEATHER_MARK_HOUR is one hour: four parts separated by tabs (hour, temperature,
 *     chance of rain, a few words), drawn as columns.
 *   every other line is plain text of any length; whoever draws it breaks it to fit. */
#define WEATHER_MARK_LABEL "\x01"
#define WEATHER_MARK_HOUR  "\x02"

#define WEATHER_TITLE_MAX 64     // room for a ticket title, including the ending 0
#define WEATHER_BODY_MAX  2048   // room for a ticket body, including the ending 0

/**
 * Turn the National Weather Service's answers into the text of a ticket.
 *
 *   forecast_json   the answer of .../gridpoints/XXX/n,n/forecast
 *   hourly_json     the answer of .../gridpoints/XXX/n,n/forecast/hourly, or NULL (none wanted, or not fetched)
 *   alerts_json     the answer of /alerts/active?point=..., or NULL if it could not be fetched
 *   now             the time right now (seconds since 1970), or 0 if the clock is not set.
 *                   Hours and parts of the day that are already over are left out, so the
 *                   ticket is about the moment it prints even if the answers are a little old.
 *   rich            false: lines are at most PRINTER_TEXT_COLS wide and lined up with spaces, for the
 *                   printer's built-in font. true: the rich form described above, for drawing.
 *   title           gets the big line: "Today", "Rest of today", "Tomorrow",
 *                   or the name of the most serious alert if there is one
 *   icon            gets the symbol for the weather the ticket starts with: the coming hour on
 *                   "today" tickets, tomorrow's daytime on the "tomorrow" ticket
 *   body            gets the rest: alerts first, then the next few hours, then one short block
 *                   per part of the day, then the credit line. Lines are at most PRINTER_TEXT_COLS wide.
 *
 * Returns false if the forecast cannot be read or has nothing for that day.
 *
 * This file only works with text (no network, no printer), so it can be tried on a computer.
 */
bool weather_ticket_build(weather_kind_t kind, const char *forecast_json, const char *hourly_json,
                          const char *alerts_json, time_t now, bool rich, weather_icon_t *icon,
                          char *title, size_t title_size, char *body, size_t body_size);

/** "today", "rest" or "tomorrow". */
const char *weather_kind_name(weather_kind_t kind);
