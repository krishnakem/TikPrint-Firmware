#pragma once
/* Stand-in for the board's saved settings: kept in memory (test_rules.c), so a test can "restart the board". */
#include <stddef.h>
#include "esp_err.h"
typedef int nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h);
void nvs_close(nvs_handle_t h);
esp_err_t nvs_commit(nvs_handle_t h);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t n);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *v, size_t *n);
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *v);
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *v, size_t *n);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);
