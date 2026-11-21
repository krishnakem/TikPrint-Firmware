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
#include "zip_table.h"

static const char *TAG = "settings";

#define NVS_NS "settings"
#define DEFAULT_HEADER_FONT "Inter"

static SemaphoreHandle_t s_lock;
static settings_t s_now = { .location = "", .show_location = true, .show_date = true, .show_time = true,
                            .header_font = DEFAULT_HEADER_FONT };
static settings_places_t s_places;   // the weather places; none until one is added
static settings_weather_look_t s_weather_look = { .font = DEFAULT_HEADER_FONT, .size = 1, .hours = true };
/* The weather schedule. Until it is changed: on, at 7:30 AM, 12:30 PM and 8:30 PM. */
static settings_weather_schedule_t s_weather_schedule = {
    .on = true,
    .slot = { { 7 * 60 + 30, true }, { 12 * 60 + 30, true }, { 20 * 60 + 30, true } },
};

static bool places_are_valid(const settings_places_t *p)
{
    return p->count <= SETTINGS_PLACES_MAX && (p->count == 0 || p->selected < p->count);
}

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
        settings_places_t places;
        char zip[SETTINGS_ZIP_MAX];
        int lat_e2, lon_e2;
        n = sizeof(places);
        if (nvs_get_blob(h, "wx_places", &places, &n) == ESP_OK && n == sizeof(places) && places_are_valid(&places)) {
            s_places = places;
        } else if ((n = sizeof(zip), nvs_get_str(h, "wx_zip", zip, &n) == ESP_OK) && zip_table_lookup(zip, &lat_e2, &lon_e2)) {
            // Saved by an older firmware, which knew one ZIP code only: it becomes the first place.
            s_places.count = 1;
            strlcpy(s_places.place[0].zip, zip, sizeof(s_places.place[0].zip));
            s_places.place[0].lat_e2 = (int16_t)lat_e2;
            s_places.place[0].lon_e2 = (int16_t)lon_e2;
        }
        settings_weather_look_t look;
        n = sizeof(look);
        if (nvs_get_blob(h, "wx_look", &look, &n) == ESP_OK && n == sizeof(look) && look.size <= 2) {
            look.font[sizeof(look.font) - 1] = 0;
            s_weather_look = look;
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

/* Save a block of settings under a name. If that fails it still applies until the next restart. */
static void save_block(const char *key, const void *block, size_t size)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "could not save \"%s\"", key);
        return;
    }
    nvs_set_blob(h, key, block, size);
    nvs_commit(h);
    nvs_close(h);
}

void settings_get_places(settings_places_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_places;
    xSemaphoreGive(s_lock);
}

esp_err_t settings_set_places(const settings_places_t *in)
{
    if (!places_are_valid(in)) {
        return ESP_ERR_INVALID_ARG;
    }
    settings_places_t clean;   // built field by field so the saved block has no stray bytes
    memset(&clean, 0, sizeof(clean));
    clean.count = in->count;
    clean.selected = in->count ? in->selected : 0;
    for (int i = 0; i < in->count; i++) {
        strlcpy(clean.place[i].name, in->place[i].name, sizeof(clean.place[i].name));
        strlcpy(clean.place[i].zip, in->place[i].zip, sizeof(clean.place[i].zip));
        clean.place[i].lat_e2 = in->place[i].lat_e2;
        clean.place[i].lon_e2 = in->place[i].lon_e2;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_places = clean;
    xSemaphoreGive(s_lock);
    save_block("wx_places", &clean, sizeof(clean));
    return ESP_OK;
}

bool settings_get_place(settings_place_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool any = s_places.count > 0;
    if (any) {
        *out = s_places.place[s_places.selected];
    }
    xSemaphoreGive(s_lock);
    return any;
}

void settings_name_place(const settings_place_t *which, const char *name)
{
    settings_places_t copy;
    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_places.count; i++) {
        settings_place_t *p = &s_places.place[i];
        if (!p->name[0] && p->lat_e2 == which->lat_e2 && p->lon_e2 == which->lon_e2 && strcmp(p->zip, which->zip) == 0) {
            strlcpy(p->name, name, sizeof(p->name));
            changed = true;
        }
    }
    copy = s_places;
    xSemaphoreGive(s_lock);
    if (changed) {
        save_block("wx_places", &copy, sizeof(copy));
    }
}

void settings_get_weather_look(settings_weather_look_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_weather_look;
    xSemaphoreGive(s_lock);
}

esp_err_t settings_set_weather_look(const settings_weather_look_t *in)
{
    if (in->size > 2 || !in->font[0] || strlen(in->font) >= SETTINGS_FONT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    settings_weather_look_t clean;
    memset(&clean, 0, sizeof(clean));
    strlcpy(clean.font, in->font, sizeof(clean.font));
    clean.size = in->size;
    clean.hours = in->hours;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_weather_look = clean;
    xSemaphoreGive(s_lock);
    save_block("wx_look", &clean, sizeof(clean));
    return ESP_OK;
}

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

    save_block("wx_sched", &clean, sizeof(clean));
    return ESP_OK;
}
