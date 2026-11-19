/*
 * Minimal DHCP server for the USB network.
 *
 * Why not ESP-IDF's built-in DHCP server? It always includes a DNS-server
 * option in its replies (it falls back to its own address), and we must not
 * hand the Mac a DNS server or a router: the Mac has to keep using its normal
 * Wi-Fi for internet. This server replies with only:
 *   - your address: 192.168.7.2
 *   - subnet mask:  255.255.255.0
 *   - lease time and server id
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/ip4_addr.h"

#include "usb_dhcp.h"
#include "usb_net.h"

static const char *TAG = "usb_dhcp";

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define LEASE_SECONDS    3600

#define BOOTREQUEST 1
#define BOOTREPLY   2
#define DHCP_MAGIC  0x63825363

#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPACK      5
#define DHCPNAK      6
#define DHCPINFORM   8

#define OPT_PAD          0
#define OPT_SUBNET_MASK  1
#define OPT_REQUESTED_IP 50
#define OPT_LEASE_TIME   51
#define OPT_MSG_TYPE     53
#define OPT_SERVER_ID    54
#define OPT_END          255

typedef struct __attribute__((packed)) {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t magic;
    uint8_t options[312];
} dhcp_msg_t;

#define DHCP_FIXED_LEN (offsetof(dhcp_msg_t, options))

static esp_netif_t *s_netif;

/* Find one option in a received message. Returns its data and sets *len, or NULL. */
static const uint8_t *find_option(const dhcp_msg_t *msg, size_t msg_len, uint8_t code, uint8_t *len)
{
    const uint8_t *p = msg->options;
    const uint8_t *end = (const uint8_t *)msg + msg_len;
    while (p < end && *p != OPT_END) {
        if (*p == OPT_PAD) {
            p++;
            continue;
        }
        if (p + 2 > end || p + 2 + p[1] > end) {
            break;
        }
        if (*p == code) {
            *len = p[1];
            return p + 2;
        }
        p += 2 + p[1];
    }
    return NULL;
}

static uint8_t *put_opt(uint8_t *p, uint8_t code, const void *data, uint8_t len)
{
    *p++ = code;
    *p++ = len;
    memcpy(p, data, len);
    return p + len;
}

static void dhcp_task(void *arg)
{
    const uint32_t server_ip = ipaddr_addr(USB_NET_IP_STR);
    const uint32_t client_ip = ipaddr_addr(USB_NET_HOST_IP_STR);
    const uint32_t netmask = ipaddr_addr(USB_NET_MASK_STR);

    int sock = -1;
    while (sock < 0) {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    // Only listen and reply on the USB interface, never on Wi-Fi.
    struct ifreq ifr = { 0 };
    esp_netif_get_netif_impl_name(s_netif, ifr.ifr_name);
    if (setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) != 0) {
        ESP_LOGE(TAG, "could not bind to USB interface %s (errno %d)", ifr.ifr_name, errno);
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DHCP_SERVER_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind failed (errno %d)", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DHCP server listening on %s; will offer " USB_NET_HOST_IP_STR " (no router, no DNS)", ifr.ifr_name);

    static dhcp_msg_t req, rep;
    while (true) {
        int n = recvfrom(sock, &req, sizeof(req), 0, NULL, NULL);
        if (n < (int)DHCP_FIXED_LEN || req.op != BOOTREQUEST || ntohl(req.magic) != DHCP_MAGIC) {
            continue;
        }

        uint8_t len = 0;
        const uint8_t *opt = find_option(&req, n, OPT_MSG_TYPE, &len);
        if (!opt || len != 1) {
            continue;
        }
        uint8_t type = opt[0];

        // If the client names a different server, the message isn't for us.
        opt = find_option(&req, n, OPT_SERVER_ID, &len);
        if (opt && len == 4 && memcmp(opt, &server_ip, 4) != 0) {
            continue;
        }

        uint8_t reply_type;
        uint32_t yiaddr = client_ip;
        if (type == DHCPDISCOVER) {
            reply_type = DHCPOFFER;
        } else if (type == DHCPREQUEST) {
            uint32_t wanted = req.ciaddr;
            opt = find_option(&req, n, OPT_REQUESTED_IP, &len);
            if (opt && len == 4) {
                memcpy(&wanted, opt, 4);
            }
            reply_type = (wanted == client_ip) ? DHCPACK : DHCPNAK;
        } else if (type == DHCPINFORM) {
            reply_type = DHCPACK;
            yiaddr = 0;
        } else {
            continue;  // RELEASE / DECLINE: nothing to do with a single fixed lease
        }

        memset(&rep, 0, sizeof(rep));
        rep.op = BOOTREPLY;
        rep.htype = req.htype;
        rep.hlen = req.hlen;
        rep.xid = req.xid;
        rep.flags = req.flags;
        rep.ciaddr = req.ciaddr;
        rep.giaddr = req.giaddr;
        memcpy(rep.chaddr, req.chaddr, sizeof(rep.chaddr));
        rep.magic = htonl(DHCP_MAGIC);

        uint8_t *p = rep.options;
        p = put_opt(p, OPT_MSG_TYPE, &reply_type, 1);
        p = put_opt(p, OPT_SERVER_ID, &server_ip, 4);
        if (reply_type != DHCPNAK) {
            rep.yiaddr = yiaddr;
            p = put_opt(p, OPT_SUBNET_MASK, &netmask, 4);
            if (type != DHCPINFORM) {
                uint32_t lease = htonl(LEASE_SECONDS);
                p = put_opt(p, OPT_LEASE_TIME, &lease, 4);
            }
            // Intentionally no router (3) and no DNS (6) options.
        }
        *p++ = OPT_END;

        // Replies are at least 300 bytes for compatibility with old BOOTP clients.
        size_t rep_len = p - (uint8_t *)&rep;
        if (rep_len < 300) {
            rep_len = 300;
        }

        struct sockaddr_in to = {
            .sin_family = AF_INET,
            .sin_port = htons(DHCP_CLIENT_PORT),
            .sin_addr.s_addr = htonl(INADDR_BROADCAST),
        };
        if (sendto(sock, &rep, rep_len, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
            ESP_LOGW(TAG, "reply failed (errno %d)", errno);
        } else if (reply_type == DHCPACK && type == DHCPREQUEST) {
            ESP_LOGI(TAG, "computer got " USB_NET_HOST_IP_STR " (%02x:%02x:%02x:%02x:%02x:%02x)",
                     req.chaddr[0], req.chaddr[1], req.chaddr[2], req.chaddr[3], req.chaddr[4], req.chaddr[5]);
        }
    }
}

esp_err_t usb_dhcp_start(esp_netif_t *netif)
{
    if (!netif) {
        return ESP_ERR_INVALID_ARG;
    }
    s_netif = netif;
    BaseType_t ok = xTaskCreate(dhcp_task, "usb_dhcp", 4096, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
