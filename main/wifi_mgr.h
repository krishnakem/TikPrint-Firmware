#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;      // needs a password
    bool enterprise;  // WPA2/WPA3-Enterprise (username + password): not supported
} wifi_mgr_net_t;

typedef struct {
    bool connected;
    bool has_saved;   // credentials are stored on the board
    char ssid[33];    // connected network, or the saved one while (re)connecting
    char ip[16];
    int8_t rssi;
} wifi_mgr_status_t;

typedef enum {
    WIFI_MGR_OK = 0,
    WIFI_MGR_WRONG_PASSWORD,
    WIFI_MGR_NOT_FOUND,
    WIFI_MGR_TIMEOUT,
} wifi_mgr_result_t;

/** Start Wi-Fi in station mode. If credentials are saved in NVS, connect in the background. */
esp_err_t wifi_mgr_start(void);

void wifi_mgr_get_status(wifi_mgr_status_t *out);

/** Scan for networks (takes 2-3 s). Results are de-duplicated and sorted strongest first. */
esp_err_t wifi_mgr_scan(wifi_mgr_net_t *out, size_t max, size_t *count);

/** Try to join a network (blocks up to ~20 s). Credentials are saved to NVS only if it works. */
wifi_mgr_result_t wifi_mgr_connect(const char *ssid, const char *password);

/** Disconnect and erase the saved credentials. */
esp_err_t wifi_mgr_forget(void);
