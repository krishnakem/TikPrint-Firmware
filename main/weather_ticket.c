/*
 * The text of a weather ticket, made from the National Weather Service's answers.
 *
 * The ticket prints in the printer's built-in font, which fits PRINTER_TEXT_COLS characters
 * on a line. It is laid out to be read at a glance:
 *
 *   ALERT: Winter Storm Warning                       <- only when there are alerts
 *     until Nov 21, 6:00 PM
 *
 *   TODAY                             38°F  Precip 40%
 *   Light Snow Likely
 *   Wind SW 5 to 10 mph
 *
 *   TONIGHT                           27°F  Precip 20%
 *   Mostly Cloudy
 *   Wind W 5 mph
 *
 *   Weather: National Weather Service
 *
 * The forecast comes as a list of "periods" (Today, Tonight, Sunday, Sunday Night, ...),
 * each with the date it starts on. The first period is always the one happening now, so:
 *   today / rest of today   = the periods that start on the same date as the first one
 *   tomorrow                = the periods that start on the next date in the list
 * By midday the weather service has already dropped the morning and renamed the first
 * period "This Afternoon", so "rest of today" needs no extra work.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#include "printer.h"
#include "weather_ticket.h"

#define COLS        PRINTER_TEXT_COLS
#define MAX_PERIODS 3      // parts of the day on one ticket (before 6 AM "today" is: Overnight, Today, Tonight)
#define MAX_ALERTS  3      // alerts listed by name; any more are only counted
#define DATE_CHARS  10     // the "2026-11-21" at the start of a time like "2026-11-21T06:00:00-05:00"
#define CREDIT      "Weather: National Weather Service"

/* The text being built: where the next character goes, and how much room is left. */
typedef struct {
    char *at;
    size_t left;   // always at least 1: the ending 0
} out_t;

/* Add text, printf style. Text that does not fit is cut off. */
static void put(out_t *o, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int n = vsnprintf(o->at, o->left, format, args);
    va_end(args);
    if (n < 0) {
        return;
    }
    if ((size_t)n >= o->left) {
        n = (int)o->left - 1;
    }
    o->at += n;
    o->left -= (size_t)n;
}

/* How many columns the first `bytes` bytes of text take on paper. Some characters (like the
 * degree sign) are stored as two or three bytes but print as one: only count the first byte of each. */
static int columns(const char *text, size_t bytes)
{
    int n = 0;
    for (size_t i = 0; i < bytes && text[i]; i++) {
        if (((unsigned char)text[i] & 0xC0) != 0x80) {
            n++;
        }
    }
    return n;
}

static bool is_gap(char c)
{
    return (unsigned char)c <= ' ';   // spaces, and any line breaks or tabs in the weather service's text
}

/* Add text, breaking it between words so no line is wider than the paper, and end the line.
 *   used     columns already written on the current line
 *   indent   spaces to start each following line with */
static void put_wrapped(out_t *o, const char *text, int used, int indent)
{
    int line_start = used, col = used;
    const char *p = text;
    for (;;) {
        while (*p && is_gap(*p)) {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *end = p;
        while (*end && !is_gap(*end)) {
            end++;
        }
        int width = columns(p, (size_t)(end - p));
        if (col > line_start && col + 1 + width > COLS) {
            put(o, "\n%*s", indent, "");
            line_start = col = indent;
        }
        if (col > line_start) {
            put(o, " ");
            col++;
        }
        put(o, "%.*s", (int)(end - p), p);
        col += width;
        p = end;
    }
    put(o, "\n");
}

/* The text of obj[key], or "" if it is missing or not text. */
static const char *text_of(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

/* ---------- Alerts ---------- */

/* How serious the weather service says an alert is: 4 = worst. */
static int severity_rank(const char *severity)
{
    static const char *const NAMES[] = { "Minor", "Moderate", "Severe", "Extreme" };
    for (int i = 0; i < 4; i++) {
        if (strcmp(severity, NAMES[i]) == 0) {
            return i + 1;
        }
    }
    return 0;
}

static int two_digits(const char *s)
{
    bool digits = s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9';
    return digits ? (s[0] - '0') * 10 + (s[1] - '0') : -1;
}

/* "2026-11-21T18:00:00-07:00" -> "Nov 21, 6:00 PM". The time is already local to the place
 * the alert is for, so nothing is converted. Returns false if the text is not such a time. */
static bool friendly_time(const char *iso, char *out, size_t size)
{
    static const char *const MONTHS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    if (strlen(iso) < 16) {
        return false;
    }
    int month = two_digits(iso + 5), day = two_digits(iso + 8), hour = two_digits(iso + 11), min = two_digits(iso + 14);
    if (month < 1 || month > 12 || day < 1 || hour < 0 || hour > 23 || min < 0) {
        return false;
    }
    int hour12 = hour % 12 == 0 ? 12 : hour % 12;
    snprintf(out, size, "%s %d, %d:%02d %s", MONTHS[month - 1], day, hour12, min, hour < 12 ? "AM" : "PM");
    return true;
}

/* Write the alerts, most serious first, each kind of alert once. Returns the name of the
 * most serious one (it becomes the ticket's title), or NULL if there are none. */
static const char *put_alerts(out_t *o, const cJSON *alerts)
{
    const cJSON *features = cJSON_GetObjectItem(alerts, "features");
    const char *listed[MAX_ALERTS];
    int count = 0, more = 0;

    for (int rank = 4; rank >= 0; rank--) {
        const cJSON *feature;
        cJSON_ArrayForEach(feature, features) {
            const cJSON *props = cJSON_GetObjectItem(feature, "properties");
            const char *event = text_of(props, "event");
            if (!event[0] || severity_rank(text_of(props, "severity")) != rank) {
                continue;
            }
            bool seen = false;
            for (int i = 0; i < count; i++) {
                seen = seen || strcmp(listed[i], event) == 0;
            }
            if (seen) {
                continue;   // e.g. the same advisory issued for two neighbouring areas
            }
            if (count == MAX_ALERTS) {
                more++;
                continue;
            }
            listed[count++] = event;
            put(o, "ALERT: ");
            put_wrapped(o, event, 7, 7);
            // "ends" is when the weather itself is expected to be over. Some alerts only say when the notice runs out.
            const char *until = text_of(props, "ends");
            char when[32];
            if (friendly_time(until[0] ? until : text_of(props, "expires"), when, sizeof(when))) {
                put(o, "  until %s\n", when);
            }
        }
    }
    if (more > 0) {
        put(o, "+ %d more alert%s\n", more, more == 1 ? "" : "s");
    }
    if (count > 0) {
        put(o, "\n");
    }
    return count > 0 ? listed[0] : NULL;
}

/* ---------- Forecast ---------- */

/* One part of the day:
 *   TODAY                             38°F  Precip 40%
 *   Light Snow Likely
 *   Wind SW 5 to 10 mph                                  */
static void put_period(out_t *o, const cJSON *period)
{
    char name[40], right[40] = "";
    strlcpy(name, text_of(period, "name"), sizeof(name));
    for (char *p = name; *p; p++) {
        *p = (*p >= 'a' && *p <= 'z') ? (char)(*p - 'a' + 'A') : *p;
    }

    const cJSON *temp = cJSON_GetObjectItem(period, "temperature");
    const cJSON *rain = cJSON_GetObjectItem(cJSON_GetObjectItem(period, "probabilityOfPrecipitation"), "value");
    size_t n = 0;
    if (cJSON_IsNumber(temp)) {
        n += (size_t)snprintf(right + n, sizeof(right) - n, "%d\xC2\xB0%.1s", temp->valueint,   // \xC2\xB0 = degree sign
                              text_of(period, "temperatureUnit"));
    }
    if (cJSON_IsNumber(rain) && n < sizeof(right)) {   // the weather service leaves this out when it has no figure
        snprintf(right + n, sizeof(right) - n, "%sPrecip %d%%", n ? "  " : "", rain->valueint);
    }

    // Name on the left, numbers pushed to the right edge. If both do not fit on one line, use two.
    int gap = COLS - columns(name, sizeof(name)) - columns(right, sizeof(right));
    if (!right[0]) {
        put(o, "%s\n", name);
    } else if (gap >= 2) {
        put(o, "%s%*s%s\n", name, gap, "", right);
    } else {
        put(o, "%s\n%s\n", name, right);
    }

    put_wrapped(o, text_of(period, "shortForecast"), 0, 0);

    const char *speed = text_of(period, "windSpeed"), *from = text_of(period, "windDirection");
    if (speed[0]) {
        put(o, "Wind %s%s%s\n", from, from[0] ? " " : "", speed);
    }
    put(o, "\n");
}

const char *weather_kind_name(weather_kind_t kind)
{
    return kind == WEATHER_TOMORROW ? "tomorrow" : kind == WEATHER_REST_OF_TODAY ? "rest" : "today";
}

bool weather_ticket_build(weather_kind_t kind, const char *forecast_json, const char *alerts_json,
                          char *title, size_t title_size, char *body, size_t body_size)
{
    if (title_size == 0 || body_size == 0) {
        return false;
    }
    title[0] = body[0] = 0;
    out_t o = { .at = body, .left = body_size };

    cJSON *forecast = cJSON_Parse(forecast_json);
    cJSON *alerts = alerts_json ? cJSON_Parse(alerts_json) : NULL;
    const cJSON *periods = cJSON_GetObjectItem(cJSON_GetObjectItem(forecast, "properties"), "periods");

    const char *alert = put_alerts(&o, alerts);
    strlcpy(title, alert ? alert : kind == WEATHER_TOMORROW ? "Tomorrow"
                           : kind == WEATHER_REST_OF_TODAY ? "Rest of today" : "Today", title_size);

    // Find the date wanted: today is the date of the first period, tomorrow the next date after it.
    char today[DATE_CHARS + 1] = "", want[DATE_CHARS + 1] = "";
    const cJSON *period;
    cJSON_ArrayForEach(period, periods) {
        const char *start = text_of(period, "startTime");
        if (strlen(start) < DATE_CHARS) {
            continue;
        }
        if (!today[0]) {
            memcpy(today, start, DATE_CHARS);
        }
        if (kind != WEATHER_TOMORROW || strncmp(start, today, DATE_CHARS) != 0) {
            memcpy(want, start, DATE_CHARS);
            break;
        }
    }

    int shown = 0;
    cJSON_ArrayForEach(period, periods) {
        if (want[0] && shown < MAX_PERIODS && strncmp(text_of(period, "startTime"), want, DATE_CHARS) == 0) {
            put_period(&o, period);
            shown++;
        }
    }
    put(&o, CREDIT);

    cJSON_Delete(forecast);   // only now: the title and the alert names pointed into these until here
    cJSON_Delete(alerts);
    return shown > 0;
}
