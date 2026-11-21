/*
 * Weather from the US National Weather Service (https://api.weather.gov).
 * Free, no account and no key. US only. The board asks it directly: no TikPrint server is involved.
 *
 * Getting a forecast takes three questions:
 *
 *   1. GET /points/42.28,-83.74             "which forecast square is this spot in?"
 *        -> the address of that square's forecast, and the name of the nearest town
 *   2. GET /gridpoints/DTX/47,35/forecast   the forecast itself (the address from step 1)
 *   3. GET /alerts/active?point=42.28,-83.74   warnings in force for that spot
 *
 * The spot comes from the ZIP code typed in the web UI, looked up in the table built into
 * the firmware (zip_table.c).
 *
 * Being a good guest, as the weather service asks:
 *   - Every request says who is asking (USER_AGENT_FORMAT, with the contact from config.h).
 *   - Answers are kept and reused instead of asked for again:
 *       step 1  saved on the board (NVS namespace "weather"). It practically never changes,
 *               so it is only asked again when the ZIP code changes or step 2 fails.
 *       step 2  kept in memory for as long as the answer itself says it is good for
 *               (its "Cache-Control: max-age=..." line, usually an hour).
 *       step 3  the same, but never longer than ALERTS_KEEP_MAX_S: warnings must be current.
 *   - Scheduled prints wait a random 0-60 seconds first (SPREAD_MAX_MS), so that thousands of
 *     printers do not all ask at exactly 7:30:00.
 *
 * All requests go over HTTPS and the weather service's certificate is checked.
 *
 * Everything here runs on the automations task, except weather_get_place(), which the web
 * server calls. What those two share (s_point) is guarded by s_lock.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "config.h"
#include "settings.h"
#include "weather.h"
#include "wifi_mgr.h"
#include "zip_table.h"

static const char *TAG = "weather";

/* ---- Settings ---- */
#define NWS_HOST            "https://api.weather.gov"
#define USER_AGENT_FORMAT   "TikPrint/%s (%s)"     // firmware version, APP_CONTACT
#define HTTP_TIMEOUT_MS     15000
#define SPREAD_MAX_MS       60000                  // scheduled prints wait a random time up to this before asking
#define POINT_MAX_BYTES     (16 * 1024)            // biggest answer accepted for each question (usual size: 4 KB,
#define FORECAST_MAX_BYTES  (96 * 1024)            //   13 KB, and 2-4 KB per alert). Answers are held in PSRAM.
#define ALERTS_MAX_BYTES    (256 * 1024)
#define FORECAST_KEEP_DEFAULT_S (30 * 60)          // how long to reuse a forecast that does not say how long it is good for
#define FORECAST_KEEP_MIN_S     (5 * 60)           // ... and the limits on what it may say
#define FORECAST_KEEP_MAX_S     (3 * 60 * 60)
#define ALERTS_KEEP_DEFAULT_S   (5 * 60)
#define ALERTS_KEEP_MIN_S       60
#define ALERTS_KEEP_MAX_S       (15 * 60)
#define NVS_NS              "weather"
#define URL_MAX             160

/* What step 1 found out about a ZIP code. Saved on the board as one block. */
typedef struct {
    char zip[SETTINGS_ZIP_MAX];
    char forecast_url[URL_MAX];
    char place[WEATHER_PLACE_MAX];
} point_t;

/* An answer kept for reuse. */
typedef struct {
    char *json;            // NULL = nothing kept
    char key[URL_MAX];     // the address it was the answer to
    int64_t good_until_us; // board uptime after which it must be asked for again
} kept_t;

static SemaphoreHandle_t s_lock;   // guards s_point
static point_t s_point;
static kept_t s_forecast, s_alerts;
static char s_user_agent[96];

/* ---------- One HTTPS request ---------- */

typedef struct {
    char *buf;       // the answer's body, as text
    size_t len, max;
    bool too_big;
    int max_age_s;   // from the answer's "Cache-Control: max-age=N" line, -1 if it has none
} reply_t;

/* Called by the HTTP client as pieces of the answer arrive. */
static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    reply_t *r = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        const char *max_age = strcasecmp(evt->header_key, "Cache-Control") == 0 ? strstr(evt->header_value, "max-age=") : NULL;
        if (max_age) {
            r->max_age_s = atoi(max_age + strlen("max-age="));
        }
    } else if (evt->event_id == HTTP_EVENT_ON_DATA) {
        // Only keep the body of a real answer (200), not of a "moved to..." or an error page.
        if (esp_http_client_get_status_code(evt->client) != 200) {
            return ESP_OK;
        }
        if (r->len + (size_t)evt->data_len > r->max) {
            r->too_big = true;
            return ESP_OK;
        }
        memcpy(r->buf + r->len, evt->data, (size_t)evt->data_len);
        r->len += (size_t)evt->data_len;
    }
    return ESP_OK;
}

/* GET a weather service address. Returns the HTTP status (200 = fine), or -1 if it could not
 * be reached. On 200, *json is the answer as text (free() it when done) and *max_age_s says
 * how many seconds the answer may be reused (-1 = it did not say). */
static int nws_get(const char *url, size_t max_bytes, char **json, int *max_age_s)
{
    *json = NULL;
    reply_t reply = { .max = max_bytes, .max_age_s = -1 };
    reply.buf = heap_caps_malloc(max_bytes + 1, MALLOC_CAP_SPIRAM);
    if (!reply.buf) {
        return -1;
    }
    const esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,   // check the weather service's certificate
        .timeout_ms = HTTP_TIMEOUT_MS,
        .user_agent = s_user_agent,
        .event_handler = on_http_event,
        .user_data = &reply,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(reply.buf);
        return -1;
    }
    esp_http_client_set_header(client, "Accept", "application/geo+json");
    int64_t started_us = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(client);   // also follows "moved to..." answers
    int status = err == ESP_OK ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);

    int took_ms = (int)((esp_timer_get_time() - started_us) / 1000);
    if (status == 200 && !reply.too_big) {
        reply.buf[reply.len] = 0;
        char *fitted = heap_caps_realloc(reply.buf, reply.len + 1, MALLOC_CAP_SPIRAM);   // give back the unused room
        *json = fitted ? fitted : reply.buf;
        *max_age_s = reply.max_age_s;
        ESP_LOGI(TAG, "GET %s: %u bytes in %d ms, good for %d s", url, (unsigned)reply.len, took_ms, reply.max_age_s);
        return 200;
    }
    free(reply.buf);
    if (status == 200) {
        ESP_LOGW(TAG, "GET %s: the answer is bigger than %u bytes", url, (unsigned)max_bytes);
        return -1;
    }
    ESP_LOGW(TAG, "GET %s failed: %s (after %d ms)", url, status < 0 ? esp_err_to_name(err) : "HTTP error", took_ms);
    if (status > 0) {
        ESP_LOGW(TAG, "  HTTP status %d", status);
    }
    return status;
}

/* ---------- Kept answers ---------- */

static bool kept_is_fresh(const kept_t *k, const char *key)
{
    return k->json && strcmp(k->key, key) == 0 && esp_timer_get_time() < k->good_until_us;
}

static void kept_drop(kept_t *k)
{
    free(k->json);
    k->json = NULL;
}

/* Keep an answer (takes over json) for as long as it said, within the limits given. */
static void kept_store(kept_t *k, const char *key, char *json, int max_age_s, int default_s, int min_s, int max_s)
{
    int keep_s = max_age_s < 0 ? default_s : max_age_s < min_s ? min_s : max_age_s > max_s ? max_s : max_age_s;
    kept_drop(k);
    k->json = json;
    strlcpy(k->key, key, sizeof(k->key));
    k->good_until_us = esp_timer_get_time() + (int64_t)keep_s * 1000000;
}

/* ---------- Step 1: where is the ZIP code? ---------- */

/* "42.28,-83.74" from 4228 and -8374. Trailing zeros are left off ("39.7", not "39.70"):
 * the weather service wants it that way and would otherwise send us round to ask again. */
static void format_spot(char *out, size_t size, int lat_e2, int lon_e2)
{
    int v[2] = { lat_e2, lon_e2 };
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; i < 2 && n < size; i++) {
        int whole = abs(v[i]) / 100, part = abs(v[i]) % 100;
        n += (size_t)snprintf(out + n, size - n, "%s%s%d", i ? "," : "", v[i] < 0 ? "-" : "", whole);
        if (part % 10 && n < size) {
            n += (size_t)snprintf(out + n, size - n, ".%02d", part);
        } else if (part && n < size) {
            n += (size_t)snprintf(out + n, size - n, ".%d", part / 10);
        }
    }
}

static void point_save(const point_t *p)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "could not save the forecast address");
        return;
    }
    nvs_set_blob(h, "point", p, sizeof(*p));
    nvs_commit(h);
    nvs_close(h);
}

static bool point_known(const char *zip)
{
    return s_point.forecast_url[0] && strcmp(s_point.zip, zip) == 0;   // only this task changes s_point: no lock to read
}

/* Ask the weather service which forecast square the ZIP code is in, and save the answer. */
static weather_result_t point_fetch(const char *zip, const char *spot)
{
    char url[URL_MAX], *json;
    int max_age_s;
    snprintf(url, sizeof(url), NWS_HOST "/points/%s", spot);
    if (nws_get(url, POINT_MAX_BYTES, &json, &max_age_s) != 200) {
        return WEATHER_UNREACHABLE;
    }
    cJSON *root = cJSON_Parse(json);
    free(json);
    const cJSON *props = cJSON_GetObjectItem(root, "properties");
    const cJSON *forecast = cJSON_GetObjectItem(props, "forecast");
    const cJSON *near = cJSON_GetObjectItem(cJSON_GetObjectItem(props, "relativeLocation"), "properties");
    const cJSON *city = cJSON_GetObjectItem(near, "city"), *state = cJSON_GetObjectItem(near, "state");

    // The address must be the weather service's own, and fit.
    bool ok = cJSON_IsString(forecast) && strncmp(forecast->valuestring, NWS_HOST "/", strlen(NWS_HOST "/")) == 0 &&
              strlen(forecast->valuestring) < URL_MAX;
    if (ok) {
        point_t p;
        memset(&p, 0, sizeof(p));
        strlcpy(p.zip, zip, sizeof(p.zip));
        strlcpy(p.forecast_url, forecast->valuestring, sizeof(p.forecast_url));
        if (cJSON_IsString(city) && cJSON_IsString(state)) {
            snprintf(p.place, sizeof(p.place), "%s, %s", city->valuestring, state->valuestring);
        } else {
            strlcpy(p.place, zip, sizeof(p.place));
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_point = p;
        xSemaphoreGive(s_lock);
        point_save(&p);
        ESP_LOGI(TAG, "ZIP code %s is %s; forecast at %s", zip, p.place, p.forecast_url);
    } else {
        ESP_LOGW(TAG, "the answer for ZIP code %s has no forecast address", zip);
    }
    cJSON_Delete(root);
    return ok ? WEATHER_OK : WEATHER_BAD_ANSWER;
}

/* ---------- Putting it together ---------- */

static bool wifi_up(void)
{
    wifi_mgr_status_t w;
    wifi_mgr_get_status(&w);
    return w.connected;
}

/* The saved ZIP code and its spot on the map ("42.28,-83.74"). False if there is none. */
static bool saved_spot(char zip[SETTINGS_ZIP_MAX], char *spot, size_t spot_size)
{
    int lat_e2, lon_e2;
    settings_get_weather_zip(zip);
    if (!zip_table_lookup(zip, &lat_e2, &lon_e2)) {
        return false;
    }
    format_spot(spot, spot_size, lat_e2, lon_e2);
    return true;
}

weather_result_t weather_locate(void)
{
    char zip[SETTINGS_ZIP_MAX], spot[32];
    if (!saved_spot(zip, spot, sizeof(spot))) {
        return WEATHER_NO_ZIP;
    }
    if (point_known(zip)) {
        return WEATHER_OK;
    }
    return wifi_up() ? point_fetch(zip, spot) : WEATHER_NO_WIFI;
}

weather_result_t weather_make_ticket(weather_kind_t kind, bool spread_load, char *source, char *title, char *body)
{
    char zip[SETTINGS_ZIP_MAX], spot[32], alerts_url[URL_MAX];
    source[0] = title[0] = body[0] = 0;
    if (!saved_spot(zip, spot, sizeof(spot))) {
        return WEATHER_NO_ZIP;
    }
    snprintf(alerts_url, sizeof(alerts_url), NWS_HOST "/alerts/active?point=%s", spot);

    // Is there anything to ask the weather service, or is everything still fresh in memory?
    bool must_ask = !point_known(zip) || !kept_is_fresh(&s_forecast, s_point.forecast_url) ||
                    !kept_is_fresh(&s_alerts, alerts_url);
    if (must_ask && !wifi_up()) {
        return WEATHER_NO_WIFI;
    }
    if (must_ask && spread_load) {
        uint32_t wait_ms = esp_random() % (SPREAD_MAX_MS + 1);
        ESP_LOGI(TAG, "waiting %u s before asking the weather service (spreads the load)", (unsigned)(wait_ms / 1000));
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }

    // Step 1 (normally already known)
    bool point_is_new = false;
    if (!point_known(zip)) {
        weather_result_t res = point_fetch(zip, spot);
        if (res != WEATHER_OK) {
            return res;
        }
        point_is_new = true;
    }

    // Step 2. If the saved forecast address has stopped working, ask step 1 again, once.
    for (int attempt = 0; !kept_is_fresh(&s_forecast, s_point.forecast_url); attempt++) {
        char *json;
        int max_age_s;
        int status = nws_get(s_point.forecast_url, FORECAST_MAX_BYTES, &json, &max_age_s);
        if (status == 200) {
            kept_store(&s_forecast, s_point.forecast_url, json, max_age_s, FORECAST_KEEP_DEFAULT_S,
                       FORECAST_KEEP_MIN_S, FORECAST_KEEP_MAX_S);
            break;
        }
        if (status < 0 || point_is_new || attempt > 0) {
            return WEATHER_UNREACHABLE;
        }
        ESP_LOGW(TAG, "the saved forecast address failed: looking it up again");
        weather_result_t res = point_fetch(zip, spot);
        if (res != WEATHER_OK) {
            return res;
        }
        point_is_new = true;
    }

    // Step 3. If the alerts cannot be had, the forecast still prints (without them).
    if (!kept_is_fresh(&s_alerts, alerts_url)) {
        char *json;
        int max_age_s;
        kept_drop(&s_alerts);
        if (nws_get(alerts_url, ALERTS_MAX_BYTES, &json, &max_age_s) == 200) {
            kept_store(&s_alerts, alerts_url, json, max_age_s, ALERTS_KEEP_DEFAULT_S, ALERTS_KEEP_MIN_S, ALERTS_KEEP_MAX_S);
        } else {
            ESP_LOGW(TAG, "no answer about alerts: printing the forecast without them");
        }
    }

    if (!weather_ticket_build(kind, s_forecast.json, s_alerts.json, title, WEATHER_TITLE_MAX, body, WEATHER_BODY_MAX)) {
        ESP_LOGW(TAG, "the forecast could not be read");
        kept_drop(&s_forecast);   // do not reuse an answer we cannot read
        return WEATHER_BAD_ANSWER;
    }
    snprintf(source, WEATHER_SOURCE_MAX, "WEATHER \xC2\xB7 %s", s_point.place);   // \xC2\xB7 = the middle dot
    for (char *p = source; *p; p++) {
        *p = (*p >= 'a' && *p <= 'z') ? (char)(*p - 'a' + 'A') : *p;
    }
    return WEATHER_OK;
}

void weather_get_place(char *out, size_t size)
{
    char zip[SETTINGS_ZIP_MAX];
    settings_get_weather_zip(zip);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(out, zip[0] && strcmp(s_point.zip, zip) == 0 ? s_point.place : "", size);
    xSemaphoreGive(s_lock);
}

const char *weather_result_name(weather_result_t result)
{
    switch (result) {
    case WEATHER_OK:          return "ok";
    case WEATHER_NO_ZIP:      return "no_zip";
    case WEATHER_NO_WIFI:     return "no_wifi";
    case WEATHER_UNREACHABLE: return "unreachable";
    default:                  return "bad_answer";
    }
}

const char *weather_result_message(weather_result_t result)
{
    switch (result) {
    case WEATHER_OK:          return "";
    case WEATHER_NO_ZIP:      return "Set a ZIP code for the weather first";
    case WEATHER_NO_WIFI:     return "The printer is not on Wi-Fi, so it cannot get the weather";
    case WEATHER_UNREACHABLE: return "The National Weather Service did not answer. Try again in a minute";
    default:                  return "The National Weather Service sent a forecast the printer could not read";
    }
}

esp_err_t weather_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(s_user_agent, sizeof(s_user_agent), USER_AGENT_FORMAT, esp_app_get_description()->version, APP_CONTACT);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        point_t p;
        size_t n = sizeof(p);
        if (nvs_get_blob(h, "point", &p, &n) == ESP_OK && n == sizeof(p)) {
            p.zip[sizeof(p.zip) - 1] = p.forecast_url[sizeof(p.forecast_url) - 1] = p.place[sizeof(p.place) - 1] = 0;
            s_point = p;
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "weather ready: %d ZIP codes in the table, asking as \"%s\"", zip_table_count(), s_user_agent);
    return ESP_OK;
}
