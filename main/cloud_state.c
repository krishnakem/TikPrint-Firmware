/*
 * The printer's link to TikPrint Cloud, stored in NVS (see cloud_state.h).
 *
 *   namespace "cloud"
 *     "cloud_tok"  the device token (text). Linked = this key is there.
 *     "cloud_pid"  the printer id the cloud gave when linking (text, may be missing)
 *
 * Whether the printer is linked, and its printer id, are also kept in memory, because the page
 * asks every couple of seconds. The token is not: it is read from NVS only when asked for.
 */
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "cloud_state.h"
#include "config.h"

static const char *TAG = "cloud";

#define NVS_NS      "cloud"
#define KEY_TOKEN   "cloud_tok"
#define KEY_PRINTER "cloud_pid"

static SemaphoreHandle_t s_lock;
static bool s_linked;
static char s_printer_id[CLOUD_PRINTER_ID_MAX];

esp_err_t cloud_state_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {   // fails on a printer that was never linked: nothing to read
        size_t n = 0;
        s_linked = nvs_get_str(h, KEY_TOKEN, NULL, &n) == ESP_OK && n > 1;   // n counts the ending 0
        n = sizeof(s_printer_id);
        if (!s_linked || nvs_get_str(h, KEY_PRINTER, s_printer_id, &n) != ESP_OK) {
            s_printer_id[0] = 0;
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "%s", s_linked ? "linked to TikPrint Cloud" : "not linked to TikPrint Cloud");
    return ESP_OK;
}

bool cloud_state_is_linked(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool linked = s_linked;
    xSemaphoreGive(s_lock);
    return linked;
}

esp_err_t cloud_state_save_link(const char *token, const char *printer_id)
{
    if (!printer_id) {
        printer_id = "";
    }
    if (!token || !token[0] || strlen(token) >= CLOUD_TOKEN_MAX || strlen(printer_id) >= CLOUD_PRINTER_ID_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        // The printer id first: if power is lost in between, the printer is simply not linked yet.
        err = nvs_set_str(h, KEY_PRINTER, printer_id);
        if (err == ESP_OK) {
            err = nvs_set_str(h, KEY_TOKEN, token);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err == ESP_OK) {
        s_linked = true;
        strlcpy(s_printer_id, printer_id, sizeof(s_printer_id));
    } else {
        ESP_LOGW(TAG, "could not save the link (%s)", esp_err_to_name(err));
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t cloud_state_clear(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_erase_key(h, KEY_TOKEN);     // "there was none" is fine too
        nvs_erase_key(h, KEY_PRINTER);
        err = nvs_commit(h);
        nvs_close(h);
    }
    if (err == ESP_OK) {
        s_linked = false;
        s_printer_id[0] = 0;
    } else {
        ESP_LOGW(TAG, "could not erase the link (%s)", esp_err_to_name(err));
    }
    xSemaphoreGive(s_lock);
    return err;
}

bool cloud_state_get_token(char *out, size_t size)
{
    bool ok = false;
    if (!out || !size) {
        return false;
    }
    out[0] = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    nvs_handle_t h;
    if (s_linked && nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = size;
        ok = nvs_get_str(h, KEY_TOKEN, out, &n) == ESP_OK;
        nvs_close(h);
    }
    xSemaphoreGive(s_lock);
    if (!ok) {
        out[0] = 0;
    }
    return ok;
}

bool cloud_state_get_printer_id(char *out, size_t size)
{
    if (!out || !size) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool linked = s_linked;
    strlcpy(out, linked ? s_printer_id : "", size);
    xSemaphoreGive(s_lock);
    return linked;
}

const char *cloud_state_get_cloud_url(void)
{
    return CLOUD_URL;
}
