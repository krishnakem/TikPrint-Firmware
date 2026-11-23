/*
 * Listening on Supabase Realtime (see cloud_realtime.h). The contract is docs/device-api.md in the
 * TikPrint Cloud repo, section "Realtime".
 *
 * Three tasks take part:
 *   "cloud_rt"   this file's own task. It decides what happens next: ask for a session, open the
 *                socket, join, send the heartbeats, write the printer's row, close and try again.
 *   "cloud_ws"   made by the WebSocket library for as long as a socket is open. It receives the
 *                messages and calls on_ws_event() with each. It only reads the message and passes
 *                the news on; it never closes the socket itself.
 *   "cloud"      cloud.c. It makes the calls that need the device token, among them the one that
 *                fetches a session for this task (cloud_realtime_wants_session / ..._session).
 *
 * A life of the socket, as this task sees it (s_phase):
 *
 *   waiting ──(time to try, Wi-Fi up)──> asking ──(session)──> connecting ──(open)──> joining
 *      ^                                    │ 503 realtime_off                            │ both "ok"
 *      │                                    v                                             v
 *      └──(closed, failed, no heartbeat)── off (an hour) <──────────────────────────── listening
 *
 * "Listening" needs two messages from the other side: the reply to the join ("phx_reply", status
 * ok) and "system", status ok (the subscription to the table is in place).
 *
 * While listening:
 *   every 25 s        a heartbeat on the socket. No reply within 10 s: the socket is dead.
 *   every heartbeat_s the printer's own row is written (PATCH), which is what the website shows as
 *                     "online". An empty answer means "not linked any more": cloud.c checks in at
 *                     once, and the 401 it gets is what sends the printer back to local only.
 *   at 80% of the token's life   a fresh token is asked for and handed to the open socket.
 *
 * When the socket is lost, cloud.c checks in at once (without "X-Realtime", so the site goes back to
 * a short interval), and this task tries again after 5 s, then 10, 20, ... up to 5 minutes, with a
 * fresh session each time. Once listening again, cloud.c checks in once more (with "X-Realtime: 1"):
 * whatever was missed is in that answer.
 *
 * Memory: the session, the message being received and the texts being sent live in one block in
 * the big external RAM (PSRAM), taken when the task starts and wiped and given back when it ends.
 */
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cloud.h"
#include "cloud_realtime.h"
#include "cloud_update.h"
#include "wifi_mgr.h"

static const char *TAG = "cloud_rt";

#define TASK_STACK        12288   // HTTPS (writing the row) needs a deep stack
#define TASK_PRIORITY     3       // the same as the cloud task: below the web server, the print queue and the automations
#define WS_TASK_STACK     12288   // the library's task: the secure connection is set up on it (a deep stack), and messages are read on it
#define WS_BUFFER         2048    // a message arrives in pieces of this size at most
#define RX_MAX            8192    // the biggest message accepted (a command is under 1000 bytes)
#define TICK_MS           1000    // the task looks around this often when nothing wakes it
#define RETRY_FIRST_S     5       // after a socket is lost: try again after this long,
#define RETRY_MAX_S       300     // ... twice as long each time, up to this
#define OFF_RETRY_S       3600    // "realtime_off": ask again in an hour
#define ASK_AGAIN_S       60      // no answer from the cloud task about a session: remind it
#define CONNECT_WITHIN_S  25      // from opening the socket to "listening": longer than this is a failure
#define BEAT_EVERY_S      25      // the heartbeat on the socket
#define BEAT_REPLY_S      10      // ... and how long its reply may take
#define ROW_EVERY_S       60      // write the row this often, unless the cloud says otherwise ("heartbeat_s")
#define ROW_TIMEOUT_MS    8000
#define REFRESH_RETRY_S   60      // a fresh token could not be got: try again after this
#define REPORT_RETRY_S    5       // an update step could not be reported: try again after this

/* What on_ws_event() tells the task (task notification bits). */
#define N_WAKE       (1u << 0)   // look around
#define N_CONNECTED  (1u << 1)   // the socket is open
#define N_FAILED     (1u << 2)   // the socket closed, failed, or the other side refused
#define N_JOIN_OK    (1u << 3)   // the reply to the join: ok
#define N_SYSTEM_OK  (1u << 4)   // "system", status ok: subscribed to the table

typedef enum { PH_STOPPED, PH_WAITING, PH_ASKING, PH_CONNECTING, PH_JOINING, PH_LISTENING, PH_OFF } phase_t;

typedef enum {
    ROW_WRITTEN,      // 200 with the printer's id
    ROW_NOT_LINKED,   // 200 with []: nothing was written, the printer is not linked any more
    ROW_EXPIRED,      // 401: the access token ran out
    ROW_FAILED,       // anything else: try again at the next beat
} row_result_t;

/* Everything big, in one block of external RAM for as long as the task lives. */
typedef struct {
    cloud_realtime_session_t session;    // the one in use. Only the Realtime task touches it
    cloud_realtime_session_t incoming;   // the one the cloud task just handed over (guarded by s_lock)
    char rx[RX_MAX + 1];                 // the message being received. Only the library's task touches it
    char out[2048];                      // the text being sent, or the "Bearer ..." header
    char url[768];
} memory_t;

static SemaphoreHandle_t s_lock;         // guards the block below
static TaskHandle_t s_task;              // NULL = not running
static memory_t *s_mem;
static bool s_running, s_stop;
static phase_t s_phase;
static bool s_want_session;              // the cloud task should fetch a session
static bool s_have_incoming;             // ... and has answered:
static cloud_realtime_result_t s_incoming_result;
static char s_last_event[16];
static int64_t s_last_event_us;
static unsigned s_commands, s_reconnects;

/* Set before a socket is opened and not changed while it is open: the library's task reads them. */
static char s_topic[sizeof(((cloud_realtime_session_t *)0)->topic)];
static char s_printer_id[sizeof(((cloud_realtime_session_t *)0)->printer_id)];
static volatile int s_beat_ref;          // the "ref" of the heartbeat that is waiting for its reply
static volatile bool s_beat_waiting;

/* Only the Realtime task: */
static esp_websocket_client_handle_t s_ws;
static esp_http_client_handle_t s_rest;  // the connection the row is written on, kept open between writes
static char s_rest_body[96];             // the start of the answer to a row write
static size_t s_rest_len;
static int s_ref;                        // every message sent on a socket has a number of its own
/* Only the library's task: */
static size_t s_rx_len;
static bool s_rx_too_big;

/* ---------- Small things ---------- */

static void notify(uint32_t bits)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_running && s_task) {
        xTaskNotify(s_task, bits, eSetBits);
    }
    xSemaphoreGive(s_lock);
}

void cloud_realtime_wake(void)
{
    if (s_lock) {
        notify(N_WAKE);
    }
}

static void set_event(const char *what)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_last_event, what, sizeof(s_last_event));
    s_last_event_us = esp_timer_get_time();
    xSemaphoreGive(s_lock);
}

static void set_phase(phase_t phase)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_phase = phase;
    xSemaphoreGive(s_lock);
}

bool cloud_realtime_is_listening(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool listening = s_running && s_phase == PH_LISTENING;
    xSemaphoreGive(s_lock);
    return listening;
}

void cloud_realtime_get_status(cloud_realtime_status_t *out)
{
    static const char *const NAMES[] = { "stopped", "waiting", "asking", "connecting", "joining", "listening", "off" };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->running = s_running;
    out->listening = s_running && s_phase == PH_LISTENING;
    out->state = NAMES[s_running ? s_phase : PH_STOPPED];
    strlcpy(out->last_event, s_last_event, sizeof(out->last_event));
    out->last_event_s_ago = s_last_event[0] ? (int)((esp_timer_get_time() - s_last_event_us) / 1000000) : 0;
    out->commands = s_commands;
    out->reconnects = s_reconnects;
    xSemaphoreGive(s_lock);
}

static bool wifi_up(void)
{
    wifi_mgr_status_t wifi;
    wifi_mgr_get_status(&wifi);
    return wifi.connected;
}

static const char *text_of(const cJSON *json, const char *name)
{
    const cJSON *item = cJSON_GetObjectItem(json, name);
    return cJSON_IsString(item) ? item->valuestring : "";
}

/* ---------- The session ---------- */

/* Is this text there, and made only of letters, digits and the characters in `also`? Everything in
 * a session goes into an address, a header or a JSON text as it is, so nothing else is let through. */
static bool is_plain(const char *text, const char *also)
{
    if (!text[0]) {
        return false;
    }
    for (const char *p = text; *p; p++) {
        bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr(also, *p) != NULL;
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool session_is_usable(const cloud_realtime_session_t *s)
{
    return strncmp(s->supabase_url, "https://", 8) == 0 && is_plain(s->supabase_url + 8, ".-_:/") &&
           strncmp(s->realtime_url, "wss://", 6) == 0 && is_plain(s->realtime_url + 6, ".-_:/") &&
           is_plain(s->apikey, ".-_") &&
           is_plain(s->access_token, ".-_+/=~") &&
           is_plain(s->printer_id, "-") &&
           is_plain(s->topic, ":-_.") &&
           is_plain(s->table, "_") &&
           is_plain(s->filter, "=.-_");
}

bool cloud_realtime_wants_session(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool wants = s_running && s_want_session;
    xSemaphoreGive(s_lock);
    return wants;
}

void cloud_realtime_session(cloud_realtime_result_t result, const cloud_realtime_session_t *session)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_running && s_mem) {
        if (result == CLOUD_REALTIME_SESSION_OK && !(session && session_is_usable(session))) {
            ESP_LOGW(TAG, "the session the cloud sent cannot be used");   // what is in it is never logged
            result = CLOUD_REALTIME_SESSION_FAILED;
        }
        if (result == CLOUD_REALTIME_SESSION_OK) {
            s_mem->incoming = *session;
        }
        s_incoming_result = result;
        s_have_incoming = true;
        s_want_session = false;
        xTaskNotify(s_task, N_WAKE, eSetBits);
    }
    xSemaphoreGive(s_lock);
}

/* Ask the cloud task for a session. */
static void ask_for_session(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_want_session = true;
    xSemaphoreGive(s_lock);
    cloud_wake();
}

/* Has the cloud task answered? A good session becomes the one in use. */
static bool take_session_answer(cloud_realtime_result_t *result)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool have = s_have_incoming;
    if (have) {
        s_have_incoming = false;
        *result = s_incoming_result;
        if (*result == CLOUD_REALTIME_SESSION_OK) {
            s_mem->session = s_mem->incoming;
        }
        memset(&s_mem->incoming, 0, sizeof(s_mem->incoming));
    }
    xSemaphoreGive(s_lock);
    return have;
}

/* ---------- Messages from the socket (on the library's task) ---------- */

/* A command: one new row of the printer_commands table. Only payload.data.record is read. */
static void on_command(const cJSON *record)
{
    // Not this printer's (the cloud's own rules should never let that through), or empty: ignore.
    if (!cJSON_IsObject(record) || strcmp(text_of(record, "printer_id"), s_printer_id) != 0) {
        return;
    }
    const char *type = text_of(record, "type");
    const cJSON *payload = cJSON_GetObjectItem(record, "payload");
    if (strcmp(type, "print") == 0) {
        // The payload is the job, as a check-in lists it. cloud.c downloads, prints and reports it.
        const char *id = text_of(payload, "id");
        const cJSON *size = cJSON_GetObjectItem(payload, "size");
        cloud_print_command(id[0] ? id : text_of(record, "id"), cJSON_IsNumber(size) ? size->valueint : 0);
    } else if (strcmp(type, "update") == 0) {
        // A version and nothing else: no address is looked for, here or anywhere.
        cloud_update_command(text_of(record, "id"), text_of(payload, "version"));
    } else {
        return;   // a kind of command this firmware does not know
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_commands++;
    xSemaphoreGive(s_lock);
    set_event(type);
}

/* One whole message. */
static void on_message(const char *text, size_t len)
{
    cJSON *msg = cJSON_ParseWithLength(text, len);
    if (!msg) {
        return;
    }
    const char *event = text_of(msg, "event"), *topic = text_of(msg, "topic");
    const cJSON *payload = cJSON_GetObjectItem(msg, "payload");
    bool ours = strcmp(topic, s_topic) == 0;
    bool ok = strcmp(text_of(payload, "status"), "ok") == 0;
    if (strcmp(event, "phx_reply") == 0 && strcmp(topic, "phoenix") == 0) {
        if (atoi(text_of(msg, "ref")) == s_beat_ref) {
            s_beat_waiting = false;   // the heartbeat was answered
        }
    } else if (strcmp(event, "phx_reply") == 0 && ours && strcmp(text_of(msg, "ref"), "1") == 0) {
        if (!ok) {
            ESP_LOGW(TAG, "the join was refused");
        }
        notify(ok ? N_JOIN_OK : N_FAILED);
    } else if (strcmp(event, "system") == 0 && ours) {
        if (ok) {
            notify(N_SYSTEM_OK);
        } else if (strcmp(text_of(payload, "status"), "error") == 0) {
            ESP_LOGW(TAG, "the subscription was refused: %.120s", text_of(payload, "message"));   // the cloud's reason; no token is in it
            notify(N_FAILED);
        }
    } else if (strcmp(event, "postgres_changes") == 0 && ours) {
        on_command(cJSON_GetObjectItem(cJSON_GetObjectItem(payload, "data"), "record"));
    } else if ((strcmp(event, "phx_error") == 0 || strcmp(event, "phx_close") == 0) && ours) {
        notify(N_FAILED);
    }
    cJSON_Delete(msg);
}

/* Called by the WebSocket library, on its task, with everything that happens to the socket. */
static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const esp_websocket_event_data_t *d = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        notify(N_CONNECTED);
        break;
    case WEBSOCKET_EVENT_DATA:
        // A text message (1), or a further frame of one (0). A long message comes in several
        // pieces: put them together before reading it.
        if (d->op_code == 0x1 || d->op_code == 0x0) {
            if (d->op_code == 0x1 && d->payload_offset == 0) {
                s_rx_len = 0;
                s_rx_too_big = false;
            }
            if (s_rx_len + (size_t)d->data_len > RX_MAX) {
                s_rx_too_big = true;
            } else if (d->data_len > 0) {
                memcpy(s_mem->rx + s_rx_len, d->data_ptr, (size_t)d->data_len);
                s_rx_len += (size_t)d->data_len;
            }
            if (d->fin && d->payload_offset + d->data_len >= d->payload_len) {
                if (s_rx_too_big) {
                    // Too big to be a command as this firmware knows them. If it was one, the check-in finds it.
                    ESP_LOGW(TAG, "a message was too big to read: checking in instead");
                    cloud_poll_now();
                } else {
                    s_mem->rx[s_rx_len] = 0;
                    on_message(s_mem->rx, s_rx_len);
                }
                s_rx_len = 0;
                s_rx_too_big = false;
            }
        } else if (d->op_code == 0x8) {
            notify(N_FAILED);   // the other side is closing the socket
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        // Why, as numbers only: the address has the site's key in it and is never logged.
        ESP_LOGW(TAG, "socket error: kind %d, HTTP status %d, TLS %s", (int)d->error_handle.error_type,
                 d->error_handle.esp_ws_handshake_status_code, esp_err_to_name(d->error_handle.esp_tls_last_esp_err));
        notify(N_FAILED);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_FINISH:
        notify(N_FAILED);
        break;
    default:
        break;
    }
}

/* ---------- The socket (on the Realtime task) ---------- */

static bool socket_open(void)
{
    const cloud_realtime_session_t *s = &s_mem->session;
    strlcpy(s_topic, s->topic, sizeof(s_topic));
    strlcpy(s_printer_id, s->printer_id, sizeof(s_printer_id));
    s_ref = 1;
    s_beat_waiting = false;
    int n = snprintf(s_mem->url, sizeof(s_mem->url), "%s?apikey=%s&vsn=1.0.0", s->realtime_url, s->apikey);
    if (n >= (int)sizeof(s_mem->url)) {
        return false;
    }
    char agent[48];
    snprintf(agent, sizeof(agent), "TikPrint/%s", esp_app_get_description()->version);
    const esp_websocket_client_config_t config = {
        .uri = s_mem->url,
        .crt_bundle_attach = esp_crt_bundle_attach,   // check the site's certificate
        .user_agent = agent,
        .disable_auto_reconnect = true,               // when to try again is decided here, not by the library
        .task_name = "cloud_ws",
        .task_prio = TASK_PRIORITY,
        .task_stack = WS_TASK_STACK,
        .buffer_size = WS_BUFFER,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 60,
        .disable_pingpong_discon = true,              // the heartbeat below is what tells a dead socket
        .keep_alive_enable = true,
    };
    s_ws = esp_websocket_client_init(&config);
    if (!s_ws) {
        return false;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, on_ws_event, NULL);
    return esp_websocket_client_start(s_ws) == ESP_OK;
}

static void socket_close(void)
{
    if (s_ws) {
        esp_websocket_client_destroy(s_ws);   // stops the library's task first: nothing more is heard after this
        s_ws = NULL;
    }
    ulTaskNotifyValueClear(NULL, N_CONNECTED | N_FAILED | N_JOIN_OK | N_SYSTEM_OK);   // news of a socket that is gone
}

/* Send s_mem->out on the socket. */
static bool socket_send(int len)
{
    bool sent = len > 0 && len < (int)sizeof(s_mem->out) &&
                esp_websocket_client_send_text(s_ws, s_mem->out, len, pdMS_TO_TICKS(5000)) == len;
    memset(s_mem->out, 0, sizeof(s_mem->out));   // it may have had the token in it
    return sent;
}

/* "Let me hear the new rows of this table that are mine." Everything in it was checked by
 * session_is_usable(), so it can be put into the text as it is. */
static bool send_join(void)
{
    const cloud_realtime_session_t *s = &s_mem->session;
    return socket_send(snprintf(s_mem->out, sizeof(s_mem->out),
        "{\"topic\":\"%s\",\"event\":\"phx_join\",\"ref\":\"1\",\"join_ref\":\"1\",\"payload\":{\"config\":{"
        "\"broadcast\":{\"ack\":false,\"self\":false},\"presence\":{\"key\":\"\"},\"private\":false,"
        "\"postgres_changes\":[{\"event\":\"INSERT\",\"schema\":\"public\",\"table\":\"%s\",\"filter\":\"%s\"}]},"
        "\"access_token\":\"%s\"}}", s->topic, s->table, s->filter, s->access_token));
}

static bool send_beat(void)
{
    s_beat_ref = ++s_ref;
    s_beat_waiting = true;
    return socket_send(snprintf(s_mem->out, sizeof(s_mem->out),
        "{\"topic\":\"phoenix\",\"event\":\"heartbeat\",\"payload\":{},\"ref\":\"%d\"}", s_beat_ref));
}

/* A fresh token for the open socket. */
static bool send_token(void)
{
    const cloud_realtime_session_t *s = &s_mem->session;
    return socket_send(snprintf(s_mem->out, sizeof(s_mem->out),
        "{\"topic\":\"%s\",\"event\":\"access_token\",\"payload\":{\"access_token\":\"%s\"},\"ref\":\"%d\"}",
        s->topic, s->access_token, ++s_ref));
}

/* ---------- The printer's own row ---------- */

static esp_err_t on_rest_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        size_t room = sizeof(s_rest_body) - 1 - s_rest_len, n = (size_t)evt->data_len;
        n = n > room ? room : n;
        memcpy(s_rest_body + s_rest_len, evt->data, n);
        s_rest_len += n;
    }
    return ESP_OK;
}

static void rest_close(void)
{
    if (s_rest) {
        esp_http_client_cleanup(s_rest);
        s_rest = NULL;
    }
}

/*
 * Write the printer's row: the version that is running and, if `status` is not NULL, where an
 * update stands (`error` = why it failed, or ""). This write is the printer's "last seen".
 * The cloud lets this token write these columns of this one row and nothing else.
 */
static row_result_t write_row(const char *status, const char *error)
{
    const cloud_realtime_session_t *s = &s_mem->session;
    cJSON *row = cJSON_CreateObject();
    cJSON_AddStringToObject(row, "firmware_version", esp_app_get_description()->version);
    if (status) {
        cJSON_AddStringToObject(row, "update_status", status);
        if (error && error[0]) {
            cJSON_AddStringToObject(row, "update_error", error);
        }
    }
    char *json = cJSON_PrintUnformatted(row);
    cJSON_Delete(row);
    int n = snprintf(s_mem->url, sizeof(s_mem->url), "%s/rest/v1/printers?id=eq.%s&select=id", s->supabase_url, s->printer_id);
    if (!json || n >= (int)sizeof(s_mem->url)) {
        free(json);
        return ROW_FAILED;
    }

    esp_err_t err = ESP_FAIL;
    int http = -1;
    for (int try = 0; try < 2; try++) {
        bool reused = s_rest != NULL;
        if (!s_rest) {
            const esp_http_client_config_t config = {
                .url = s_mem->url,
                .crt_bundle_attach = esp_crt_bundle_attach,   // check the site's certificate
                .timeout_ms = ROW_TIMEOUT_MS,
                .event_handler = on_rest_event,
                .buffer_size = 2048,
                .buffer_size_tx = 4096,                       // the token is a long header
                .keep_alive_enable = true,
            };
            s_rest = esp_http_client_init(&config);
            if (!s_rest) {
                break;
            }
        } else {
            esp_http_client_set_url(s_rest, s_mem->url);
        }
        snprintf(s_mem->out, sizeof(s_mem->out), "Bearer %s", s->access_token);
        esp_http_client_set_method(s_rest, HTTP_METHOD_PATCH);
        esp_http_client_set_header(s_rest, "apikey", s->apikey);
        esp_http_client_set_header(s_rest, "Authorization", s_mem->out);
        esp_http_client_set_header(s_rest, "Content-Type", "application/json");
        esp_http_client_set_header(s_rest, "Prefer", "return=representation");
        esp_http_client_set_post_field(s_rest, json, (int)strlen(json));
        memset(s_mem->out, 0, sizeof(s_mem->out));   // the client keeps its own copy of the header
        s_rest_len = 0;
        err = esp_http_client_perform(s_rest);
        http = esp_http_client_get_status_code(s_rest);
        if (err == ESP_OK) {
            break;
        }
        // The other side closes a connection that has been quiet for a while: that is not a
        // failure. Once more, on a new one. (A 401 also ends up here: the client treats it as an error.)
        rest_close();
        if (!reused || http == 401) {
            break;
        }
    }
    free(json);
    s_rest_body[s_rest_len] = 0;

    if (http == 401) {
        return ROW_EXPIRED;
    }
    if (err != ESP_OK || http != 200) {
        ESP_LOGW(TAG, "the printer's row could not be written (%s, HTTP %d)", esp_err_to_name(err), http);
        return ROW_FAILED;
    }
    if (strstr(s_rest_body, "\"id\"")) {
        return ROW_WRITTEN;
    }
    const char *p = s_rest_body + strspn(s_rest_body, " \r\n\t");
    return p[0] == '[' && p[1 + strspn(p + 1, " \r\n\t")] == ']' ? ROW_NOT_LINKED : ROW_FAILED;
}

/* ---------- The task ---------- */

static void realtime_task(void *arg)
{
    phase_t phase = PH_WAITING;
    int backoff_s = RETRY_FIRST_S;
    int row_every_s = ROW_EVERY_S;
    int64_t retry_at = 0;               // when to try for a socket again (0 = now)
    int64_t asked_at = 0;               // when the cloud task was last asked for a session
    int64_t deadline = 0;               // connecting, joining: when it has taken too long
    int64_t next_beat = 0, beat_deadline = 0, next_row = 0, next_report = 0;
    int64_t session_until = 0;          // when the access token runs out (0 = there is no session)
    int64_t refresh_at = 0;             // when to ask for a fresh one (80% of its life)
    bool refreshing = false;            // a fresh token was asked for, for the open socket
    bool join_ok = false, system_ok = false;
    ESP_LOGI(TAG, "Realtime task started");

    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(TICK_MS));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool stop = s_stop;
        xSemaphoreGive(s_lock);
        if (stop) {
            break;
        }
        int64_t now = esp_timer_get_time();
        bool wifi = wifi_up();
        const char *lost = NULL;        // set when the socket has to be given up: why
        bool off = false;               // ... because the site has no Realtime for printers

        // The cloud task's answer to "a session, please".
        cloud_realtime_result_t result;
        if (take_session_answer(&result)) {
            if (result == CLOUD_REALTIME_SESSION_OK) {
                const cloud_realtime_session_t *s = &s_mem->session;
                int life_s = s->expires_in_s < 60 ? 60 : s->expires_in_s;
                session_until = now + (int64_t)life_s * 1000000;
                refresh_at = now + (int64_t)life_s * 800000;   // 80% of its life
                row_every_s = s->heartbeat_s < 15 ? 15 : s->heartbeat_s > 900 ? 900 : s->heartbeat_s;
            }
            if (phase == PH_ASKING) {
                if (result == CLOUD_REALTIME_SESSION_OK) {
                    phase = PH_CONNECTING;
                    deadline = now + (int64_t)CONNECT_WITHIN_S * 1000000;
                    join_ok = system_ok = false;
                    if (!socket_open()) {
                        lost = "the socket could not be opened";
                    }
                } else if (result == CLOUD_REALTIME_SESSION_OFF) {
                    off = true;
                } else {
                    phase = PH_WAITING;
                    retry_at = now + (int64_t)backoff_s * 1000000;
                    backoff_s = backoff_s * 2 > RETRY_MAX_S ? RETRY_MAX_S : backoff_s * 2;
                }
            } else if (refreshing) {
                refreshing = false;
                if (result == CLOUD_REALTIME_SESSION_OK) {
                    if (phase == PH_LISTENING && !send_token()) {
                        lost = "the fresh token could not be sent";
                    }
                } else if (result == CLOUD_REALTIME_SESSION_OFF) {
                    off = true;
                } else {
                    refresh_at = now + (int64_t)REFRESH_RETRY_S * 1000000;
                }
            }
        }

        bool failed = (bits & N_FAILED) != 0;
        switch (phase) {
        case PH_WAITING:
        case PH_OFF:
            if (!wifi) {
                backoff_s = RETRY_FIRST_S;   // no Wi-Fi is no fault of the socket's: start over when it is back
            } else if (now >= retry_at) {
                ask_for_session();           // a fresh session for every try
                asked_at = now;
                phase = PH_ASKING;
            }
            break;
        case PH_ASKING:
            if (now - asked_at > (int64_t)ASK_AGAIN_S * 1000000) {
                ask_for_session();
                asked_at = now;
            }
            break;
        case PH_CONNECTING:
            if (lost) {
                // could not even be opened: see above
            } else if (failed || !wifi) {
                lost = "it could not connect";
            } else if (now > deadline) {
                lost = "connecting took too long";
            } else if (bits & N_CONNECTED) {
                if (send_join()) {
                    phase = PH_JOINING;
                } else {
                    lost = "the join could not be sent";
                }
            }
            break;
        case PH_JOINING:
            join_ok = join_ok || (bits & N_JOIN_OK);
            system_ok = system_ok || (bits & N_SYSTEM_OK);
            if (failed || !wifi) {
                lost = "it closed while joining";
            } else if (now > deadline) {
                lost = "joining took too long";
            } else if (join_ok && system_ok) {
                phase = PH_LISTENING;
                backoff_s = RETRY_FIRST_S;
                next_beat = now + (int64_t)BEAT_EVERY_S * 1000000;
                next_row = now;   // say "I am here" straight away
                set_phase(phase);
                set_event("listening");
                ESP_LOGI(TAG, "listening on Realtime: prints arrive at once, check-ins are the backup");
                cloud_poll_now();   // with "X-Realtime: 1": whatever was missed meanwhile is in the answer
            }
            break;
        case PH_LISTENING:
            if (lost) {
                // see above
            } else if (failed) {
                lost = "it closed";
            } else if (!wifi) {
                lost = "Wi-Fi is gone";
            } else if (s_beat_waiting && now > beat_deadline) {
                lost = "no reply to the heartbeat";
            } else {
                if (now >= next_beat) {
                    next_beat = now + (int64_t)BEAT_EVERY_S * 1000000;
                    beat_deadline = now + (int64_t)BEAT_REPLY_S * 1000000;
                    if (!send_beat()) {
                        lost = "the heartbeat could not be sent";
                    }
                }
                if (!refreshing && now >= refresh_at) {
                    ask_for_session();
                    refreshing = true;
                }
                if (!lost && now >= next_row) {
                    next_row = now + (int64_t)row_every_s * 1000000;
                    row_result_t row = write_row(NULL, NULL);
                    if (row == ROW_NOT_LINKED) {
                        // Nothing was written: the printer has been unlinked. The check-in's 401 is what acts on it.
                        ESP_LOGW(TAG, "the row was not written: checking in to see whether the printer is still linked");
                        cloud_poll_now();
                    } else if (row == ROW_EXPIRED && !refreshing) {
                        refresh_at = now;   // the token ran out early: a fresh one, now
                    }
                }
            }
            break;
        default:
            break;
        }

        if (lost || off) {
            bool was_listening = phase == PH_LISTENING;
            phase = off ? PH_OFF : PH_WAITING;
            set_phase(phase);   // "not listening" first, so that the check-in below goes without "X-Realtime"
            if (was_listening) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_reconnects++;
                xSemaphoreGive(s_lock);
                cloud_poll_now();
            }
            socket_close();
            refreshing = false;
            if (off) {
                set_event("off");
                ESP_LOGI(TAG, "this site has no Realtime for printers: checking in only, asking again in an hour");
                retry_at = esp_timer_get_time() + (int64_t)OFF_RETRY_S * 1000000;
                backoff_s = RETRY_FIRST_S;
            } else {
                set_event(was_listening ? "closed" : "failed");
                ESP_LOGW(TAG, "Realtime socket %s (%s): trying again in %d s", was_listening ? "lost" : "not up", lost, backoff_s);
                retry_at = esp_timer_get_time() + (int64_t)backoff_s * 1000000;
                backoff_s = backoff_s * 2 > RETRY_MAX_S ? RETRY_MAX_S : backoff_s * 2;
            }
        } else {
            set_phase(phase);
        }

        // An update the cloud asked for: start it, and tell the cloud each step. That only needs a
        // session whose token is still good, not the socket.
        cloud_update_tick();
        now = esp_timer_get_time();
        if (wifi && now < session_until && now >= next_report) {
            char status[CLOUD_UPDATE_STATUS_MAX], error[CLOUD_UPDATE_ERROR_MAX];
            uint32_t report;
            if (cloud_update_next_report(status, error, &report)) {
                row_result_t row = write_row(status, error);
                if (row == ROW_WRITTEN) {
                    cloud_update_reported(report);
                } else {
                    next_report = now + (int64_t)REPORT_RETRY_S * 1000000;
                    if (row == ROW_NOT_LINKED) {
                        cloud_poll_now();
                    } else if (row == ROW_EXPIRED && phase == PH_LISTENING && !refreshing) {
                        refresh_at = now;
                    } else if (row == ROW_EXPIRED && phase != PH_LISTENING) {
                        session_until = 0;   // the next try for a socket brings a new one
                    }
                }
            }
        }
    }

    // The printer is not linked any more: close everything and leave nothing of the session behind.
    socket_close();
    rest_close();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_mem, 0, sizeof(*s_mem));
    free(s_mem);
    s_mem = NULL;
    s_phase = PH_STOPPED;
    s_want_session = s_have_incoming = false;
    s_running = false;
    s_task = NULL;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Realtime task ended");
    vTaskDelete(NULL);
}

void cloud_realtime_start(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_running) {
        s_mem = heap_caps_calloc(1, sizeof(*s_mem), MALLOC_CAP_SPIRAM);
        s_stop = false;
        s_want_session = s_have_incoming = false;
        s_phase = PH_WAITING;
        if (s_mem && xTaskCreate(realtime_task, "cloud_rt", TASK_STACK, NULL, TASK_PRIORITY, &s_task) == pdPASS) {
            s_running = true;
        } else {
            // No memory just now. The printer still works by checking in; the cloud task tries this again.
            ESP_LOGW(TAG, "the Realtime task could not start: checking in only for now");
            free(s_mem);
            s_mem = NULL;
            s_task = NULL;
        }
    }
    xSemaphoreGive(s_lock);
}

void cloud_realtime_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool running = s_running;
    if (running) {
        s_stop = true;
        xTaskNotify(s_task, N_WAKE, eSetBits);
    }
    xSemaphoreGive(s_lock);
    while (running) {
        vTaskDelay(pdMS_TO_TICKS(50));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        running = s_running;
        xSemaphoreGive(s_lock);
    }
}

esp_err_t cloud_realtime_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}
