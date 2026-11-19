#pragma once

#include "esp_http_server.h"

/** Register all /api/... handlers on the HTTP server. */
void api_register(httpd_handle_t server);
