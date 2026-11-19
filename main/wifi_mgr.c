/*
 * Wi-Fi manager (station mode: the board joins an existing network).
 *
 * - Credentials live in NVS (namespace "wifi", keys "ssid" / "pass") and are
 *   written only after a successful join, so a typo never replaces a working
 *   network.
 * - On boot, and whenever the connection drops, the board keeps retrying the
 *   saved network in the background.
 */
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "nvs.h"

#include "wifi_mgr.h"
#include "net_name.h"

static const char *TAG = "wifi";

#define NVS_NS "wifi"
#define BIT_GOT_IP BIT0
#define BIT_FAILED BIT1
#define JOIN_TIMEOUT_MS 20000
#define JOIN_MAX_TRIES 3
#define RETRY_DELAY_US (5 * 1000 * 1000)

static esp_netif_t *s_sta;
static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_lock;        // one scan/join/forget at a time
static esp_timer_handle_t s_retry_timer;

static volatile bool s_connected;
static volatile bool s_joining;         // a user-requested join is in progress
static volatile bool s_scanning;
static volatile int s_join_tries;
static volatile uint8_t s_last_reason;
static bool s_has_saved;
static char s_saved_ssid[33];
static char s_saved_pass[65];
static char s_ip[16];

/* ---------- NVS ---------- */
static void creds_load(void)
{
    nvs_handle_t h;
    s_has_saved = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t n = sizeof(s_saved_ssid);
    if (nvs_get_str(h, "ssid", s_saved_ssid, &n) == ESP_OK && s_saved_ssid[0]) {
        n = sizeof(s_saved_pass);
        if (nvs_get_str(h, "pass", s_saved_pass, &n) != ESP_OK) {
            s_saved_pass[0] = 0;
        }
        s_has_saved = true;
    }
    nvs_close(h);
}

static void creds_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", ssid);
        nvs_set_str(h, "pass", pass);
        nvs_commit(h);
        nvs_close(h);
    }
    strlcpy(s_saved_ssid, ssid, sizeof(s_saved_ssid));
    strlcpy(s_saved_pass, pass, sizeof(s_saved_pass));
    s_has_saved = true;
}

static void creds_erase(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    s_has_saved = false;
    s_saved_ssid[0] = 0;
    memset(s_saved_pass, 0, sizeof(s_saved_pass));
}

/* ---------- helpers ---------- */
static void apply_config(const char *ssid, const char *pass)
{
    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;       // pick the strongest access point
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

static bool reason_is_wrong_password(uint8_t r)
{
    return r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           r == WIFI_REASON_HANDSHAKE_TIMEOUT || r == WIFI_REASON_MIC_FAILURE;
}

static bool reason_is_not_found(uint8_t r)
{
    return r == WIFI_REASON_NO_AP_FOUND || r == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
           r == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD || r == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD;
}

static void retry_timer_cb(void *arg)
{
    if (s_has_saved && !s_connected && !s_joining && !s_scanning) {
        esp_wifi_connect();
    }
}

static void schedule_retry(void)
{
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, RETRY_DELAY_US);
}

/* ---------- events ---------- */
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        bool was_connected = s_connected;
        s_connected = false;
        s_ip[0] = 0;
        s_last_reason = d->reason;

        if (s_joining) {
            // Decide whether this join attempt is over or worth another try.
            if (d->reason == WIFI_REASON_ASSOC_LEAVE) {
                return;  // our own disconnect from the previous network
            }
            ESP_LOGW(TAG, "join attempt %d failed (reason %d)", s_join_tries + 1, d->reason);
            if (reason_is_wrong_password(d->reason) || ++s_join_tries >= JOIN_MAX_TRIES) {
                xEventGroupSetBits(s_events, BIT_FAILED);
            } else {
                esp_wifi_connect();
            }
            return;
        }
        if (was_connected) {
            ESP_LOGW(TAG, "disconnected (reason %d)", d->reason);
        }
        if (s_has_saved && !s_scanning) {
            schedule_retry();  // keep trying the saved network in the background
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        ESP_LOGI(TAG, "connected, IP %s -> http://" NET_HOSTNAME ".local", s_ip);
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

/* ---------- public API ---------- */
esp_err_t wifi_mgr_start(void)
{
    s_events = xEventGroupCreate();
    s_lock = xSemaphoreCreateMutex();
    const esp_timer_create_args_t targs = { .callback = retry_timer_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));

    s_sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta, NET_HOSTNAME);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));  // we manage NVS ourselves (see top of file)
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    creds_load();
    if (s_has_saved) {
        apply_config(s_saved_ssid, s_saved_pass);
    }
    ESP_ERROR_CHECK(esp_wifi_start());

    if (s_has_saved) {
        ESP_LOGI(TAG, "saved network \"%s\": connecting", s_saved_ssid);
        esp_wifi_connect();
    } else {
        ESP_LOGI(TAG, "no saved network: set up Wi-Fi from the web UI");
    }
    return ESP_OK;
}

void wifi_mgr_get_status(wifi_mgr_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->has_saved = s_has_saved;
    strlcpy(out->ssid, s_saved_ssid, sizeof(out->ssid));
    if (s_connected) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out->connected = true;
            out->rssi = ap.rssi;
            strlcpy(out->ssid, (const char *)ap.ssid, sizeof(out->ssid));
            strlcpy(out->ip, s_ip, sizeof(out->ip));
        }
    }
}

static int by_rssi_desc(const void *a, const void *b)
{
    return ((const wifi_mgr_net_t *)b)->rssi - ((const wifi_mgr_net_t *)a)->rssi;
}

esp_err_t wifi_mgr_scan(wifi_mgr_net_t *out, size_t max, size_t *count)
{
    *count = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_scanning = true;
    esp_timer_stop(s_retry_timer);
    if (!s_connected) {
        esp_wifi_disconnect();  // a scan can't run while a connection attempt is in flight
    }

    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err == ESP_OK) {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        if (n > 40) {
            n = 40;
        }
        wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
        if (recs && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
            for (int i = 0; i < n; i++) {
                const char *ssid = (const char *)recs[i].ssid;
                if (!ssid[0]) {
                    continue;  // hidden network
                }
                // The same name can come from several access points: keep the strongest.
                size_t j;
                for (j = 0; j < *count; j++) {
                    if (strcmp(out[j].ssid, ssid) == 0) {
                        break;
                    }
                }
                if (j < *count) {
                    if (recs[i].rssi > out[j].rssi) {
                        out[j].rssi = recs[i].rssi;
                    }
                    continue;
                }
                if (*count >= max) {
                    continue;
                }
                wifi_auth_mode_t a = recs[i].authmode;
                wifi_mgr_net_t *o = &out[(*count)++];
                strlcpy(o->ssid, ssid, sizeof(o->ssid));
                o->rssi = recs[i].rssi;
                o->secure = (a != WIFI_AUTH_OPEN && a != WIFI_AUTH_OWE);
                o->enterprise = (a == WIFI_AUTH_WPA2_ENTERPRISE || a == WIFI_AUTH_WPA3_ENTERPRISE ||
                                 a == WIFI_AUTH_WPA2_WPA3_ENTERPRISE || a == WIFI_AUTH_WPA3_ENT_192 ||
                                 a == WIFI_AUTH_WPA_ENTERPRISE);
            }
            qsort(out, *count, sizeof(out[0]), by_rssi_desc);
        } else {
            esp_wifi_clear_ap_list();
        }
        free(recs);
    } else {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
    }

    s_scanning = false;
    if (s_has_saved && !s_connected) {
        esp_wifi_connect();
    }
    xSemaphoreGive(s_lock);
    return err;
}

wifi_mgr_result_t wifi_mgr_connect(const char *ssid, const char *password)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ESP_LOGI(TAG, "joining \"%s\"", ssid);

    esp_timer_stop(s_retry_timer);
    s_joining = true;
    s_join_tries = 0;
    s_last_reason = 0;
    xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_FAILED);

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));  // let the "we left" event from the old network pass
    s_connected = false;
    xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_FAILED);

    apply_config(ssid, password);
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_FAILED, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(JOIN_TIMEOUT_MS));
    s_joining = false;

    wifi_mgr_result_t res;
    if (bits & BIT_GOT_IP) {
        creds_save(ssid, password);
        res = WIFI_MGR_OK;
    } else {
        uint8_t r = s_last_reason;
        res = reason_is_wrong_password(r) ? WIFI_MGR_WRONG_PASSWORD
              : reason_is_not_found(r) ? WIFI_MGR_NOT_FOUND
              : WIFI_MGR_TIMEOUT;
        ESP_LOGW(TAG, "could not join \"%s\" (last reason %d)", ssid, r);
        // Go back to the previously saved network, if there is one.
        esp_wifi_disconnect();
        if (s_has_saved) {
            apply_config(s_saved_ssid, s_saved_pass);
            schedule_retry();
        }
    }
    xSemaphoreGive(s_lock);
    return res;
}

esp_err_t wifi_mgr_forget(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_timer_stop(s_retry_timer);
    ESP_LOGI(TAG, "forgetting saved network");
    creds_erase();
    esp_wifi_disconnect();
    wifi_config_t empty = { 0 };
    esp_wifi_set_config(WIFI_IF_STA, &empty);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
