/*
 * TikPrint firmware entry point.
 */
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "usb_net.h"
#include "usb_dhcp.h"
#include "net_name.h"
#include "wifi_mgr.h"
#include "clock.h"
#include "settings.h"
#include "fw_github.h"
#include "fw_update.h"
#include "printer.h"
#include "web_server.h"
#include "weather.h"
#include "automations.h"
#include "connectors.h"
#include "cloud_state.h"

static const char *TAG = "tikprint";

void app_main(void)
{
    ESP_LOGI(TAG, "TikPrint starting");
    ESP_LOGI(TAG, "free heap: %u bytes, free PSRAM: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // NVS = small key/value storage in flash (Wi-Fi credentials and the ticket counter will live here).
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(cloud_state_init());   // only reads what is saved: no network
    ESP_ERROR_CHECK(clock_init());   // before Wi-Fi, so it hears "Wi-Fi got an address" and fetches internet time
    ESP_ERROR_CHECK(printer_init());
    ESP_ERROR_CHECK(weather_init());
    ESP_ERROR_CHECK(connectors_init());   // before automations: its task prints the active connector
    ESP_ERROR_CHECK(automations_start());   // after the printer: its tickets go into the print queue
    ESP_ERROR_CHECK(usb_net_start());
    ESP_ERROR_CHECK(usb_dhcp_start(usb_net_get_netif()));
    net_name_start(usb_net_get_netif());  // not fatal: the IP address still works without it
    ESP_ERROR_CHECK(wifi_mgr_start());
    ESP_ERROR_CHECK(web_server_start());
    // Last, so that freshly updated firmware is only kept if everything above started.
    ESP_ERROR_CHECK(fw_update_init());
    ESP_ERROR_CHECK(fw_github_start());   // looks for newer firmware on GitHub once a day

    ESP_LOGI(TAG, "ready: plug the USB-C cable into a computer and open http://" NET_HOSTNAME ".local (or http://" USB_NET_IP_STR ")");
}
