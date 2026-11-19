/*
 * USB network adapter.
 *
 * The ESP32-S3's built-in USB port shows up on the computer as a network card
 * (CDC-NCM). On the board side we attach it to its own lwIP network interface
 * with the static address 192.168.7.1/24. It is NOT bridged to Wi-Fi.
 *
 *      Mac (192.168.7.2)  <== USB cable ==>  board (192.168.7.1)
 */
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "lwip/esp_netif_net_stack.h"
#include "lwip/ip4_addr.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"

#include "usb_net.h"

static const char *TAG = "usb_net";

static esp_netif_t *s_netif;
static volatile bool s_attached;

/* ---- lwIP -> USB (the board sends a packet to the Mac) ---- */
static esp_err_t netif_transmit(void *h, void *buffer, size_t len)
{
    if (!s_attached) {
        return ESP_OK;  // nobody is plugged in: silently drop
    }
    esp_err_t err = tinyusb_net_send_sync(buffer, len, NULL, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "tx dropped (%s)", esp_err_to_name(err));
    }
    return ESP_OK;
}

static void netif_free_rx(void *h, void *buffer)
{
    free(buffer);
}

/* ---- USB -> lwIP (a packet arrived from the Mac) ---- */
static esp_err_t usb_recv_cb(void *buffer, uint16_t len, void *ctx)
{
    if (!s_netif) {
        return ESP_OK;
    }
    // TinyUSB reuses its buffer right after this returns, so hand lwIP a copy.
    void *copy = malloc(len);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, buffer, len);
    return esp_netif_receive(s_netif, copy, len, NULL);
}

static void usb_event_cb(tinyusb_event_t *event, void *arg)
{
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        s_attached = true;
        ESP_LOGI(TAG, "USB attached: computer connected");
        break;
    case TINYUSB_EVENT_DETACHED:
        s_attached = false;
        ESP_LOGI(TAG, "USB detached");
        break;
    default:
        break;
    }
}

esp_err_t usb_net_start(void)
{
    /* Two MAC addresses are needed: one for the Mac's end of the "cable" and
     * one for the board's end. Both are derived from the chip's unique MAC and
     * marked "locally administered" (bit 1 of the first byte). */
    uint8_t base[6];
    esp_read_mac(base, ESP_MAC_BASE);
    uint8_t host_mac[6], board_mac[6];
    memcpy(host_mac, base, 6);
    memcpy(board_mac, base, 6);
    host_mac[0] = (base[0] | 0x02) & 0xFE;
    board_mac[0] = host_mac[0];
    host_mac[5] ^= 0x01;
    board_mac[5] ^= 0x02;

    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(usb_event_cb);
    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB install failed: %s", esp_err_to_name(err));
        return err;
    }

    tinyusb_net_config_t net_cfg = {
        .on_recv_callback = usb_recv_cb,
    };
    memcpy(net_cfg.mac_addr, host_mac, 6);
    err = tinyusb_net_init(&net_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB net init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Static address, no gateway: this network only ever reaches the board. */
    static esp_netif_ip_info_t ip_info;
    ip_info.ip.addr = ipaddr_addr(USB_NET_IP_STR);
    ip_info.netmask.addr = ipaddr_addr(USB_NET_MASK_STR);
    ip_info.gw.addr = 0;

    esp_netif_inherent_config_t base_cfg = {
        .flags = ESP_NETIF_FLAG_AUTOUP,   // no built-in DHCP server/client; see usb_dhcp.c
        .ip_info = &ip_info,
        .if_key = "usb",
        .if_desc = "usb ncm",
        .route_prio = 10,                 // lower than Wi-Fi, so Wi-Fi stays the default route
    };
    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = (void *)1,              // TinyUSB net is a singleton; just needs to be non-NULL
        .transmit = netif_transmit,
        .driver_free_rx_buffer = netif_free_rx,
    };
    struct esp_netif_netstack_config stack_cfg = {
        .lwip = {
            .init_fn = ethernetif_init,   // from lwIP's point of view this is plain Ethernet
            .input_fn = ethernetif_input,
        },
    };
    esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = &driver_cfg,
        .stack = &stack_cfg,
    };

    s_netif = esp_netif_new(&cfg);
    if (!s_netif) {
        ESP_LOGE(TAG, "could not create USB network interface");
        return ESP_FAIL;
    }
    esp_netif_set_mac(s_netif, board_mac);
    esp_netif_action_start(s_netif, NULL, 0, NULL);
    esp_netif_set_ip_info(s_netif, &ip_info);

    ESP_LOGI(TAG, "USB network up: board " USB_NET_IP_STR "/24, board MAC " MACSTR ", host MAC " MACSTR,
             MAC2STR(board_mac), MAC2STR(host_mac));
    return ESP_OK;
}

esp_netif_t *usb_net_get_netif(void)
{
    return s_netif;
}

bool usb_net_is_attached(void)
{
    return s_attached;
}
