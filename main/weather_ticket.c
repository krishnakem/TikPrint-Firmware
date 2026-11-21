/*
 * The text of a weather ticket, made from the National Weather Service's answers.
 *
 * The ticket prints in the printer's built-in font, which fits PRINTER_TEXT_COLS characters
 * on a line. It is laid out to be read at a glance:
 *
 *   ALERT: Winter Storm Warning                       <- only when there are alerts
 *     until Nov 21, 6:00 PM
 *
 *   NEXT HOURS                                        <- not on the "tomorrow" ticket
 *    1 PM   38°   40%  Light Snow Likely
 *    2 PM   37°   45%  Light Snow Likely
 *    ...
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
 * each with the time it starts and ends. Periods that are already over are skipped (the
 * answer may be up to a few hours old), so the first one left is the one happening now:
 *   today / rest of today   = the periods that start on the same date as that one
 *   tomorrow                = the periods that start on the next date in the list
 * By midday the weather service has renamed the first period "This Afternoon", so
 * "rest of today" needs no extra work.
 *
 * The hourly forecast is the same kind of list with one period per hour, a week of them.
 * Only the first few are needed, so only those are read (see first_periods).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "printer.h"
#include "weather_ticket.h"

#define COLS        PRINTER_TEXT_COLS
#define MAX_PERIODS 3      // parts of the day on one ticket (before 6 AM "today" is: Overnight, Today, Tonight)
#define MAX_ALERTS  3      // alerts listed by name; any more are only counted
#define DATE_CHARS  10     // the "2026-11-21" at the start of a time like "2026-11-21T06:00:00-05:00"
#define HOURS_SHOWN 6      // rows in the "next hours" list
#define HOURS_READ  12     // hourly periods read to find them: some may already be over if the answer is a little old
#define HOUR_TEXT_COLS (COLS - 20)   // room left for the words on an hourly row
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

/* ---------- Times ---------- */

/* "2026-11-21T18:00:00-07:00" -> seconds since 1970. Returns 0 if the text is not such a time. */
static time_t parse_time(const char *iso)
{
    int year, month, day, hour, min, sec, off_h, off_m;
    char sign;
    if (sscanf(iso, "%4d-%2d-%2dT%2d:%2d:%2d%c%2d:%2d", &year, &month, &day, &hour, &min, &sec, &sign, &off_h, &off_m) != 9 ||
        (sign != '+' && sign != '-') || month < 1 || month > 12) {
        return 0;
    }
    // Days from 1 Jan 1970 to this date (a well-known calendar formula; it treats March as the first month).
    int y = month <= 2 ? year - 1 : year;
    int era = y / 400, yoe = y - era * 400;
    int doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = (long long)era * 146097 + doe - 719468;
    long long local = days * 86400 + hour * 3600 + min * 60 + sec;
    int offset = (off_h * 3600 + off_m * 60) * (sign == '-' ? -1 : 1);
    return (time_t)(local - offset);
}

/* Is this period already over? Never, if the clock is not set or the period does not say when it ends. */
static bool is_over(const cJSON *period, time_t now)
{
    time_t end = parse_time(text_of(period, "endTime"));
    return now > 0 && end > 0 && end <= now;
}

/* ---------- The symbol ---------- */

static bool has(const char *text, const char *word)
{
    return strstr(text, word) != NULL;
}

/* Which symbol suits a period (an hour or a part of the day).
 *
 * Each period comes with the address of a little picture, e.g.
 *   https://api.weather.gov/icons/land/day/ovc/tsra,60?size=medium
 * We do not fetch the picture, but its address names the weather in fixed short codes
 * ("ovc" = overcast, "tsra" = thunderstorm and rain, ...), which is more dependable than
 * reading the forecast's wording. With two codes (first and second half of the period) the
 * more serious one wins. Without such an address, the wording is used after all. */
static weather_icon_t icon_for(const cJSON *period)
{
    bool night = cJSON_IsFalse(cJSON_GetObjectItem(period, "isDaytime"));
    const char *url = text_of(period, "icon");
    const char *codes = strstr(url, night ? "/night/" : "/day/");
    char text[96];
    if (codes) {
        strlcpy(text, codes, sizeof(text));
    } else {
        // No codes: use the wording, lower-cased so it can be searched the same way.
        strlcpy(text, text_of(period, "shortForecast"), sizeof(text));
        for (char *p = text; *p; p++) {
            *p = (*p >= 'A' && *p <= 'Z') ? (char)(*p - 'A' + 'a') : *p;
        }
        if (!text[0]) {
            return WEATHER_ICON_NONE;
        }
    }
    if (has(text, "tsra") || has(text, "thunder") || has(text, "tornado") || has(text, "hurricane") || has(text, "tropical")) {
        return WEATHER_ICON_THUNDER;
    }
    if (has(text, "snow") || has(text, "sleet") || has(text, "blizzard") || has(text, "flurr")) {
        return WEATHER_ICON_SNOW;
    }
    if (has(text, "rain") || has(text, "fzra") || has(text, "shower") || has(text, "drizzle")) {
        return WEATHER_ICON_RAIN;
    }
    if (has(text, "fog") || has(text, "haze") || has(text, "smoke") || has(text, "dust")) {
        return WEATHER_ICON_FOG;
    }
    if (has(text, "ovc") || has(text, "overcast") || (has(text, "cloudy") && !has(text, "partly") && !has(text, "mostly"))) {
        return WEATHER_ICON_CLOUDY;
    }
    if (has(text, "sct") || has(text, "bkn") || has(text, "cloud") || has(text, "partly") || has(text, "mostly sunny") ||
        has(text, "mostly clear")) {
        return night ? WEATHER_ICON_PARTLY_NIGHT : WEATHER_ICON_PARTLY_DAY;
    }
    return night ? WEATHER_ICON_CLEAR_NIGHT : WEATHER_ICON_CLEAR_DAY;
}

/* ---------- The next few hours ---------- */

/* Read only the first `max` periods of a forecast, as a list. The hourly forecast is about
 * 160 KB of text for a whole week; reading all of it would take a lot of memory for nothing.
 * So this walks the text to the end of the max-th period and reads just that much.
 * Returns NULL if there are none. cJSON_Delete() the result. */
static cJSON *first_periods(const char *json, int max)
{
    const char *list = strstr(json, "\"periods\"");
    list = list ? strchr(list, '[') : NULL;
    if (!list) {
        return NULL;
    }
    const char *last_end = NULL;   // the "}" that closes the last whole period seen
    int depth = 0, found = 0;
    bool in_text = false;
    for (const char *p = list + 1; *p && found < max; p++) {
        if (in_text) {
            if (*p == '\\' && p[1]) {
                p++;                      // an escaped character inside quotes: skip it
            } else if (*p == '"') {
                in_text = false;
            }
        } else if (*p == '"') {
            in_text = true;
        } else if (*p == '{') {
            depth++;
        } else if (*p == '}') {
            if (--depth == 0) {
                last_end = p;
                found++;
            }
        } else if (*p == ']' && depth == 0) {
            break;                        // the list ended before max periods
        }
    }
    if (!last_end) {
        return NULL;
    }
    size_t n = (size_t)(last_end - list) + 1;
    char *copy = malloc(n + 2);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, list, n);
    copy[n] = ']';
    copy[n + 1] = 0;
    cJSON *periods = cJSON_Parse(copy);
    free(copy);
    return periods;
}

/* The list of the coming hours:
 *   NEXT HOURS
 *    1 PM   38°   40%  Light Snow Likely        (hour, temperature, chance of rain or snow, a few words) */
static void put_hours(out_t *o, const char *hourly_json, time_t now, weather_icon_t *icon)
{
    cJSON *hours = first_periods(hourly_json, HOURS_READ);
    int shown = 0;
    const cJSON *hour;
    cJSON_ArrayForEach(hour, hours) {
        const char *start = text_of(hour, "startTime");
        int h = strlen(start) >= 13 ? two_digits(start + 11) : -1;
        const cJSON *temp = cJSON_GetObjectItem(hour, "temperature");
        if (shown == HOURS_SHOWN || h < 0 || h > 23 || !cJSON_IsNumber(temp) || is_over(hour, now)) {
            continue;
        }
        if (shown++ == 0) {
            put(o, "NEXT HOURS\n");
            *icon = icon_for(hour);   // the ticket's symbol is the weather of the coming hour
        }
        const cJSON *rain = cJSON_GetObjectItem(cJSON_GetObjectItem(hour, "probabilityOfPrecipitation"), "value");
        char chance[8] = "";
        if (cJSON_IsNumber(rain)) {
            snprintf(chance, sizeof(chance), "%d%%", rain->valueint);
        }
        // Cut the words to the room left, so every hour stays on one line.
        const char *words = text_of(hour, "shortForecast");
        int bytes = 0, cols = 0;
        while (words[bytes] && (cols < HOUR_TEXT_COLS || ((unsigned char)words[bytes] & 0xC0) == 0x80)) {
            cols += ((unsigned char)words[bytes] & 0xC0) != 0x80;
            bytes++;
        }
        put(o, "%2d %s  %3d\xC2\xB0  %4s  %.*s\n", h % 12 == 0 ? 12 : h % 12, h < 12 ? "AM" : "PM", temp->valueint,
            chance, bytes, words);
    }
    if (shown > 0) {
        put(o, "\n");
    }
    cJSON_Delete(hours);
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

bool weather_ticket_build(weather_kind_t kind, const char *forecast_json, const char *hourly_json,
                          const char *alerts_json, time_t now, weather_icon_t *icon,
                          char *title, size_t title_size, char *body, size_t body_size)
{
    *icon = WEATHER_ICON_NONE;
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

    if (hourly_json && kind != WEATHER_TOMORROW) {
        put_hours(&o, hourly_json, now, icon);
    }

    // Find the date wanted: today is the date of the first period that is not over yet,
    // tomorrow the next date after it.
    char today[DATE_CHARS + 1] = "", want[DATE_CHARS + 1] = "";
    const cJSON *period;
    cJSON_ArrayForEach(period, periods) {
        const char *start = text_of(period, "startTime");
        if (strlen(start) < DATE_CHARS || is_over(period, now)) {
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
        if (want[0] && shown < MAX_PERIODS && !is_over(period, now) &&
            strncmp(text_of(period, "startTime"), want, DATE_CHARS) == 0) {
            if (*icon == WEATHER_ICON_NONE) {
                *icon = icon_for(period);   // no hourly list: the symbol of the first part of the day shown
            }
            put_period(&o, period);
            shown++;
        }
    }
    put(&o, CREDIT);

    cJSON_Delete(forecast);   // only now: the title and the alert names pointed into these until here
    cJSON_Delete(alerts);
    return shown > 0;
}
