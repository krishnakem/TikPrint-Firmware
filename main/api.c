/*
 * JSON API used by the web UI.
 *
 *   GET  /api/ping           empty reply; the UI calls it ~3x a second to notice unplugging fast
 *   GET  /api/status         board, USB, Wi-Fi, print queue and clock state
 *   POST /api/settings       {"location": "...", "show_location": true, "show_date": true, "show_time": true, "header_font": "Inter"}
 *   POST /api/time           {"unix_ms": N, "timezone": "America/Detroit", "posix_tz": "EST5EDT,..."}
 *   GET  /api/wifi/scan      nearby networks
 *   POST /api/wifi/connect   {"ssid": "...", "password": "..."}
 *   POST /api/wifi/forget
 *   POST /api/print          put a ticket in the print queue (binary body, see print_post)
 *   GET  /api/diag/printer-test?baud=N  print one test line (optionally at another serial speed)
 *   GET  /api/diag/printer-status       ask the printer how it is; raw answer bytes + what they mean
 *   GET  /api/last-ticket    the last ticket that started printing (JSON)
 *   GET  /api/last-ticket.pbm  its image as a PBM file (opens in Preview)
 *   POST /api/update/check   ask GitHub whether there is newer firmware (the answer shows up in /api/status)
 *   POST /api/update/install download the newer firmware from GitHub and restart into it
 *   POST /api/update         install firmware from a file. Only over the USB cable (see update_post)
 *   POST /api/automations/weather            {"zip": "48104"}: the ZIP code weather tickets are for
 *   POST /api/automations/weather/print-now  fetch the weather and print it now ({"kind": "today"|"rest"|"tomorrow"}, optional)
 */
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
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
#include "settings.h"
#include "fw_github.h"
#include "fw_update.h"
#include "automations.h"
#include "zip_table.h"
#include "city_table.h"
#include "ticket_draw.h"

static const char *TAG = "api";

#define MAX_BODY 512
#define MAX_NETS 24
#define MAX_PRINT_META (32 * 1024)     // a ticket of printer-font lines lists every line in its JSON
#define MAX_PRINT_BODY (4 + MAX_PRINT_META + PRINTER_MAX_ROWS * PRINTER_WIDTH_BYTES)
#define UPDATE_CHUNK 4096          // a firmware upload is written to flash in pieces of this size
#define UPDATE_MAX_STALLS 6        // give up on an upload after this many 5 s waits in a row with no data
#define UPDATE_LOG_EVERY (256 * 1024)

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

/* The other end's IPv4 address, or 0 if it can't be told. */
static uint32_t request_peer_ip(httpd_req_t *req)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    uint32_t peer = 0;
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&ss, &len) != 0) {
        return 0;
    }
    if (ss.ss_family == AF_INET) {
        peer = ((struct sockaddr_in *)&ss)->sin_addr.s_addr;
    } else if (ss.ss_family == AF_INET6) {
        memcpy(&peer, &((struct sockaddr_in6 *)&ss)->sin6_addr.s6_addr[12], 4);
    }
    return peer;
}

/* Stricter than request_came_over_usb, for things only someone holding the board may do:
 * the request was sent to 192.168.7.1 AND comes from an address on the USB network.
 * A device on Wi-Fi can aim a request at 192.168.7.1, but it cannot also sit at a
 * 192.168.7.x address, because the board's answers to those go down the cable. */
static bool request_is_from_usb_cable(httpd_req_t *req)
{
    uint32_t mask = ipaddr_addr(USB_NET_MASK_STR);
    uint32_t peer = request_peer_ip(req);
    return request_came_over_usb(req) && peer != 0 && (peer & mask) == (ipaddr_addr(USB_NET_IP_STR) & mask);
}

/* Was this request sent by the TikPrint page itself (or by a tool like curl)?
 * A browser adds an "Origin" header saying which site the page that sent the request came
 * from. If that is some other website, the request is refused: otherwise any web page open
 * on a computer near the board could quietly send it new firmware. curl sends no Origin. */
static bool request_is_same_site(httpd_req_t *req)
{
    char origin[160] = "", host[128] = "", expect[160];
    esp_err_t err = httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin));
    if (err == ESP_ERR_NOT_FOUND) {
        return true;
    }
    if (err != ESP_OK || httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }
    snprintf(expect, sizeof(expect), "http://%s", host);
    return strcmp(origin, expect) == 0;
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

/* The "settings" part of /api/status: what the ticket header shows (see settings.h). */
static void add_settings(cJSON *root)
{
    settings_t s;
    settings_get(&s);
    cJSON *set = cJSON_AddObjectToObject(root, "settings");
    cJSON_AddStringToObject(set, "location", s.location);
    cJSON_AddBoolToObject(set, "show_location", s.show_location);
    cJSON_AddBoolToObject(set, "show_date", s.show_date);
    cJSON_AddBoolToObject(set, "show_time", s.show_time);
    cJSON_AddStringToObject(set, "header_font", s.header_font);
}

/* The "automations" part of /api/status:
 *   {"weather":{"zip":"48104","place":"Ann Arbor, MI","busy":false,
 *               "last":{"ok":true,"error":"","message":"","ticket":42,"s_ago":12}}}
 * "zip" is "" until one is set. "place" is "" until the weather service has been asked about that ZIP code.
 * "busy" is true while a weather ticket is being fetched. "last" is how the latest one went (null if none
 * since the board started); when "ok" is false, "error" and "message" say why and there is no "ticket". */
static void add_automations(cJSON *root)
{
    automations_weather_status_t w;
    settings_places_t pl;
    settings_weather_look_t look;
    settings_get_places(&pl);
    settings_get_weather_look(&look);
    automations_get_weather_status(&w);
    const settings_place_t *now = pl.count ? &pl.place[pl.selected] : NULL;

    cJSON *weather = cJSON_AddObjectToObject(cJSON_AddObjectToObject(root, "automations"), "weather");
    // The selected place: "zip" is "" if it was added by town name, "place" is "" while a ZIP code's town is not known yet.
    cJSON_AddStringToObject(weather, "zip", now ? now->zip : "");
    cJSON_AddStringToObject(weather, "place", now ? now->name : "");
    // All saved places, and which one is selected (-1 = there are none).
    cJSON *places = cJSON_AddArrayToObject(weather, "places");
    for (int i = 0; i < pl.count; i++) {
        cJSON *place = cJSON_CreateObject();
        cJSON_AddStringToObject(place, "name", pl.place[i].name);
        cJSON_AddStringToObject(place, "zip", pl.place[i].zip);
        cJSON_AddItemToArray(places, place);
    }
    cJSON_AddNumberToObject(weather, "selected", now ? pl.selected : -1);
    cJSON_AddNumberToObject(weather, "max_places", SETTINGS_PLACES_MAX);
    // How the ticket looks: font ("Printer" = the printer's built-in font), size 0-2, hour-by-hour list or not.
    cJSON *jlook = cJSON_AddObjectToObject(weather, "look");
    cJSON_AddStringToObject(jlook, "font", look.font);
    cJSON_AddNumberToObject(jlook, "size", look.size);
    cJSON_AddBoolToObject(jlook, "hours", look.hours);
    cJSON_AddBoolToObject(weather, "busy", w.busy);
    // Looking up the ZIP code's town: "locating" = still trying, "locate_error" = why the last try failed ("" = it has not).
    cJSON_AddBoolToObject(weather, "locating", w.locating);
    cJSON_AddStringToObject(weather, "locate_error", w.locate_error);
    cJSON_AddStringToObject(weather, "locate_message", w.locate_message);
    // The schedule: "on" is the whole thing; each slot has a time of day ("07:30", 24-hour) and its own switch.
    settings_weather_schedule_t sched;
    settings_get_weather_schedule(&sched);
    cJSON *schedule = cJSON_AddObjectToObject(weather, "schedule");
    cJSON_AddBoolToObject(schedule, "on", sched.on);
    cJSON_AddNumberToObject(schedule, "max_per_day", AUTOMATIONS_WEATHER_MAX_PER_DAY);
    cJSON *slots = cJSON_AddArrayToObject(schedule, "slots");
    static const weather_kind_t SLOT_KIND[SETTINGS_WEATHER_SLOTS] = { WEATHER_TODAY, WEATHER_REST_OF_TODAY, WEATHER_TOMORROW };
    for (int i = 0; i < SETTINGS_WEATHER_SLOTS; i++) {
        char at[8];
        snprintf(at, sizeof(at), "%02d:%02d", sched.slot[i].minute / 60, sched.slot[i].minute % 60);
        cJSON *slot = cJSON_CreateObject();
        cJSON_AddStringToObject(slot, "time", at);
        cJSON_AddBoolToObject(slot, "on", sched.slot[i].on);
        cJSON_AddStringToObject(slot, "kind", weather_kind_name(SLOT_KIND[i]));
        cJSON_AddItemToArray(slots, slot);
    }
    if (!w.ran) {
        cJSON_AddNullToObject(weather, "last");
        return;
    }
    cJSON *last = cJSON_AddObjectToObject(weather, "last");
    cJSON_AddBoolToObject(last, "ok", w.ok);
    cJSON_AddStringToObject(last, "error", w.error);
    cJSON_AddStringToObject(last, "message", w.message);
    if (w.ok) {
        cJSON_AddNumberToObject(last, "ticket", w.ticket);
    }
    cJSON_AddNumberToObject(last, "s_ago", w.s_ago);
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
    add_settings(root);
    add_automations(root);
    // Firmware: "id" changes with every build, "slot" is where it runs from, "boot_id" is a new number
    // after every restart. The rest is about updates from GitHub (see fw_github.h): "latest" is the newest
    // release ("" if not known), "available" says it is newer than this one, "state" is what the board is
    // doing about it (idle, checking, downloading, restarting), "progress" is the download in percent.
    const esp_app_desc_t *app = esp_app_get_description();
    char built[40];
    snprintf(built, sizeof(built), "%s %s", app->date, app->time);
    cJSON_AddNumberToObject(root, "boot_id", fw_update_boot_id());
    cJSON *upd = cJSON_AddObjectToObject(root, "update");
    cJSON_AddStringToObject(upd, "id", esp_app_get_elf_sha256_str());
    cJSON_AddStringToObject(upd, "built", built);
    cJSON_AddStringToObject(upd, "slot", fw_update_slot());
    cJSON_AddBoolToObject(upd, "on_trial", fw_update_on_trial());
    cJSON_AddNumberToObject(upd, "max_bytes", fw_update_max_size());
    fw_github_status_t gh;
    fw_github_get_status(&gh);
    cJSON_AddStringToObject(upd, "state", fw_github_state_name(gh.state));
    cJSON_AddStringToObject(upd, "latest", gh.latest);
    cJSON_AddBoolToObject(upd, "available", gh.available);
    cJSON_AddNumberToObject(upd, "progress", gh.progress);
    cJSON_AddStringToObject(upd, "error", gh.error);
    if (gh.checked) {
        cJSON_AddNumberToObject(upd, "checked_s_ago", gh.checked_s_ago);
    } else {
        cJSON_AddNullToObject(upd, "checked_s_ago");
    }
    cJSON_AddNumberToObject(root, "free_heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return send_json(req, root);
}

/* ---------- POST /api/settings ----------
 * The ticket header: {"location": "Los Angeles", "show_location": true, "show_date": true, "show_time": true,
 *                     "header_font": "Inter"}.
 * Send only what you want to change; the rest stays as it is.
 * Reply: {"ok":true,"settings":{...all four...}}, or {"ok":false,"error":"bad_setting",...}. */
static esp_err_t settings_post(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!body) {
        return ESP_OK;
    }
    settings_t s;
    settings_get(&s);
    const cJSON *jloc = cJSON_GetObjectItem(body, "location");
    const cJSON *jshow_loc = cJSON_GetObjectItem(body, "show_location");
    const cJSON *jshow_date = cJSON_GetObjectItem(body, "show_date");
    const cJSON *jshow_time = cJSON_GetObjectItem(body, "show_time");
    const cJSON *jfont = cJSON_GetObjectItem(body, "header_font");
    bool too_long = (cJSON_IsString(jloc) && strlen(jloc->valuestring) >= sizeof(s.location)) ||
                    (cJSON_IsString(jfont) && strlen(jfont->valuestring) >= sizeof(s.header_font));
    if (cJSON_IsString(jloc) && !too_long) {
        strlcpy(s.location, jloc->valuestring, sizeof(s.location));
    }
    if (cJSON_IsString(jfont) && !too_long) {
        strlcpy(s.header_font, jfont->valuestring, sizeof(s.header_font));
    }
    s.show_location = cJSON_IsBool(jshow_loc) ? cJSON_IsTrue(jshow_loc) : s.show_location;
    s.show_date = cJSON_IsBool(jshow_date) ? cJSON_IsTrue(jshow_date) : s.show_date;
    s.show_time = cJSON_IsBool(jshow_time) ? cJSON_IsTrue(jshow_time) : s.show_time;
    cJSON_Delete(body);

    bool ok = !too_long && settings_set(&s) == ESP_OK;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", ok);
    if (!ok) {
        cJSON_AddStringToObject(root, "error", "bad_setting");
        cJSON_AddStringToObject(root, "message", "The location or font name is too long or not plain text");
    }
    add_settings(root);
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
 *                     "text_in_image": true,                  (optional, see below)
 *                     "parts": [...]}                         (optional, see below)
 *   3. if "image" is present: H * 72 bytes of raw image bits.
 *      "text_in_image" means the page drew the text at the top of the image, in its own
 *      font. title/body/source are then only kept for the log and /api/last-ticket.
 *      "parts" is for tickets with lines in the printer's built-in font (the page's
 *      "Printer default" font): the ticket top to bottom, as image rows and lines of
 *      text in turn (see build_layout). Those lines are then not in the image.
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

/*
 * Turn the "parts" of a ticket laid out by the page into the bytes the printer driver reads
 * (see "Layout" in printer.h). Each part is one of:
 *   {"rows": 120}                                             the next 120 rows of the image
 *   {"size": 2, "runs": [{"text": "Total", "bold": true},     one line in the printer's built-in font,
 *                        {"text": " 5.00", "underline": true}]}   size = 1..8 times its normal size
 * Returns NULL if the parts are not in that form, or ask for more image rows than there are.
 * The result never needs more than max_len bytes (it is shorter than the JSON it came from). free() it.
 */
static uint8_t *build_layout(const cJSON *parts, int image_rows, size_t max_len, size_t *len_out)
{
    uint8_t *out = heap_caps_malloc(max_len, MALLOC_CAP_SPIRAM);
    if (!out) {
        return NULL;
    }
    size_t n = 0;
    int rows_used = 0;
    const cJSON *part;
    cJSON_ArrayForEach(part, parts) {
        const cJSON *jrows = cJSON_GetObjectItem(part, "rows");
        const cJSON *jruns = cJSON_GetObjectItem(part, "runs");
        if (cJSON_IsNumber(jrows)) {
            int rows = jrows->valueint;
            if (rows < 1 || rows > image_rows - rows_used || n + 3 > max_len) {
                goto bad;
            }
            rows_used += rows;
            out[n++] = LAYOUT_ROWS;
            out[n++] = rows & 0xFF;
            out[n++] = rows >> 8;
            continue;
        }
        const cJSON *jsize = cJSON_GetObjectItem(part, "size");
        int size = cJSON_IsNumber(jsize) ? jsize->valueint : 1;
        int runs = cJSON_IsArray(jruns) ? cJSON_GetArraySize(jruns) : -1;
        if (size < 1 || size > LAYOUT_MAX_SIZE || runs < 0 || runs > 255 || n + 3 > max_len) {
            goto bad;
        }
        out[n++] = LAYOUT_LINE;
        out[n++] = (uint8_t)size;
        out[n++] = (uint8_t)runs;
        const cJSON *run;
        cJSON_ArrayForEach(run, jruns) {
            const cJSON *jtext = cJSON_GetObjectItem(run, "text");
            size_t len = cJSON_IsString(jtext) ? strlen(jtext->valuestring) : SIZE_MAX;
            if (len > LAYOUT_MAX_RUN || n + 2 + len > max_len) {
                goto bad;
            }
            out[n++] = (cJSON_IsTrue(cJSON_GetObjectItem(run, "bold")) ? LAYOUT_BOLD : 0) |
                       (cJSON_IsTrue(cJSON_GetObjectItem(run, "underline")) ? LAYOUT_UNDERLINE : 0);
            out[n++] = (uint8_t)len;
            for (size_t i = 0; i < len; i++) {
                // Only printable text reaches the printer: a control character (a line break, or
                // the ESC that starts a printer command) becomes a space.
                uint8_t c = (uint8_t)jtext->valuestring[i];
                out[n++] = (c < 0x20 || c == 0x7F) ? ' ' : c;
            }
        }
    }
    *len_out = n;
    return out;

bad:
    free(out);
    return NULL;
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
    uint8_t *layout = NULL;
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
    const cJSON *jparts = cJSON_GetObjectItem(meta, "parts");
    if (cJSON_IsArray(jparts) && cJSON_GetArraySize(jparts) > 0) {
        layout = build_layout(jparts, job.image_rows, meta_len, &job.layout_len);
        if (!layout) {
            ret = print_fail(req, "bad_layout", "The ticket's layout could not be read");
            goto out;
        }
        job.layout = layout;
        job.text_in_image = true;   // the page laid the whole ticket out, text included
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
    free(layout);
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

/* Say no: {"ok":false,"error":"...","message":"..."} with an HTTP status to match. */
static esp_err_t refuse(httpd_req_t *req, const char *http_status, const char *error, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    httpd_resp_set_status(req, http_status);
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", error);
    cJSON_AddStringToObject(root, "message", message);
    return send_json(req, root);
}

/* ---------- POST /api/update/check ----------
 * Ask GitHub for the latest release. Replies {"ok":true} at once; the answer arrives a
 * second or two later in /api/status ("update": state, latest, available, error). */
static esp_err_t update_check_post(httpd_req_t *req)
{
    if (!request_is_same_site(req)) {
        return refuse(req, "403 Forbidden", "other_site", "This request did not come from the TikPrint page");
    }
    if (fw_github_check_now() != ESP_OK) {
        return refuse(req, "409 Conflict", "busy", "The printer is already checking or updating");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

/* ---------- POST /api/update/install ----------
 * Download the latest release from GitHub and restart into it. Only does anything if a
 * check found a release newer than the running firmware. Replies {"ok":true} at once;
 * follow the download in /api/status ("update": state, progress, error). */
static esp_err_t update_install_post(httpd_req_t *req)
{
    if (!request_is_same_site(req)) {
        return refuse(req, "403 Forbidden", "other_site", "This request did not come from the TikPrint page");
    }
    int waiting;
    bool printing, paused;
    printer_queue_info(&waiting, &printing, &paused);
    if (printing || waiting > 0) {
        return refuse(req, "409 Conflict", "busy", "Tickets are still printing or waiting. Update when the queue is empty");
    }
    if (fw_update_on_trial()) {
        return refuse(req, "409 Conflict", "on_trial", "The printer was updated a moment ago. Try again in a few seconds");
    }
    esp_err_t err = fw_github_install();
    if (err == ESP_ERR_NOT_FOUND) {
        return refuse(req, "409 Conflict", "up_to_date", "There is no newer firmware to install. Check for updates first");
    }
    if (err != ESP_OK) {
        return refuse(req, "409 Conflict", "busy", "The printer is already checking or updating");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

/* ---------- POST /api/update ----------
 * Install firmware from a file, for trying a build that is not a release yet.
 * The body is the firmware file itself (build/tikprint_firmware.bin):
 *
 *   curl --data-binary @build/tikprint_firmware.bin http://192.168.7.1/api/update
 *
 * This only works over the USB cable. Over Wi-Fi the only firmware the board accepts is
 * a release from GitHub (see fw_github.c), so nobody on the network can hand it anything else.
 *
 * The file is written to the firmware slot that is not running, checked, and then the board
 * restarts into it (see fw_update.c). The reply comes just before the restart:
 *   {"ok":true,"bytes":1832848,"slot":"ota_0","message":"Update installed. Restarting"}
 * On any failure the running firmware is untouched:
 *   {"ok":false,"error":"...","message":"..."}
 *   error: other_site, usb_only, bad_size, busy, not_firmware, on_trial, bad_file, failed
 * The web server handles one request at a time, so the board answers nothing else during
 * the upload. */
static uint8_t s_update_buf[UPDATE_CHUNK];

/* Read up to want bytes of the upload. Returns how many arrived, or -1 if the upload broke off. */
static int update_recv(httpd_req_t *req, uint8_t *buf, size_t want)
{
    for (int stalls = 0; stalls < UPDATE_MAX_STALLS; stalls++) {
        int n = httpd_req_recv(req, (char *)buf, want);
        if (n > 0) {
            return n;
        }
        if (n != HTTPD_SOCK_ERR_TIMEOUT) {
            return -1;   // the connection dropped
        }
    }
    return -1;           // nothing for too long
}

/* Say no to an upload, then read and throw away what is still coming, so the sender
 * gets to see the answer instead of a broken connection. */
static esp_err_t update_refuse(httpd_req_t *req, size_t left, cJSON *root)
{
    esp_err_t err = send_json(req, root);
    while (err == ESP_OK && left > 0) {
        int n = update_recv(req, s_update_buf, left < UPDATE_CHUNK ? left : UPDATE_CHUNK);
        if (n <= 0) {
            return ESP_FAIL;
        }
        left -= n;
    }
    return err;
}

static esp_err_t update_fail(httpd_req_t *req, size_t left, const char *http_status, const char *error,
                             const char *message)
{
    ESP_LOGW(TAG, "firmware update refused: %s", error);
    cJSON *root = cJSON_CreateObject();
    httpd_resp_set_status(req, http_status);
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", error);
    cJSON_AddStringToObject(root, "message", message);
    return update_refuse(req, left, root);
}

static esp_err_t update_post(httpd_req_t *req)
{
    size_t len = req->content_len, left = len;
    ESP_LOGI(TAG, "firmware update offered: %u bytes", (unsigned)len);

    if (!request_is_same_site(req)) {
        return update_fail(req, left, "403 Forbidden", "other_site", "This request did not come from the TikPrint page");
    }

    // 1. Only over the USB cable (see the note above).
    if (!request_is_from_usb_cable(req)) {
        return update_fail(req, left, "403 Forbidden", "usb_only",
                           "A firmware file can only be installed over the USB cable, at http://" USB_NET_IP_STR
                           ". Over Wi-Fi, use Check for updates");
    }

    // 2. Is it a sensible moment, and a sensible size?
    if (len < FW_UPDATE_HEADER_BYTES || len > fw_update_max_size()) {
        return update_fail(req, left, "400 Bad Request", "bad_size", "That file is too small or too large to be firmware");
    }
    int waiting;
    bool printing, paused;
    printer_queue_info(&waiting, &printing, &paused);
    if (printing || waiting > 0) {
        return update_fail(req, left, "409 Conflict", "busy",
                           "Tickets are still printing or waiting. Update when the queue is empty");
    }
    fw_github_status_t gh;
    fw_github_get_status(&gh);
    if (gh.state != FW_GITHUB_IDLE) {
        return update_fail(req, left, "409 Conflict", "busy", "The printer is already checking or updating");
    }

    // 3. The first piece of the file says what it is: refuse anything that is not TikPrint firmware.
    size_t got = 0, first = len < UPDATE_CHUNK ? len : UPDATE_CHUNK;
    while (got < first) {
        int n = update_recv(req, s_update_buf + got, first - got);
        if (n <= 0) {
            ESP_LOGW(TAG, "firmware upload broke off");
            return ESP_FAIL;
        }
        got += n;
    }
    left -= got;
    if (!fw_update_header_ok(s_update_buf, got)) {
        return update_fail(req, left, "400 Bad Request", "not_firmware",
                           "That is not a TikPrint firmware file (use build/tikprint_firmware.bin)");
    }

    // 4. Write it to the free slot as it arrives.
    esp_err_t err = fw_update_begin();
    if (err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
        return update_fail(req, left, "409 Conflict", "on_trial",
                           "The printer was updated a moment ago. Try again in a few seconds");
    }
    if (err != ESP_OK) {
        return update_fail(req, left, "500 Internal Server Error", "failed",
                           "The printer has no free firmware slot. Flash it once with the USB cable");
    }
    size_t written = 0, next_log = UPDATE_LOG_EVERY;
    while (true) {
        err = fw_update_write(s_update_buf, got);
        if (err != ESP_OK) {
            fw_update_abort();
            return update_fail(req, left, "400 Bad Request", "bad_file", "The printer could not write that file");
        }
        written += got;
        if (written >= next_log) {
            ESP_LOGI(TAG, "firmware update: %u of %u KB", (unsigned)(written / 1024), (unsigned)(len / 1024));
            next_log += UPDATE_LOG_EVERY;
        }
        if (left == 0) {
            break;
        }
        int n = update_recv(req, s_update_buf, left < UPDATE_CHUNK ? left : UPDATE_CHUNK);
        if (n <= 0) {
            fw_update_abort();
            ESP_LOGW(TAG, "firmware upload broke off after %u bytes", (unsigned)written);
            return ESP_FAIL;   // nobody is left to answer
        }
        got = n;
        left -= n;
    }

    // 5. Check the whole file, select it, answer, restart.
    err = fw_update_finish();
    if (err != ESP_OK) {
        fw_update_abort();
        return update_fail(req, 0, "400 Bad Request", "bad_file",
                           "The file arrived damaged or is not complete. Nothing was changed");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "bytes", written);
    cJSON_AddStringToObject(root, "message", "Update installed. Restarting");
    esp_err_t sent = send_json(req, root);
    fw_update_restart_soon();
    return sent;
}

/* ---------- POST /api/automations/weather ----------
 * Changes the weather settings. Send only what should change:
 *   places     {"zip": "48104"} or {"city": "Austin, TX"}  add a place and select it
 *              {"select": 1}   select a place from the list     {"remove": 1}   take one off the list
 *   schedule   {"on": true}    {"slots": [{"time": "07:30", "on": true}, ...three of them]}
 *   look       {"font": "Inter", "size": 1, "hours": true}
 * A ZIP code or town is looked up in the tables inside the firmware (no internet needed); a ZIP
 * code's town name is then fetched in the background and appears in /api/status a few seconds later.
 * Reply: {"ok":true,"automations":{...as in /api/status}}, or {"ok":false,"error":"bad_zip","message":"..."}. */
static esp_err_t weather_post(httpd_req_t *req)
{
    if (!request_is_same_site(req)) {
        return refuse(req, "403 Forbidden", "other_site", "This request did not come from the TikPrint page");
    }
    cJSON *body = read_json_body(req);
    if (!body) {
        return ESP_OK;
    }
    // Every part is optional: only what is sent is changed.
    const char *problem = NULL, *problem_text = NULL;

    // Places: {"zip": "48104"} or {"city": "Austin, TX"} adds a place and selects it (or just selects it if it
    // is already there), {"select": 1} selects one from the list, {"remove": 1} takes one off the list.
    const cJSON *jzip = cJSON_GetObjectItem(body, "zip"), *jcity = cJSON_GetObjectItem(body, "city");
    const cJSON *jselect = cJSON_GetObjectItem(body, "select"), *jremove = cJSON_GetObjectItem(body, "remove");
    settings_places_t pl;
    settings_get_places(&pl);
    bool places_changed = false;
    if (jzip || jcity) {
        settings_place_t add;
        city_t city;
        int lat_e2 = 0, lon_e2 = 0;
        memset(&add, 0, sizeof(add));
        if (jzip && cJSON_IsString(jzip) && zip_table_lookup(jzip->valuestring, &lat_e2, &lon_e2)) {
            strlcpy(add.zip, jzip->valuestring, sizeof(add.zip));
            add.lat_e2 = (int16_t)lat_e2;
            add.lon_e2 = (int16_t)lon_e2;
        } else if (!jzip && cJSON_IsString(jcity) && city_table_find(jcity->valuestring, &city)) {
            strlcpy(add.name, city.name, sizeof(add.name));
            add.lat_e2 = (int16_t)city.lat_e2;
            add.lon_e2 = (int16_t)city.lon_e2;
        } else {
            problem = jzip ? "bad_zip" : "bad_city";
            problem_text = jzip ? "That is not a US ZIP code the printer knows" : "That is not a US town the printer knows";
        }
        int at = -1;
        for (int i = 0; !problem && i < pl.count; i++) {
            bool same = add.zip[0] ? strcmp(pl.place[i].zip, add.zip) == 0
                                   : !pl.place[i].zip[0] && strcmp(pl.place[i].name, add.name) == 0;
            at = same ? i : at;
        }
        if (!problem && at < 0 && pl.count == SETTINGS_PLACES_MAX) {
            problem = "places_full";
            problem_text = "The list of places is full. Remove one to add another";
        } else if (!problem) {
            if (at < 0) {
                at = pl.count++;
                pl.place[at] = add;
            }
            pl.selected = (uint8_t)at;
            places_changed = true;
        }
    }
    if (!problem && (jselect || jremove)) {
        const cJSON *which = jselect ? jselect : jremove;
        if (!cJSON_IsNumber(which) || which->valueint < 0 || which->valueint >= pl.count) {
            problem = "bad_place";
            problem_text = "That place is not on the list";
        } else if (jselect) {
            pl.selected = (uint8_t)which->valueint;
            places_changed = true;
        } else {
            int gone = which->valueint;
            for (int i = gone; i + 1 < pl.count; i++) {
                pl.place[i] = pl.place[i + 1];
            }
            pl.count--;
            pl.selected = pl.selected > gone ? pl.selected - 1 : pl.selected;   // keep pointing at the same place
            pl.selected = pl.selected >= pl.count ? 0 : pl.selected;            // the selected one was removed: the first is next
            places_changed = true;
        }
    }

    // The look: {"font": "Inter", "size": 1, "hours": true}
    const cJSON *jfont = cJSON_GetObjectItem(body, "font"), *jsize = cJSON_GetObjectItem(body, "size");
    const cJSON *jhours = cJSON_GetObjectItem(body, "hours");
    settings_weather_look_t look;
    settings_get_weather_look(&look);
    if (!problem && (jfont || jsize || jhours)) {
        bool font_ok = !jfont || (cJSON_IsString(jfont) && (ticket_font_id(jfont->valuestring) >= 0 ||
                                                              strcmp(jfont->valuestring, "Printer") == 0));
        bool size_ok = !jsize || (cJSON_IsNumber(jsize) && jsize->valueint >= 0 && jsize->valueint <= 2);
        if (!font_ok || !size_ok || (jhours && !cJSON_IsBool(jhours))) {
            problem = "bad_look";
            problem_text = "That is not a font or size the printer has";
        } else {
            if (jfont) {
                strlcpy(look.font, jfont->valuestring, sizeof(look.font));
            }
            look.size = jsize ? (uint8_t)jsize->valueint : look.size;
            look.hours = jhours ? cJSON_IsTrue(jhours) : look.hours;
        }
    }

    const cJSON *jon = cJSON_GetObjectItem(body, "on"), *jslots = cJSON_GetObjectItem(body, "slots");
    // The schedule: {"on": true, "slots": [{"time": "07:30", "on": true}, ... one per slot]}
    settings_weather_schedule_t was, sched;
    settings_get_weather_schedule(&was);
    sched = was;
    bool sched_ok = (!jon || cJSON_IsBool(jon)) && (!jslots || cJSON_GetArraySize(jslots) == SETTINGS_WEATHER_SLOTS);
    if (jon && sched_ok) {
        sched.on = cJSON_IsTrue(jon);
    }
    for (int i = 0; jslots && sched_ok && i < SETTINGS_WEATHER_SLOTS; i++) {
        const cJSON *slot = cJSON_GetArrayItem(jslots, i);
        const cJSON *at = cJSON_GetObjectItem(slot, "time"), *on = cJSON_GetObjectItem(slot, "on");
        int hour, minute;
        char extra;
        if (at) {
            sched_ok = cJSON_IsString(at) && sscanf(at->valuestring, "%2d:%2d%c", &hour, &minute, &extra) == 2 &&
                       hour >= 0 && hour < 24 && minute >= 0 && minute < 60;
            if (sched_ok) {
                sched.slot[i].minute = (uint16_t)(hour * 60 + minute);
            }
        }
        if (on && sched_ok) {
            sched_ok = cJSON_IsBool(on);
            sched.slot[i].on = cJSON_IsTrue(on);
        }
    }
    cJSON_Delete(body);
    if (!problem && !sched_ok) {
        problem = "bad_schedule";
        problem_text = "That is not a schedule the printer understands";
    }
    if (problem) {   // nothing is changed unless everything sent was right
        return refuse(req, "400 Bad Request", problem, problem_text);
    }
    settings_set_weather_schedule(&sched);
    settings_set_weather_look(&look);
    if (places_changed) {
        settings_set_places(&pl);
    }
    for (int i = 0; i < SETTINGS_WEATHER_SLOTS; i++) {
        if (sched.slot[i].minute != was.slot[i].minute) {
            automations_weather_slot_changed(i);   // a slot moved to a new time may print again today
        }
    }
    if (places_changed) {
        automations_weather_zip_changed();   // look up the selected place's forecast address (and town) in the background
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    add_automations(root);
    return send_json(req, root);
}

/* ---------- POST /api/automations/places/search ----------
 * Towns to suggest while a place is being typed: {"q": "aust"} ->
 *   {"ok":true,"matches":[{"name":"Austin, TX"},{"name":"Austin, MN"},...]}   (the biggest first, at most 6)
 * Looked up in a table on the board, with no internet. Add one with POST /api/automations/weather {"city": name}. */
static esp_err_t places_search_post(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!body) {
        return ESP_OK;
    }
    const cJSON *q = cJSON_GetObjectItem(body, "q");
    city_t found[6];
    int n = cJSON_IsString(q) ? city_table_search(q->valuestring, found, 6) : 0;
    cJSON_Delete(body);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON *matches = cJSON_AddArrayToObject(root, "matches");
    for (int i = 0; i < n; i++) {
        cJSON *match = cJSON_CreateObject();
        cJSON_AddStringToObject(match, "name", found[i].name);
        cJSON_AddItemToArray(matches, match);
    }
    return send_json(req, root);
}

/* ---------- POST /api/automations/weather/run-slot ----------
 * For testing the schedule without waiting for the clock: {"slot": 0} runs the first scheduled
 * print right now (0, 1 or 2). It does not count towards the daily limit. Not used by the page:
 *   curl -X POST -H 'Content-Type: application/json' -d '{"slot":0}' http://tikprint.local/api/automations/weather/run-slot */
static esp_err_t weather_run_slot_post(httpd_req_t *req)
{
    if (!request_is_same_site(req)) {
        return refuse(req, "403 Forbidden", "other_site", "This request did not come from the TikPrint page");
    }
    cJSON *body = read_json_body(req);
    if (!body) {
        return ESP_OK;
    }
    const cJSON *jslot = cJSON_GetObjectItem(body, "slot");
    esp_err_t err = cJSON_IsNumber(jslot) ? automations_weather_run_slot(jslot->valueint) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(body);
    if (err == ESP_ERR_INVALID_ARG) {
        return refuse(req, "400 Bad Request", "bad_slot", "The slot must be 0, 1 or 2");
    }
    if (err != ESP_OK) {
        return refuse(req, "409 Conflict", "busy", "The printer is still busy with an earlier request");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

/* ---------- POST /api/automations/weather/print-now ----------
 * Fetch the weather and print it now. The body is optional: {"kind": "today"} (the default),
 * {"kind": "rest"} (rest of today) or {"kind": "tomorrow"}.
 * Replies {"ok":true} at once; the ticket follows a few seconds later, and how it went shows
 * up in /api/status ("automations": "weather": "busy", then "last").
 * This is a manual print, so it does not count towards the limit on scheduled prints. */
static esp_err_t weather_print_now_post(httpd_req_t *req)
{
    if (!request_is_same_site(req)) {
        return refuse(req, "403 Forbidden", "other_site", "This request did not come from the TikPrint page");
    }
    weather_kind_t kind = WEATHER_TODAY;
    if (req->content_len > 0) {
        cJSON *body = read_json_body(req);
        if (!body) {
            return ESP_OK;
        }
        const cJSON *jkind = cJSON_GetObjectItem(body, "kind");
        const char *name = cJSON_IsString(jkind) ? jkind->valuestring : "today";
        bool known = false;
        for (weather_kind_t k = WEATHER_TODAY; k <= WEATHER_TOMORROW; k++) {
            if (strcmp(name, weather_kind_name(k)) == 0) {
                kind = k;
                known = true;
            }
        }
        cJSON_Delete(body);
        if (!known) {
            return refuse(req, "400 Bad Request", "bad_kind", "Use \"today\", \"rest\" or \"tomorrow\"");
        }
    }
    settings_place_t place;
    if (!settings_get_place(&place)) {
        return refuse(req, "409 Conflict", "no_zip", weather_result_message(WEATHER_NO_ZIP));
    }
    if (automations_weather_print_now(kind) != ESP_OK) {
        return refuse(req, "409 Conflict", "busy", "The printer is still getting the last weather ticket");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

void api_register(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        { .uri = "/api/ping",         .method = HTTP_GET,  .handler = ping_get },
        { .uri = "/api/status",       .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/time",         .method = HTTP_POST, .handler = time_post },
        { .uri = "/api/settings",     .method = HTTP_POST, .handler = settings_post },
        { .uri = "/api/diag/ping",    .method = HTTP_GET,  .handler = diag_ping_get },
        { .uri = "/api/diag/printer-test", .method = HTTP_GET, .handler = diag_printer_test_get },
        { .uri = "/api/diag/printer-status", .method = HTTP_GET, .handler = diag_printer_status_get },
        { .uri = "/api/wifi/scan",    .method = HTTP_GET,  .handler = wifi_scan_get },
        { .uri = "/api/wifi/connect", .method = HTTP_POST, .handler = wifi_connect_post },
        { .uri = "/api/wifi/forget",  .method = HTTP_POST, .handler = wifi_forget_post },
        { .uri = "/api/print",        .method = HTTP_POST, .handler = print_post },
        { .uri = "/api/last-ticket",  .method = HTTP_GET,  .handler = last_ticket_get },
        { .uri = "/api/last-ticket.pbm", .method = HTTP_GET, .handler = last_ticket_pbm_get },
        { .uri = "/api/update/check", .method = HTTP_POST, .handler = update_check_post },
        { .uri = "/api/update/install", .method = HTTP_POST, .handler = update_install_post },
        { .uri = "/api/update",       .method = HTTP_POST, .handler = update_post },
        { .uri = "/api/automations/weather", .method = HTTP_POST, .handler = weather_post },
        { .uri = "/api/automations/weather/print-now", .method = HTTP_POST, .handler = weather_print_now_post },
        { .uri = "/api/automations/places/search", .method = HTTP_POST, .handler = places_search_post },
        { .uri = "/api/automations/weather/run-slot", .method = HTTP_POST, .handler = weather_run_slot_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
}
