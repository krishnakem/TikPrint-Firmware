/*
 * JSON API used by the web UI.
 *
 *   GET  /api/ping           empty reply; the UI calls it ~3x a second to notice unplugging fast
 *   GET  /api/status         board, USB, Wi-Fi, print queue and clock state
 *   POST /api/time           {"unix_ms": N, "timezone": "America/Detroit", "posix_tz": "EST5EDT,..."}
 *   GET  /api/wifi/scan      nearby networks
 *   POST /api/wifi/connect   {"ssid": "...", "password": "..."}
 *   POST /api/wifi/forget
 *   POST /api/print          put a ticket in the print queue (binary body, see print_post)
 *   GET  /api/diag/printer-test?baud=N  print one test line (optionally at another serial speed)
 *   GET  /api/diag/printer-status       ask the printer how it is; raw answer bytes + what they mean
 *   GET  /api/last-ticket    the last ticket that started printing (JSON)
 *   GET  /api/last-ticket.pbm  its image as a PBM file (opens in Preview)
 */
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include "ping/ping_sock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "api.h"
#include "usb_net.h"
#include "net_name.h"
#include "wifi_mgr.h"
#include "counter.h"
#include "printer.h"
#include "clock.h"

static const char *TAG = "api";

#define MAX_BODY 512
#define MAX_NETS 24
#define MAX_PRINT_META (16 * 1024)
#define MAX_PRINT_BODY (4 + MAX_PRINT_META + PRINTER_MAX_ROWS * PRINTER_WIDTH_BYTES)

/* Send a cJSON object as the response and free it. */
static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    return err;
}

/* Read the request body and parse it as JSON. Returns NULL (after replying 400) on failure. */
static cJSON *read_json_body(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body size");
        return NULL;
    }
    char buf[MAX_BODY + 1];
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            return NULL;
        }
        got += n;
    }
    buf[got] = 0;
    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
    }
    return json;
}

/* Did this request arrive over the USB cable? True if it was sent to 192.168.7.1. */
static bool request_came_over_usb(httpd_req_t *req)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&ss, &len) != 0) {
        return false;
    }
    uint32_t local = 0;
    if (ss.ss_family == AF_INET) {
        local = ((struct sockaddr_in *)&ss)->sin_addr.s_addr;
    } else if (ss.ss_family == AF_INET6) {
        // IPv4 connections on a dual-stack socket look like ::ffff:a.b.c.d
        memcpy(&local, &((struct sockaddr_in6 *)&ss)->sin6_addr.s6_addr[12], 4);
    }
    return local == ipaddr_addr(USB_NET_IP_STR);
}

/* ---------- GET /api/ping ---------- */
static esp_err_t ping_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

/* ---------- GET /api/diag/ping?ip=a.b.c.d ----------
 * Troubleshooting aid: the BOARD pings an address (3 tries) and reports how many replies came back.
 * Useful to tell "the Wi-Fi network blocks device-to-device traffic" apart from a firmware problem. */
static void diag_ping_end(esp_ping_handle_t h, void *arg)
{
    xSemaphoreGive((SemaphoreHandle_t)arg);
}

static esp_err_t diag_ping_get(httpd_req_t *req)
{
    char query[64] = "", ip[20] = "";
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "ip", ip, sizeof(ip));
    ip_addr_t target;
    if (!ipaddr_aton(ip, &target)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "use ?ip=a.b.c.d");
    }

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = 3;
    cfg.interval_ms = 400;
    cfg.timeout_ms = 800;
    esp_ping_callbacks_t cbs = { .on_ping_end = diag_ping_end, .cb_args = done };
    esp_ping_handle_t h = NULL;
    if (esp_ping_new_session(&cfg, &cbs, &h) != ESP_OK) {
        vSemaphoreDelete(done);
        return httpd_resp_send_500(req);
    }
    esp_ping_start(h);
    xSemaphoreTake(done, pdMS_TO_TICKS(6000));
    uint32_t sent = 0, received = 0;
    esp_ping_get_profile(h, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
    esp_ping_get_profile(h, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_delete_session(h);
    vSemaphoreDelete(done);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddNumberToObject(root, "sent", sent);
    cJSON_AddNumberToObject(root, "replies", received);
    return send_json(req, root);
}

/* The "time" part of /api/status:
 *   {"set":true,"source":"browser","now":"2026-11-20T22:05:31-05:00","zone":"EST","timezone":"America/Detroit","last_sync_s_ago":12}
 * Until something sets the clock: "set" is false, "source" is "none", "now" and "last_sync_s_ago" are null. */
static void add_clock_status(cJSON *root)
{
    clock_status_t c;
    clock_get_status(&c);
    cJSON *time = cJSON_AddObjectToObject(root, "time");
    cJSON_AddBoolToObject(time, "set", c.set);
    cJSON_AddStringToObject(time, "source", clock_source_name(c.source));
    cJSON_AddStringToObject(time, "timezone", c.timezone);
    if (c.set) {
        cJSON_AddStringToObject(time, "now", c.now);
        cJSON_AddStringToObject(time, "zone", c.zone);   // short zone name for tickets, e.g. "EST"
        cJSON_AddNumberToObject(time, "last_sync_s_ago", c.last_sync_s_ago);
    } else {
        cJSON_AddNullToObject(time, "now");
        cJSON_AddNullToObject(time, "last_sync_s_ago");
    }
}

/* ---------- GET /api/status ---------- */
static esp_err_t status_get(httpd_req_t *req)
{
    wifi_mgr_status_t w;
    wifi_mgr_get_status(&w);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "usb_connected", usb_net_is_attached());
    cJSON_AddStringToObject(root, "via", request_came_over_usb(req) ? "usb" : "wifi");
    cJSON_AddStringToObject(root, "usb_ip", USB_NET_IP_STR);
    if (w.connected) {
        cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
        cJSON_AddStringToObject(wifi, "ssid", w.ssid);
        cJSON_AddStringToObject(wifi, "ip", w.ip);
        cJSON_AddNumberToObject(wifi, "rssi", w.rssi);
    } else {
        cJSON_AddNullToObject(root, "wifi");
    }
    // Set while the board has a saved network but isn't connected to it (yet).
    if (w.has_saved && !w.connected) {
        cJSON_AddStringToObject(root, "wifi_connecting", w.ssid);
    } else {
        cJSON_AddNullToObject(root, "wifi_connecting");
    }
    cJSON_AddStringToObject(root, "hostname", NET_HOSTNAME ".local");
    cJSON_AddStringToObject(root, "version", esp_app_get_description()->version);
    cJSON_AddNumberToObject(root, "ticket_count", counter_get());
    int waiting, checked_s_ago;
    bool printing, paused;
    printer_queue_info(&waiting, &printing, &paused);
    cJSON *queue = cJSON_AddObjectToObject(root, "print_queue");
    cJSON_AddNumberToObject(queue, "waiting", waiting);    // tickets in line, not counting the one printing
    cJSON_AddBoolToObject(queue, "printing", printing);
    cJSON_AddBoolToObject(queue, "paused", paused);        // held back until there is paper / the cover is closed
    // "unknown" = the printer doesn't answer (no status wire, or it is off). Tickets still print then.
    cJSON *prn = cJSON_AddObjectToObject(root, "printer");
    cJSON_AddStringToObject(prn, "status", printer_status_name(printer_get_status(&checked_s_ago)));
    if (checked_s_ago >= 0) {
        cJSON_AddNumberToObject(prn, "last_checked_s_ago", checked_s_ago);
    } else {
        cJSON_AddNullToObject(prn, "last_checked_s_ago");
    }
    add_clock_status(root);
    cJSON_AddNumberToObject(root, "free_heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return send_json(req, root);
}

/* ---------- POST /api/time ----------
 * The page sends the computer's clock: {"unix_ms": 1795230331000,
 *                                       "timezone": "America/Detroit",
 *                                       "posix_tz": "EST5EDT,M3.2.0,M11.1.0"}
 * "unix_ms" is the time now (milliseconds since 1970, UTC). The two timezone fields are
 * optional and go together: the name people know, and the rule the board follows.
 * Reply: {"ok":true,"applied":true|false, "time":{...as in /api/status}}. "applied" is false
 * when the board kept its internet time instead (see clock.c). */
static esp_err_t time_post(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!body) {
        return ESP_OK;
    }
    const cJSON *jms = cJSON_GetObjectItem(body, "unix_ms");
    const cJSON *jtz = cJSON_GetObjectItem(body, "timezone");
    const cJSON *jposix = cJSON_GetObjectItem(body, "posix_tz");

    const char *error = NULL;
    clock_browser_result_t res = CLOCK_BROWSER_INVALID;
    if (cJSON_IsString(jtz) && cJSON_IsString(jposix) &&
        clock_set_timezone(jtz->valuestring, jposix->valuestring) != ESP_OK) {
        error = "bad_timezone";
    } else if (!cJSON_IsNumber(jms) ||
               (res = clock_set_from_browser((int64_t)jms->valuedouble)) == CLOCK_BROWSER_INVALID) {
        error = "bad_time";
    }
    cJSON_Delete(body);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", !error);
    if (error) {
        cJSON_AddStringToObject(root, "error", error);
    } else {
        cJSON_AddBoolToObject(root, "applied", res == CLOCK_BROWSER_APPLIED);
    }
    add_clock_status(root);
    return send_json(req, root);
}

/* ---------- GET /api/wifi/scan ---------- */
static esp_err_t wifi_scan_get(httpd_req_t *req)
{
    static wifi_mgr_net_t nets[MAX_NETS];
    size_t count = 0;
    if (wifi_mgr_scan(nets, MAX_NETS, &count) != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "scan_failed");
        return send_json(req, err);
    }
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        cJSON *n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "ssid", nets[i].ssid);
        cJSON_AddNumberToObject(n, "rssi", nets[i].rssi);
        cJSON_AddBoolToObject(n, "secure", nets[i].secure);
        cJSON_AddBoolToObject(n, "enterprise", nets[i].enterprise);
        cJSON_AddItemToArray(arr, n);
    }
    ESP_LOGI(TAG, "scan: %u networks", (unsigned)count);
    return send_json(req, arr);
}

/* ---------- POST /api/wifi/connect ---------- */
static esp_err_t wifi_connect_post(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!body) {
        return ESP_OK;
    }
    const cJSON *jssid = cJSON_GetObjectItem(body, "ssid");
    const cJSON *jpass = cJSON_GetObjectItem(body, "password");
    const char *ssid = cJSON_IsString(jssid) ? jssid->valuestring : "";
    const char *pass = cJSON_IsString(jpass) ? jpass->valuestring : "";

    cJSON *root = cJSON_CreateObject();
    if (!ssid[0] || strlen(ssid) > 32 || strlen(pass) > 64) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "bad_request");
    } else {
        wifi_mgr_result_t res = wifi_mgr_connect(ssid, pass);  // note: never log the password
        if (res == WIFI_MGR_OK) {
            wifi_mgr_status_t w;
            wifi_mgr_get_status(&w);
            cJSON_AddBoolToObject(root, "ok", true);
            cJSON_AddStringToObject(root, "ssid", ssid);
            cJSON_AddStringToObject(root, "ip", w.ip);
        } else {
            cJSON_AddBoolToObject(root, "ok", false);
            cJSON_AddStringToObject(root, "error",
                                    res == WIFI_MGR_WRONG_PASSWORD ? "wrong_password"
                                    : res == WIFI_MGR_NOT_FOUND ? "not_found" : "timeout");
        }
    }
    cJSON_Delete(body);
    return send_json(req, root);
}

/* ---------- GET /api/diag/printer-test?baud=9600 ----------
 * Troubleshooting aid: print one line of text. Optional settings are changed first and last
 * until reboot, to find what your printer needs without reflashing:
 *   baud=N  serial speed    pin=N  board pin    invert=1  flip the signal (for RS-232 printers)
 * Refused with "busy" while any ticket is printing or waiting in the queue. */
static esp_err_t diag_printer_test_get(httpd_req_t *req)
{
    char query[64] = "", baud[12] = "", pin[6] = "", inv[4] = "";
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "baud", baud, sizeof(baud));
    httpd_query_key_value(query, "pin", pin, sizeof(pin));
    httpd_query_key_value(query, "invert", inv, sizeof(inv));
    int b = atoi(baud);
    int p = pin[0] ? atoi(pin) : -1;
    // Pins that are safe to drive on this board: not USB, not the log port, not flash/PSRAM, not boot-mode pins.
    // (The pin that listens to the printer is refused further down, by the driver.)
    bool pin_ok = p == -1 || (p >= 1 && p <= 18 && p != 3) || p == 21 || (p >= 38 && p <= 42) || p == 47 || p == 48;
    cJSON *root = cJSON_CreateObject();
    if (b != 0 && (b < 1200 || b > 921600)) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "bad_baud");
    } else if (!pin_ok) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "bad_pin");
    } else {
        esp_err_t err = printer_test(b, p, inv[0] ? atoi(inv) != 0 : -1);
        cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
        if (err != ESP_OK) {
            cJSON_AddStringToObject(root, "error", err == ESP_ERR_NOT_SUPPORTED ? "no_printer"
                                                   : err == ESP_ERR_INVALID_ARG ? "bad_pin" : "busy");
        }
    }
    return send_json(req, root);
}

/* ---------- GET /api/diag/printer-status ----------
 * Troubleshooting aid for the status wire: asks the printer its four status questions
 * (ESC/POS "DLE EOT 1..4") and shows each raw answer byte next to what it means.
 *   {"ok":true,"answered":4,"status":"ok",
 *    "queries":[{"query":"DLE EOT 1","about":"printer","raw":"0x16","valid":true}, ...],
 *    "decoded":{"online":true,"cover_open":false,"paper_out":false,"paper_low":false,
 *               "cutter_error":false,"other_error":false}}
 * "raw" is null when no byte came back. "valid" is false for a byte that cannot be a status
 * byte (noise). With no valid answer at all, "status" is "unknown" and "decoded" is null. */
static esp_err_t diag_printer_status_get(httpd_req_t *req)
{
    static const char *const ABOUT[PRINTER_STATUS_QUERIES] = { "printer", "offline cause", "error cause", "paper sensor" };
    printer_status_report_t r;
    esp_err_t err = printer_read_status(&r);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", err == ESP_ERR_NOT_SUPPORTED ? "no_printer" : "busy");
        cJSON_AddStringToObject(root, "message", err == ESP_ERR_NOT_SUPPORTED ? "The fake printer is in use"
                                                 : "A ticket is being sent, try again when it has finished");
        return send_json(req, root);
    }
    cJSON_AddNumberToObject(root, "answered", r.answered);
    cJSON_AddStringToObject(root, "status", printer_status_name(r.status));
    cJSON *queries = cJSON_AddArrayToObject(root, "queries");
    for (int i = 0; i < PRINTER_STATUS_QUERIES; i++) {
        char text[16];
        cJSON *q = cJSON_CreateObject();
        snprintf(text, sizeof(text), "DLE EOT %d", i + 1);
        cJSON_AddStringToObject(q, "query", text);
        cJSON_AddStringToObject(q, "about", ABOUT[i]);
        if (r.raw[i] >= 0) {
            snprintf(text, sizeof(text), "0x%02X", (unsigned)r.raw[i]);
            cJSON_AddStringToObject(q, "raw", text);
        } else {
            cJSON_AddNullToObject(q, "raw");
        }
        cJSON_AddBoolToObject(q, "valid", r.valid[i]);
        cJSON_AddItemToArray(queries, q);
    }
    if (r.answered == 0) {
        cJSON_AddNullToObject(root, "decoded");
        cJSON_AddStringToObject(root, "message", "No answer from the printer: check the status wire and that the printer is on");
        return send_json(req, root);
    }
    cJSON *d = cJSON_AddObjectToObject(root, "decoded");
    cJSON_AddBoolToObject(d, "online", r.online);
    cJSON_AddBoolToObject(d, "cover_open", r.cover_open);
    cJSON_AddBoolToObject(d, "paper_out", r.paper_out);
    cJSON_AddBoolToObject(d, "paper_low", r.paper_low);
    cJSON_AddBoolToObject(d, "cutter_error", r.cutter_error);
    cJSON_AddBoolToObject(d, "other_error", r.other_error);
    return send_json(req, root);
}

/* ---------- POST /api/wifi/forget ---------- */
static esp_err_t wifi_forget_post(httpd_req_t *req)
{
    wifi_mgr_forget();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

/* ---------- POST /api/print ----------
 * Body (application/octet-stream), in this order:
 *   1. 4 bytes: length N of the JSON that follows (little-endian)
 *   2. N bytes: JSON {"title": "...", "body": "...", "source": "...",
 *                     "image": {"width": 576, "height": H},   (optional)
 *                     "text_in_image": true}                  (optional, see below)
 *   3. if "image" is present: H * 72 bytes of raw image bits.
 *      "text_in_image" means the page drew the text at the top of the image, in its own
 *      font. title/body/source are then only kept for the log and /api/last-ticket.
 *      The browser has already turned the picture or PDF into dots, 576 wide and
 *      pure black/white, so the board never has to decode JPG, PNG or PDF.
 * The ticket joins the print queue and the reply comes back at once:
 *   {"ok":true,"number":N,"ticket_count":N,"queued":false,"position":0}
 *   "position" is its place in line: 0 = printing now, 1 = one ticket ahead of it, ...
 *   "queued" is true when it has to wait (position > 0).
 */
static esp_err_t print_fail(httpd_req_t *req, const char *error, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", error);
    cJSON_AddStringToObject(root, "message", message);
    return send_json(req, root);
}

static esp_err_t print_post(httpd_req_t *req)
{
    size_t len = req->content_len;
    if (len < 4 || len > MAX_PRINT_BODY) {
        return print_fail(req, "bad_size", "The ticket is empty or too large");
    }
    uint8_t *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);  // up to ~300 KB: use the big external RAM
    if (!buf) {
        return print_fail(req, "no_memory", "The printer is out of memory");
    }
    size_t got = 0;
    while (got < len) {
        int n = httpd_req_recv(req, (char *)buf + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            free(buf);
            return ESP_FAIL;  // connection dropped mid-upload
        }
        got += n;
    }

    uint32_t meta_len = buf[0] | (buf[1] << 8) | (buf[2] << 16) | ((uint32_t)buf[3] << 24);
    cJSON *meta = (meta_len <= MAX_PRINT_META && 4 + meta_len <= len)
                  ? cJSON_ParseWithLength((const char *)buf + 4, meta_len) : NULL;
    if (!meta) {
        free(buf);
        return print_fail(req, "bad_request", "The ticket could not be read");
    }

    esp_err_t ret;
    const cJSON *jt = cJSON_GetObjectItem(meta, "title");
    const cJSON *jb = cJSON_GetObjectItem(meta, "body");
    const cJSON *js = cJSON_GetObjectItem(meta, "source");
    const cJSON *jimg = cJSON_GetObjectItem(meta, "image");
    printer_job_t job = {
        .title = cJSON_IsString(jt) ? jt->valuestring : "",
        .body = cJSON_IsString(jb) ? jb->valuestring : "",
        .source = cJSON_IsString(js) ? js->valuestring : "",
    };

    if (cJSON_IsObject(jimg)) {
        int w = cJSON_GetObjectItem(jimg, "width") ? cJSON_GetObjectItem(jimg, "width")->valueint : 0;
        int h = cJSON_GetObjectItem(jimg, "height") ? cJSON_GetObjectItem(jimg, "height")->valueint : 0;
        size_t img_bytes = len - 4 - meta_len;
        if (w != PRINTER_WIDTH_DOTS || h < 1 || h > PRINTER_MAX_ROWS || img_bytes != (size_t)h * PRINTER_WIDTH_BYTES) {
            ret = print_fail(req, "bad_image", "The image is not in the expected format");
            goto out;
        }
        job.image = buf + 4 + meta_len;
        job.image_rows = h;
        job.text_in_image = cJSON_IsTrue(cJSON_GetObjectItem(meta, "text_in_image"));
    }
    if (!job.title[0] && !job.body[0] && !job.image) {
        ret = print_fail(req, "empty", "Nothing to print");
        goto out;
    }

    // The queue copies the job (so buf can be freed below) and gives it its ticket number.
    printer_ticket_t ticket;
    esp_err_t perr = printer_print_job(&job, &ticket);
    if (perr == ESP_ERR_INVALID_STATE) {
        ret = print_fail(req, "queue_full", "The print queue is full, try again in a moment");
        goto out;
    }
    if (perr != ESP_OK) {
        ret = print_fail(req, "no_memory", "The printer is out of memory");
        goto out;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "number", ticket.number);
    cJSON_AddNumberToObject(root, "ticket_count", ticket.number);
    cJSON_AddBoolToObject(root, "queued", ticket.position > 0);
    cJSON_AddNumberToObject(root, "position", ticket.position);
    ret = send_json(req, root);

out:
    cJSON_Delete(meta);
    free(buf);
    return ret;
}

/* ---------- GET /api/last-ticket ----------
 * The last ticket that STARTED printing. The printer task replaces it when the next
 * ticket starts, so it is only read between printer_last_job_lock() and ..._unlock(). */
static esp_err_t last_ticket_get(httpd_req_t *req)
{
    const printer_job_t *job = printer_last_job_lock();
    cJSON *root = cJSON_CreateObject();
    if (!job) {
        printer_last_job_unlock();
        httpd_resp_set_status(req, "404 Not Found");
        cJSON_AddStringToObject(root, "error", "no_ticket");
        return send_json(req, root);
    }
    cJSON_AddNumberToObject(root, "number", job->number);
    cJSON_AddStringToObject(root, "title", job->title);
    cJSON_AddStringToObject(root, "body", job->body);
    cJSON_AddStringToObject(root, "source", job->source);
    if (job->image) {
        cJSON *img = cJSON_AddObjectToObject(root, "image");
        cJSON_AddNumberToObject(img, "width", PRINTER_WIDTH_DOTS);
        cJSON_AddNumberToObject(img, "height", job->image_rows);
        cJSON_AddStringToObject(img, "url", "/api/last-ticket.pbm");
    } else {
        cJSON_AddNullToObject(root, "image");
    }
    printer_last_job_unlock();   // cJSON has copied the text, so the job is no longer needed
    return send_json(req, root);
}

/* ---------- GET /api/last-ticket.pbm ----------
 * PBM is the simplest image format there is: a text header, then 1 bit per pixel (1 = black).
 * That is exactly how the image is stored, so it can be sent as-is. */
static esp_err_t last_ticket_pbm_get(httpd_req_t *req)
{
    const printer_job_t *job = printer_last_job_lock();
    if (!job || !job->image) {
        printer_last_job_unlock();
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "the last ticket has no image");
    }
    char header[32];
    int n = snprintf(header, sizeof(header), "P4\n%d %d\n", PRINTER_WIDTH_DOTS, job->image_rows);
    httpd_resp_set_type(req, "image/x-portable-bitmap");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=\"last-ticket.pbm\"");
    httpd_resp_send_chunk(req, header, n);
    httpd_resp_send_chunk(req, (const char *)job->image, (size_t)job->image_rows * PRINTER_WIDTH_BYTES);
    printer_last_job_unlock();   // the image has been sent; the next ticket may replace it now
    return httpd_resp_send_chunk(req, NULL, 0);
}

void api_register(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        { .uri = "/api/ping",         .method = HTTP_GET,  .handler = ping_get },
        { .uri = "/api/status",       .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/time",         .method = HTTP_POST, .handler = time_post },
        { .uri = "/api/diag/ping",    .method = HTTP_GET,  .handler = diag_ping_get },
        { .uri = "/api/diag/printer-test", .method = HTTP_GET, .handler = diag_printer_test_get },
        { .uri = "/api/diag/printer-status", .method = HTTP_GET, .handler = diag_printer_status_get },
        { .uri = "/api/wifi/scan",    .method = HTTP_GET,  .handler = wifi_scan_get },
        { .uri = "/api/wifi/connect", .method = HTTP_POST, .handler = wifi_connect_post },
        { .uri = "/api/wifi/forget",  .method = HTTP_POST, .handler = wifi_forget_post },
        { .uri = "/api/print",        .method = HTTP_POST, .handler = print_post },
        { .uri = "/api/last-ticket",  .method = HTTP_GET,  .handler = last_ticket_get },
        { .uri = "/api/last-ticket.pbm", .method = HTTP_GET, .handler = last_ticket_pbm_get },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
}
