#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif.h"

#define USB_NET_IP_STR      "192.168.7.1"
#define USB_NET_HOST_IP_STR "192.168.7.2"
#define USB_NET_MASK_STR    "255.255.255.0"

/** Start TinyUSB as a USB network adapter and attach it to its own network interface (192.168.7.1/24). */
esp_err_t usb_net_start(void);

/** The USB network interface (NULL before usb_net_start). */
esp_netif_t *usb_net_get_netif(void);

/** True while a computer has the USB device attached and configured. */
bool usb_net_is_attached(void);
