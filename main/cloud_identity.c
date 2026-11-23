/*
 * The board's own ID and secret for TikPrint Cloud (see cloud_identity.h).
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs.h"

#include "cloud_identity.h"

static const char *TAG = "cloud_identity";

#define NVS_NS     "cloud"
#define KEY_ID     "dev_id"
#define KEY_SECRET "dev_secret"

static char s_id[CLOUD_ID_MAX];
static char s_secret[CLOUD_SECRET_MAX];

static void to_hex(char *out, const uint8_t *bytes, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        sprintf(out + 2 * i, "%02x", bytes[i]);
    }
}

/*
 * Make a new ID and secret.
 * The ID is "tkp-" + the board's hardware address + 4 random digits. The random part is there so
 * that a board whose saved settings were wiped comes back as a new printer: with the same ID and a
 * new secret the cloud site would refuse it for good (the secret has to match the first one it saw).
 */
static void make_identity(void)
{
    uint8_t mac[6] = { 0 }, extra[2], secret[32];
    esp_efuse_mac_get_default(mac);
    esp_fill_random(extra, sizeof(extra));
    esp_fill_random(secret, sizeof(secret));
    strcpy(s_id, "tkp-");
    to_hex(s_id + 4, mac, sizeof(mac));
    to_hex(s_id + 4 + 12, extra, sizeof(extra));
    to_hex(s_secret, secret, sizeof(secret));
}

esp_err_t cloud_identity_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t id_len = sizeof(s_id), secret_len = sizeof(s_secret);
    bool have = nvs_get_str(h, KEY_ID, s_id, &id_len) == ESP_OK &&
                nvs_get_str(h, KEY_SECRET, s_secret, &secret_len) == ESP_OK &&
                strlen(s_id) >= 8 && strlen(s_secret) >= 32;
    if (!have) {
        make_identity();
        err = nvs_set_str(h, KEY_ID, s_id);
        if (err == ESP_OK) {
            err = nvs_set_str(h, KEY_SECRET, s_secret);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        ESP_LOGI(TAG, "first start: made device ID %s and a secret", s_id);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not save the device ID: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "device ID %s", s_id);   // the secret is never logged
    return ESP_OK;
}

const char *cloud_identity_id(void)
{
    return s_id;
}

const char *cloud_identity_secret(void)
{
    return s_secret;
}
