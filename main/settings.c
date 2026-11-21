/*
 * Settings chosen in the web UI, saved on the board (NVS namespace "settings"):
 *   - the ticket header: the location text and which parts to show
 *   - automations: the ZIP code weather tickets are for
 *
 * The web server changes them and the printer reads them, on different tasks,
 * so the copy in memory is guarded by one lock (s_lock).
 */
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "settings.h"

static const char *TAG = "settings";

#define NVS_NS "settings"
#define DEFAULT_HEADER_FONT "Inter"

static SemaphoreHandle_t s_lock;
static settings_t s_now = { .location = "", .show_location = true, .show_date = true, .show_time = true,
                            .header_font = DEFAULT_HEADER_FONT };
static char s_weather_zip[SETTINGS_ZIP_MAX] = "";
/* The weather schedule. Until it is changed: on, at 7:30 AM, 12:30 PM and 8:30 PM. */
static settings_weather_schedule_t s_weather_schedule = {
    .on = true,
    .slot = { { 7 * 60 + 30, true }, { 12 * 60 + 30, true }, { 20 * 60 + 30, true } },
};

static bool schedule_is_valid(const settings_weather_schedule_t *s)
{
    for (int i = 0; i < SETTINGS_WEATHER_SLOTS; i++) {
        if (s->slot[i].minute >= 24 * 60) {
            return false;
        }
    }
    return true;
}

/* Read one on/off setting, leaving *value alone if it was never saved. */
static void load_flag(nvs_handle_t h, const char *key, bool *value)
{
    uint8_t v;
    if (nvs_get_u8(h, key, &v) == ESP_OK) {
        *value = v != 0;
    }
}

static void save(const settings_t *s)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "could not save the settings");
        return;
    }
    nvs_set_str(h, "location", s->location);
    nvs_set_u8(h, "show_loc", s->show_location);
    nvs_set_u8(h, "show_date", s->show_date);
    nvs_set_u8(h, "show_time", s->show_time);
    nvs_set_str(h, "header_font", s->header_font);
    nvs_commit(h);
    nvs_close(h);
}

/* Plain text only, shorter than max bytes: no line breaks or other control characters. */
static bool plain_text(const char *text, size_t max)
{
    if (strlen(text) >= max) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p < 0x20 || *p == 0x7F) {
            return false;
        }
    }
    return true;
}

void settings_get(settings_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_now;
    xSemaphoreGive(s_lock);
}

esp_err_t settings_set(const settings_t *in)
{
    if (!plain_text(in->location, SETTINGS_LOCATION_MAX) || !plain_text(in->header_font, SETTINGS_FONT_MAX) ||
        !in->header_font[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    // A tidy copy (unused bytes all zero), so "did anything change?" is a plain byte comparison.
    settings_t tidy;
    memset(&tidy, 0, sizeof(tidy));
    strlcpy(tidy.location, in->location, sizeof(tidy.location));
    strlcpy(tidy.header_font, in->header_font, sizeof(tidy.header_font));
    tidy.show_location = in->show_location;
    tidy.show_date = in->show_date;
    tidy.show_time = in->show_time;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = memcmp(&s_now, &tidy, sizeof(tidy)) != 0;
    memcpy(&s_now, &tidy, sizeof(tidy));
    xSemaphoreGive(s_lock);
    if (changed) {
        save(&tidy);   // flash is only written when something really changed
        ESP_LOGI(TAG, "ticket header: location \"%s\" %s, date %s, time %s, font %s", tidy.location,
                 tidy.show_location ? "on" : "off", tidy.show_date ? "on" : "off", tidy.show_time ? "on" : "off",
                 tidy.header_font);
    }
    return ESP_OK;
}

esp_err_t settings_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        char location[SETTINGS_LOCATION_MAX];
        size_t n = sizeof(location);
        if (nvs_get_str(h, "location", location, &n) == ESP_OK) {
            strlcpy(s_now.location, location, sizeof(s_now.location));
        }
        load_flag(h, "show_loc", &s_now.show_location);
        load_flag(h, "show_date", &s_now.show_date);
        load_flag(h, "show_time", &s_now.show_time);
        char font[SETTINGS_FONT_MAX];
        n = sizeof(font);
        if (nvs_get_str(h, "header_font", font, &n) == ESP_OK && font[0]) {
            strlcpy(s_now.header_font, font, sizeof(s_now.header_font));
        }
        char zip[SETTINGS_ZIP_MAX];
        n = sizeof(zip);
        if (nvs_get_str(h, "wx_zip", zip, &n) == ESP_OK) {
            strlcpy(s_weather_zip, zip, sizeof(s_weather_zip));
        }
        settings_weather_schedule_t sched;
        n = sizeof(sched);
        if (nvs_get_blob(h, "wx_sched", &sched, &n) == ESP_OK && n == sizeof(sched) && schedule_is_valid(&sched)) {
            s_weather_schedule = sched;
        }
        nvs_close(h);
    }
    return ESP_OK;
}

/* ---------- Automations: weather ---------- */

void settings_get_weather_schedule(settings_weather_schedule_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_weather_schedule;
    xSemaphoreGive(s_lock);
}

esp_err_t settings_set_weather_schedule(const settings_weather_schedule_t *in)
{
    if (!schedule_is_valid(in)) {
        return ESP_ERR_INVALID_ARG;
    }
    settings_weather_schedule_t clean;   // built field by field so the saved block has no stray bytes
    memset(&clean, 0, sizeof(clean));
    clean.on = in->on;
    for (int i = 0; i < SETTINGS_WEATHER_SLOTS; i++) {
        clean.slot[i].minute = in->slot[i].minute;
        clean.slot[i].on = in->slot[i].on;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_weather_schedule = clean;
    xSemaphoreGive(s_lock);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "could not save the weather schedule");
        return ESP_OK;   // it still applies until the next restart
    }
    nvs_set_blob(h, "wx_sched", &clean, sizeof(clean));
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

void settings_get_weather_zip(char out[SETTINGS_ZIP_MAX])
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(out, s_weather_zip, SETTINGS_ZIP_MAX);
    xSemaphoreGive(s_lock);
}

esp_err_t settings_set_weather_zip(const char *zip)
{
    size_t len = strlen(zip);
    if (len != 0 && len != SETTINGS_ZIP_MAX - 1) {
        return ESP_ERR_INVALID_ARG;
    }
    for (const char *p = zip; *p; p++) {
        if (*p < '0' || *p > '9') {
            return ESP_ERR_INVALID_ARG;
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = strcmp(s_weather_zip, zip) != 0;
    strlcpy(s_weather_zip, zip, sizeof(s_weather_zip));
    xSemaphoreGive(s_lock);
    if (changed) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
            ESP_LOGW(TAG, "could not save the weather ZIP code");
            return ESP_OK;   // it still applies until the next restart
        }
        nvs_set_str(h, "wx_zip", zip);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "weather ZIP code is now \"%s\"", zip);
    }
    return ESP_OK;
}
