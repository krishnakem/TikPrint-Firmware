#pragma once

#include "esp_err.h"

/** Start the HTTP server on port 80. It listens on every interface (USB now, Wi-Fi later). */
esp_err_t web_server_start(void);
