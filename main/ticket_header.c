/*
 * The words of the header line at the top of every ticket (see ticket_header.h).
 * Used by the printer's built-in font tickets and by tickets the board draws itself.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "clock.h"
#include "settings.h"
#include "ticket_header.h"
#include "weather.h"

#define DATE_FORMAT "%b %d, %Y"     // e.g. "Nov 20, 2026" (strftime patterns)
#define TIME_FORMAT "%I:%M %p %Z"   // e.g. "10:05 PM EST"

void ticket_header_text(char *left, char *right)
{
    settings_t s;
    clock_status_t c;
    char date[24], time[24];
    settings_get(&s);
    clock_get_status(&c);
    // No location typed in: use the weather place, or, while there is none,
    // the city of the timezone ("America/Los_Angeles" -> "Los Angeles")
    char city[CLOCK_TZ_MAX] = "";
    weather_get_place(city, sizeof(city));   // "Ann Arbor, MI"
    const char *slash = strrchr(c.timezone, '/');
    if (!city[0] && slash) {
        strlcpy(city, slash + 1, sizeof(city));
        for (char *p = city; *p; p++) {
            *p = *p == '_' ? ' ' : *p;
        }
    }
    const char *location = s.location[0] ? s.location : city;
    bool has_date = s.show_date && clock_format_now(date, sizeof(date), DATE_FORMAT);
    bool has_time = s.show_time && clock_format_now(time, sizeof(time), TIME_FORMAT);

    snprintf(left, TICKET_HEADER_MAX, "TikPrint%s%s", s.show_location && location[0] ? " | " : "",
             s.show_location ? location : "");
    right[0] = 0;
    if (has_date || has_time) {
        snprintf(right, TICKET_HEADER_MAX, "%s%s%s", has_date ? date : time, has_date && has_time ? " | " : "",
                 has_date && has_time ? time : "");
    }
}
