#pragma once

#include <stdbool.h>
#include <stddef.h>

/** Which part of the forecast a weather ticket shows. */
typedef enum {
    WEATHER_TODAY = 0,       // morning: today and tonight
    WEATHER_REST_OF_TODAY,   // midday: what is left of today, and tonight
    WEATHER_TOMORROW,        // evening: tomorrow and tomorrow night
} weather_kind_t;

#define WEATHER_TITLE_MAX 64     // room for a ticket title, including the ending 0
#define WEATHER_BODY_MAX  2048   // room for a ticket body, including the ending 0

/**
 * Turn the National Weather Service's answers into the text of a ticket.
 *
 *   forecast_json   the answer of .../gridpoints/XXX/n,n/forecast
 *   alerts_json     the answer of /alerts/active?point=..., or NULL if it could not be fetched
 *   title           gets the big line: "Today", "Rest of today", "Tomorrow",
 *                   or the name of the most serious alert if there is one
 *   body            gets the rest: alerts first, then one short block per part of the day,
 *                   then the credit line. Lines are at most PRINTER_TEXT_COLS wide.
 *
 * Returns false if the forecast cannot be read or has nothing for that day.
 *
 * This file only works with text (no network, no printer), so it can be tried on a computer.
 */
bool weather_ticket_build(weather_kind_t kind, const char *forecast_json, const char *alerts_json,
                          char *title, size_t title_size, char *body, size_t body_size);

/** "today", "rest" or "tomorrow". */
const char *weather_kind_name(weather_kind_t kind);
