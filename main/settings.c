/*
 * Settings chosen in the web UI, saved on the board (NVS namespace "settings").
 * For now that is the ticket header: the location text and which parts to show.
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

static SemaphoreHandle_t s_lock;
static settings_t s_now = { .location = "", .show_location = true, .show_date = true, .show_time = true };

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
    nvs_commit(h);
    nvs_close(h);
}

/* Plain text only: no line breaks or other control characters. */
static bool location_ok(const char *text)
{
    if (strlen(text) >= SETTINGS_LOCATION_MAX) {
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
    if (!location_ok(in->location)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = memcmp(&s_now, in, sizeof(s_now)) != 0;
    memset(&s_now, 0, sizeof(s_now));   // so the unused end of the location text always compares equal
    strlcpy(s_now.location, in->location, sizeof(s_now.location));
    s_now.show_location = in->show_location;
    s_now.show_date = in->show_date;
    s_now.show_time = in->show_time;
    settings_t copy = s_now;
    xSemaphoreGive(s_lock);
    if (changed) {
        save(&copy);   // flash is only written when something really changed
        ESP_LOGI(TAG, "ticket header: location \"%s\" %s, date %s, time %s", copy.location,
                 copy.show_location ? "on" : "off", copy.show_date ? "on" : "off", copy.show_time ? "on" : "off");
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
        nvs_close(h);
    }
    return ESP_OK;
}
