/*
 * One HTTPS call to TikPrint Cloud (see cloud_http.h). Modelled on conn_http.c.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "cloud_http.h"
#include "cloud_identity.h"
#include "config.h"
#include "wifi_mgr.h"

static const char *TAG = "cloud_http";

#define HTTP_TIMEOUT_MS 15000
#define URL_MAX         256

static esp_http_client_handle_t s_client;   // the open connection, or NULL
static cloud_reply_t *s_reply;              // where the answer being received goes
static size_t s_reply_max;

/* Called by the HTTP client as pieces of the answer arrive. */
static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && s_reply && s_reply->body) {
        size_t room = s_reply_max - s_reply->len, n = (size_t)evt->data_len;
        if (n > room) {
            s_reply->too_big = true;
            n = room;
        }
        memcpy(s_reply->body + s_reply->len, evt->data, n);
        s_reply->len += n;
    }
    return ESP_OK;
}

void cloud_http_close(void)
{
    if (s_client) {
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
}

static esp_http_client_handle_t new_client(const char *url)
{
    char agent[48];
    snprintf(agent, sizeof(agent), "TikPrint/%s", esp_app_get_description()->version);
    const esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,   // check the site's certificate
        .timeout_ms = HTTP_TIMEOUT_MS,
        .user_agent = agent,
        .event_handler = on_http_event,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .keep_alive_enable = true,
    };
    return esp_http_client_init(&config);
}

/* Set the method, the headers and the body of the request. */
static void fill_in(esp_http_client_handle_t client, const cloud_request_t *request)
{
    esp_http_client_set_method(client, request->post ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    esp_http_client_set_header(client, "X-Firmware-Version", esp_app_get_description()->version);
    // The connection is used for both kinds of call: take off the headers of the other kind.
    if (request->auth == CLOUD_AUTH_DEVICE) {
        esp_http_client_delete_header(client, "Authorization");
        esp_http_client_set_header(client, "X-Device-Id", cloud_identity_id());
        esp_http_client_set_header(client, "X-Device-Secret", cloud_identity_secret());
    } else {
        char bearer[160];
        snprintf(bearer, sizeof(bearer), "Bearer %s", request->token ? request->token : "");
        esp_http_client_delete_header(client, "X-Device-Id");
        esp_http_client_delete_header(client, "X-Device-Secret");
        esp_http_client_set_header(client, "Authorization", bearer);
    }
    if (request->json) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, request->json, (int)strlen(request->json));
    } else {
        esp_http_client_set_post_field(client, NULL, 0);   // also takes off Content-Type
    }
}

/* Send the request on the open connection, or on a new one. ESP_OK = an answer came. */
static esp_err_t send_once(const cloud_request_t *request, const char *url)
{
    if (!s_client) {
        s_client = new_client(url);
        if (!s_client) {
            return ESP_ERR_NO_MEM;
        }
    } else if (esp_http_client_set_url(s_client, url) != ESP_OK) {
        return ESP_FAIL;
    }
    fill_in(s_client, request);
    return esp_http_client_perform(s_client);
}

/*
 * The site answered 401. The HTTP client treats that as a failure and does not hand over the
 * answer's body, but the body is what says why ("invalid_token": this printer is not linked any
 * more). So ask once more, step by step on a connection of its own, and read the body ourselves.
 * This happens once in a printer's life with an account, so the extra call costs nothing.
 */
static void ask_again_for_refusal(const cloud_request_t *request, const char *url, cloud_reply_t *reply)
{
    s_reply = NULL;   // this answer is read straight into the body below, not through the event handler
    reply->len = 0;
    reply->too_big = false;
    reply->status = -1;
    esp_http_client_handle_t client = new_client(url);
    if (!client) {
        return;
    }
    fill_in(client, request);
    int json_len = request->json ? (int)strlen(request->json) : 0;
    if (esp_http_client_open(client, json_len) == ESP_OK &&
        (json_len == 0 || esp_http_client_write(client, request->json, json_len) == json_len) &&
        esp_http_client_fetch_headers(client) >= 0) {
        reply->status = esp_http_client_get_status_code(client);
        while (reply->len < s_reply_max) {
            int n = esp_http_client_read(client, reply->body + reply->len, (int)(s_reply_max - reply->len));
            if (n <= 0) {
                break;
            }
            reply->len += (size_t)n;
        }
    }
    esp_http_client_cleanup(client);
}

void cloud_http(const cloud_request_t *request, cloud_reply_t *reply)
{
    *reply = (cloud_reply_t){ .status = -1 };
    wifi_mgr_status_t wifi;
    wifi_mgr_get_status(&wifi);
    char url[URL_MAX];
    int url_len = snprintf(url, sizeof(url), "%s%s", CLOUD_URL, request->path);
    if (!wifi.connected || url_len >= (int)sizeof(url)) {
        cloud_http_close();   // the connection, if any, went with the Wi-Fi
        return;
    }
    reply->body = heap_caps_malloc(request->max_bytes + 1, MALLOC_CAP_SPIRAM);
    if (!reply->body) {
        return;
    }
    s_reply = reply;
    s_reply_max = request->max_bytes;

    int64_t started_us = esp_timer_get_time();
    bool reused = s_client != NULL;
    esp_err_t err = send_once(request, url);
    bool refused = err != ESP_OK && s_client && esp_http_client_get_status_code(s_client) == 401;
    if (err != ESP_OK && reused && !refused) {
        // The site closes a connection that has been quiet for a while: that is not a failure.
        // Start again on a new one.
        cloud_http_close();
        reply->len = 0;
        reply->too_big = false;
        err = send_once(request, url);
        refused = err != ESP_OK && s_client && esp_http_client_get_status_code(s_client) == 401;
    }
    if (refused) {
        cloud_http_close();
        ask_again_for_refusal(request, url, reply);
        err = reply->status > 0 ? ESP_OK : ESP_FAIL;
    } else if (err == ESP_OK) {
        reply->status = esp_http_client_get_status_code(s_client);
    }
    s_reply = NULL;
    int took_ms = (int)((esp_timer_get_time() - started_us) / 1000);
    int path_len = (int)strcspn(request->path, "?");   // what follows "?" is not logged
    if (err != ESP_OK) {
        cloud_http_close();
        free(reply->body);
        reply->body = NULL;
        reply->len = 0;
        reply->status = -1;
        ESP_LOGW(TAG, "%s %.*s failed after %d ms: %s", request->post ? "POST" : "GET", path_len, request->path,
                 took_ms, esp_err_to_name(err));
        return;
    }
    reply->body[reply->len] = 0;
    ESP_LOGI(TAG, "%s %.*s -> %d, %u bytes in %d ms", request->post ? "POST" : "GET", path_len, request->path,
             reply->status, (unsigned)reply->len, took_ms);
}

void cloud_reply_free(cloud_reply_t *reply)
{
    free(reply->body);
    reply->body = NULL;
    reply->len = 0;
}
