/*
 * One HTTPS request to an app's API (see conn_http.h). Modelled on nws_get() in weather.c.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "conn_http.h"
#include "wifi_mgr.h"

static const char *TAG = "conn_http";

#define HTTP_TIMEOUT_MS 15000

typedef struct {
    char *buf;       // the answer's body, as text
    size_t len, max;
    bool too_big;
} reply_t;

/* Called by the HTTP client as pieces of the answer arrive. */
static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    reply_t *r = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        // Only keep the body of a real answer (200), not of an error page.
        if (esp_http_client_get_status_code(evt->client) != 200) {
            return ESP_OK;
        }
        if (r->len + (size_t)evt->data_len > r->max) {
            r->too_big = true;
            return ESP_OK;
        }
        memcpy(r->buf + r->len, evt->data, (size_t)evt->data_len);
        r->len += (size_t)evt->data_len;
    }
    return ESP_OK;
}

conn_result_t conn_http(const conn_request_t *request, char **out)
{
    *out = NULL;
    wifi_mgr_status_t wifi;
    wifi_mgr_get_status(&wifi);
    if (!wifi.connected) {
        return CONN_NO_WIFI;
    }
    reply_t reply = { .max = request->max_bytes };
    reply.buf = heap_caps_malloc(request->max_bytes + 1, MALLOC_CAP_SPIRAM);
    if (!reply.buf) {
        return CONN_UNREACHABLE;
    }
    char agent[48];
    snprintf(agent, sizeof(agent), "TikPrint/%s", esp_app_get_description()->version);
    const esp_http_client_config_t config = {
        .url = request->url,
        .method = request->body ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,   // check the app's certificate
        .timeout_ms = HTTP_TIMEOUT_MS,
        .user_agent = agent,
        .event_handler = on_http_event,
        .user_data = &reply,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,                       // room for a long address plus a long token
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(reply.buf);
        return CONN_UNREACHABLE;
    }
    if (request->auth) {
        esp_http_client_set_header(client, "Authorization", request->auth);
    }
    for (int i = 0; i < CONN_HTTP_HEADERS_MAX; i++) {
        if (request->header[i].name) {
            esp_http_client_set_header(client, request->header[i].name, request->header[i].value);
        }
    }
    if (request->body) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, request->body, (int)strlen(request->body));
    }
    int64_t started_us = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(client);
    int status = err == ESP_OK ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);
    int took_ms = (int)((esp_timer_get_time() - started_us) / 1000);

    // The address is not logged in full: for some apps it carries the token.
    const char *host = strstr(request->url, "://");
    host = host ? host + 3 : request->url;
    int host_len = (int)strcspn(host, "/?");
    if (status == 200 && !reply.too_big) {
        reply.buf[reply.len] = 0;
        char *fitted = heap_caps_realloc(reply.buf, reply.len + 1, MALLOC_CAP_SPIRAM);   // give back the unused room
        *out = fitted ? fitted : reply.buf;
        ESP_LOGI(TAG, "%.*s: %u bytes in %d ms", host_len, host, (unsigned)reply.len, took_ms);
        return CONN_OK;
    }
    free(reply.buf);
    if (status == 200) {
        ESP_LOGW(TAG, "%.*s: the answer is bigger than %u bytes", host_len, host, (unsigned)request->max_bytes);
        return CONN_BAD_ANSWER;
    }
    ESP_LOGW(TAG, "%.*s failed after %d ms: %s, HTTP status %d", host_len, host, took_ms, esp_err_to_name(err), status);
    switch (status) {
    case 401: case 403: return CONN_BAD_TOKEN;
    case 429:           return CONN_RATE_LIMITED;
    case 400: case 404: return CONN_NOT_FOUND;
    default:            return CONN_UNREACHABLE;
    }
}
