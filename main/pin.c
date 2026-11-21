/*
 * The PIN that firmware updates can ask for. It is optional: with no PIN saved, anyone who
 * can reach the board may update it.
 *
 * - Saved on the board (NVS namespace "security", key "update_pin"), never in the code.
 * - Never logged and never sent back by the API.
 * - Guessing is slowed down: after PIN_MAX_TRIES wrong tries in a row nothing is accepted
 *   for PIN_LOCK_S seconds. The count is kept in memory, so a reboot clears it.
 * - It guards against people on the same network, not against someone holding the board:
 *   with the USB cable the PIN can be removed, and the board can be flashed anyway.
 *
 * Only the web server task calls pin_check / pin_set / pin_clear, one request at a time,
 * so there is no lock here.
 */
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "pin.h"

static const char *TAG = "pin";

#define NVS_NS        "security"
#define NVS_KEY       "update_pin"
#define PIN_MAX_TRIES 5      // wrong tries in a row before the lock
#define PIN_LOCK_S    300    // how long the lock lasts, in seconds

static char s_pin[PIN_LEN + 1];   // "" = no PIN set
static int s_wrong;               // wrong tries in a row
static int64_t s_locked_until_us; // 0 = not locked

bool pin_is_set(void)
{
    return s_pin[0] != 0;
}

bool pin_format_ok(const char *text)
{
    if (!text || strlen(text) != PIN_LEN) {
        return false;
    }
    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }
    return true;
}

pin_result_t pin_check(const char *pin, int *tries_left, int *retry_s)
{
    if (!pin_is_set()) {
        return PIN_OK;
    }
    int64_t now = esp_timer_get_time();
    if (s_locked_until_us) {
        if (now < s_locked_until_us) {
            if (retry_s) {
                *retry_s = (int)((s_locked_until_us - now) / 1000000) + 1;
            }
            return PIN_LOCKED;
        }
        s_locked_until_us = 0;   // the lock has run out: a fresh set of tries
        s_wrong = 0;
    }

    // Compare every digit even after a mismatch, so the time taken gives nothing away.
    unsigned diff = 0;
    if (pin_format_ok(pin)) {
        for (int i = 0; i < PIN_LEN; i++) {
            diff |= (unsigned)(pin[i] ^ s_pin[i]);
        }
    } else {
        diff = 1;
    }
    if (diff == 0) {
        s_wrong = 0;
        return PIN_OK;
    }

    s_wrong++;
    if (s_wrong >= PIN_MAX_TRIES) {
        s_locked_until_us = now + (int64_t)PIN_LOCK_S * 1000000;
        ESP_LOGW(TAG, "%d wrong PINs in a row: locked for %d s", s_wrong, PIN_LOCK_S);
        if (retry_s) {
            *retry_s = PIN_LOCK_S;
        }
        return PIN_LOCKED;
    }
    ESP_LOGW(TAG, "wrong PIN (%d of %d tries used)", s_wrong, PIN_MAX_TRIES);
    if (tries_left) {
        *tries_left = PIN_MAX_TRIES - s_wrong;
    }
    return PIN_WRONG;
}

esp_err_t pin_set(const char *pin)
{
    if (!pin_format_ok(pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, NVS_KEY, pin);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        strlcpy(s_pin, pin, sizeof(s_pin));
        s_wrong = 0;
        s_locked_until_us = 0;
        ESP_LOGI(TAG, "PIN set: firmware updates now ask for it");   // note: never log the PIN itself
    }
    return err;
}

esp_err_t pin_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;   // there was none to begin with
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        s_pin[0] = 0;
        s_wrong = 0;
        s_locked_until_us = 0;
        ESP_LOGI(TAG, "PIN removed: firmware updates no longer ask for one");
    }
    return err;
}

esp_err_t pin_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        char saved[PIN_LEN + 1] = "";
        size_t n = sizeof(saved);
        if (nvs_get_str(h, NVS_KEY, saved, &n) == ESP_OK && pin_format_ok(saved)) {
            strlcpy(s_pin, saved, sizeof(s_pin));
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "PIN for firmware updates: %s", pin_is_set() ? "on" : "off");
    return ESP_OK;
}
