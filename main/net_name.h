#pragma once

#include "esp_err.h"
#include "esp_netif.h"

/** The board's network name. Browsers reach it as http://tikprint.local */
#define NET_HOSTNAME "tikprint"

/** Start mDNS: announce NET_HOSTNAME.local and the web server (_http._tcp) on the USB interface. */
esp_err_t net_name_start(esp_netif_t *usb_netif);
