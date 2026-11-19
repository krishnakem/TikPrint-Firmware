/*
 * mDNS: lets computers find the board by name (tikprint.local) instead of by
 * IP address. No DNS server is involved; the board simply answers "that's me"
 * when a computer on the same link asks who "tikprint.local" is.
 */
#include "esp_log.h"
#include "mdns.h"

#include "net_name.h"

static const char *TAG = "net_name";

esp_err_t net_name_start(esp_netif_t *usb_netif)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return err;
    }
    mdns_hostname_set(NET_HOSTNAME);
    mdns_instance_name_set("TikPrint");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    // The USB interface is a custom one, so mDNS has to be told about it by hand.
    err = mdns_register_netif(usb_netif);
    if (err == ESP_OK) {
        err = mdns_netif_action(usb_netif, MDNS_EVENT_ENABLE_IP4 | MDNS_EVENT_ANNOUNCE_IP4);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS on USB failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "mDNS name: http://" NET_HOSTNAME ".local");
    return ESP_OK;
}
