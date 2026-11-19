#pragma once

#include "esp_err.h"
#include "esp_netif.h"

/**
 * Start a tiny DHCP server on the given interface only.
 * It hands out exactly one address (192.168.7.2) and deliberately sends
 * NO router and NO DNS option, so the computer keeps its normal internet.
 */
esp_err_t usb_dhcp_start(esp_netif_t *netif);
