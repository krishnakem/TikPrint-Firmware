#pragma once
/* Stand-in for the board's esp_err.h (tools/host_test). */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>   /* the board's own pulls these in, and the QR code library counts on it */
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
