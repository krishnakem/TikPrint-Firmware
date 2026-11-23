/*
 * The board's side of TikPrint Cloud (see cloud.h). The contract is docs/device-api.md in the
 * TikPrint Cloud repo.
 *
 * Calling the cloud takes seconds and needs a deep stack, so it all happens on one background
 * task, "cloud". That task only exists while there is something to do: while pairing. A printer
 * that is not linked has no such task and makes no calls.
 *
 * The web server asks for things by setting a flag (s_want_pair) and waking the task; it reads
 * how things stand from s_status. Both are guarded by s_lock.
 */
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cloud.h"
#include "cloud_http.h"
#include "cloud_state.h"
#include "cloud_ticket.h"

static const char *TAG = "cloud";

#define TASK_STACK       12288   // HTTPS needs a deep stack
#define TASK_PRIORITY    3       // below the web server, the print queue and the automations: it never holds them up
#define JSON_MAX         8192    // the biggest JSON answer accepted
#define PAIR_EVERY_S     3       // "am I linked yet?" this often, unless the cloud says otherwise
#define PAIR_LIFETIME_S  600     // how long a pairing code is good for, unless the cloud says otherwise
#define PAIR_GRACE_S     20      // keep asking this long after the code should have run out: the cloud has the last word

#define MSG_UNREACHABLE "Can't reach TikPrint Cloud. Check the Wi-Fi."
#define MSG_REFUSED     "TikPrint Cloud did not accept this printer."

static SemaphoreHandle_t s_lock;      // guards everything below
static SemaphoreHandle_t s_wake;      // given whenever the task should look again
static cloud_status_t s_status;
static bool s_task_running;
static bool s_want_pair;              // Upgrade was pressed
static int64_t s_pair_until_us;       // when the pairing code runs out (board uptime)
static int s_pair_every_s;

const char *cloud_phase_name(cloud_phase_t phase)
{
    switch (phase) {
    case CLOUD_STARTING: return "starting";
    case CLOUD_PAIRING:  return "pairing";
    case CLOUD_LINKED:   return "linked";
    case CLOUD_EXPIRED:  return "expired";
    case CLOUD_FAILED:   return "failed";
    default:             return "local";
    }
}

void cloud_get_status(cloud_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    if (out->phase == CLOUD_PAIRING) {
        int64_t left = (s_pair_until_us - esp_timer_get_time()) / 1000000;
        out->expires_in_s = left > 0 ? (int)left : 0;
    }
    xSemaphoreGive(s_lock);
}

/* Change what the page is told. code and message may be NULL (= none). */
static void set_phase(cloud_phase_t phase, const char *code, const char *message)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cloud_phase_t before = s_status.phase;
    s_status.phase = phase;
    strlcpy(s_status.code, code ? code : "", sizeof(s_status.code));
    strlcpy(s_status.message, message ? message : "", sizeof(s_status.message));
    xSemaphoreGive(s_lock);
    if (phase != before) {
        ESP_LOGI(TAG, "%s -> %s%s%s", cloud_phase_name(before), cloud_phase_name(phase), message ? ": " : "",
                 message ? message : "");
    }
}

static cloud_phase_t phase_now(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cloud_phase_t phase = s_status.phase;
    xSemaphoreGive(s_lock);
    return phase;
}

/* Sleep until someone wakes the task, but no longer than seconds. */
static void wait_s(int seconds)
{
    xSemaphoreTake(s_wake, pdMS_TO_TICKS(seconds * 1000));
}

/* ---------- Reading the cloud's answers ---------- */

/* The answer as JSON, or NULL if it is not JSON. cJSON_Delete() it. */
static cJSON *reply_json(const cloud_reply_t *reply)
{
    return reply->body && !reply->too_big ? cJSON_ParseWithLength(reply->body, reply->len) : NULL;
}

/* A text field of the answer, or "" if it is not there. */
static const char *text_of(const cJSON *json, const char *name)
{
    const cJSON *item = cJSON_GetObjectItem(json, name);
    return cJSON_IsString(item) ? item->valuestring : "";
}

/* A whole number in the answer, kept within low..high; otherwise if_missing. */
static int number_of(const cJSON *json, const char *name, int if_missing, int low, int high)
{
    const cJSON *item = cJSON_GetObjectItem(json, name);
    if (!cJSON_IsNumber(item)) {
        return if_missing;
    }
    return item->valuedouble < low ? low : item->valuedouble > high ? high : item->valueint;
}

/* The site could not be reached, or it has trouble of its own (5xx), or it says "slow down" (429):
 * nothing is wrong with this printer, so it tries again later. */
static bool is_temporary(int status)
{
    return status < 0 || status >= 500 || status == 429 || status == 408;
}

/* ---------- Pairing ---------- */

/* A pairing code is letters, digits and dashes: it goes into an address and onto the page as it is. */
static bool code_is_plain(const char *code)
{
    size_t n = strlen(code);
    if (n < 4 || n >= CLOUD_CODE_MAX) {
        return false;
    }
    for (const char *p = code; *p; p++) {
        bool ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* Upgrade was pressed: get a code and print the ticket. This is the first call a printer ever makes. */
static void pair_start(void)
{
    cloud_reply_t reply;
    cloud_http(&(cloud_request_t){ .post = true, .path = "/api/device/pair/start", .auth = CLOUD_AUTH_DEVICE,
                                   .max_bytes = JSON_MAX }, &reply);
    cJSON *json = reply_json(&reply);
    const char *code = text_of(json, "code"), *link = text_of(json, "link_url");
    if (reply.status != 200) {
        ESP_LOGW(TAG, "pair/start: %d %s", reply.status, text_of(json, "error"));
        set_phase(CLOUD_FAILED, NULL, is_temporary(reply.status) ? MSG_UNREACHABLE : MSG_REFUSED);
    } else if (!code_is_plain(code) || strncmp(link, "https://", 8) != 0) {
        ESP_LOGW(TAG, "pair/start: the answer has no usable code or link");
        set_phase(CLOUD_FAILED, NULL, MSG_UNREACHABLE);
    } else {
        int lifetime_s = number_of(json, "expires_in_s", PAIR_LIFETIME_S, 30, 3600);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_pair_every_s = number_of(json, "poll_interval_s", PAIR_EVERY_S, 2, 30);
        s_pair_until_us = esp_timer_get_time() + (int64_t)lifetime_s * 1000000;
        xSemaphoreGive(s_lock);
        set_phase(CLOUD_PAIRING, code, NULL);
        // If the ticket cannot print (queue full, no paper), the page still shows the code to type in.
        esp_err_t err = cloud_ticket_print(link, code, (lifetime_s + 30) / 60);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "the pairing ticket did not join the print queue: %s", esp_err_to_name(err));
        }
    }
    cJSON_Delete(json);
    cloud_reply_free(&reply);
}

/* "Am I linked yet?" Asked every few seconds while the ticket is out. */
static void pair_ask(void)
{
    cloud_status_t now;
    cloud_get_status(&now);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool overdue = esp_timer_get_time() > s_pair_until_us + (int64_t)PAIR_GRACE_S * 1000000;
    xSemaphoreGive(s_lock);

    char path[80];
    snprintf(path, sizeof(path), "/api/device/pair/status?code=%s", now.code);
    cloud_reply_t reply;
    cloud_http(&(cloud_request_t){ .path = path, .auth = CLOUD_AUTH_DEVICE, .max_bytes = JSON_MAX }, &reply);
    cJSON *json = reply_json(&reply);
    const char *status = text_of(json, "status"), *error = text_of(json, "error");

    if (reply.status == 200 && strcmp(status, "linked") == 0) {
        // Save the token at once. If this answer had been lost, the next one would carry a fresh
        // token and cancel this one: the newest always wins, and it is the only one ever saved.
        if (cloud_state_save_link(text_of(json, "device_token"), text_of(json, "printer_id")) == ESP_OK) {
            ESP_LOGI(TAG, "linked as \"%s\": device token saved", text_of(json, "name"));   // the token is never logged
            set_phase(CLOUD_LINKED, NULL, NULL);
        } else {
            ESP_LOGW(TAG, "linked, but the token could not be saved: asking for another");
        }
    } else if (reply.status == 200 && strcmp(status, "expired") == 0) {
        set_phase(CLOUD_EXPIRED, NULL, NULL);
    } else if (reply.status == 200 && strcmp(status, "pending") == 0) {
        // keep asking
    } else if (reply.status == 409 && cloud_state_is_linked()) {
        set_phase(CLOUD_LINKED, NULL, NULL);   // "already_linked", and the token is here: all is well
    } else if (reply.status == 404 || reply.status == 409) {
        // unknown_code, unknown_device, or linked with a token this board never got: start over with Upgrade
        ESP_LOGW(TAG, "pair/status: %d %s", reply.status, error);
        set_phase(CLOUD_EXPIRED, NULL, NULL);
    } else if (reply.status == 400 || reply.status == 401) {
        ESP_LOGW(TAG, "pair/status: %d %s", reply.status, error);   // bad_device, bad_code, bad_secret
        set_phase(CLOUD_FAILED, NULL, MSG_REFUSED);
    } else if (overdue) {
        // No clear answer (no network, a 5xx, ...) and the code has run out by now anyway.
        set_phase(CLOUD_EXPIRED, NULL, NULL);
    }
    cJSON_Delete(json);
    cloud_reply_free(&reply);
}

/* ---------- The task ---------- */

static void cloud_task(void *arg)
{
    ESP_LOGI(TAG, "cloud task started");
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool pair = s_want_pair;
        s_want_pair = false;
        cloud_phase_t phase = s_status.phase;
        int every_s = s_pair_every_s;
        xSemaphoreGive(s_lock);

        if (pair) {
            pair_start();
        } else if (phase == CLOUD_PAIRING) {
            wait_s(every_s);
            if (phase_now() == CLOUD_PAIRING) {
                pair_ask();
            }
        } else {
            // Nothing left to do: close the connection and end the task. From here the board calls
            // nobody, until Upgrade is pressed again.
            cloud_http_close();
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (!s_want_pair) {
                s_task_running = false;
                xSemaphoreGive(s_lock);
                ESP_LOGI(TAG, "cloud task ended: nothing to do");
                vTaskDelete(NULL);
            }
            xSemaphoreGive(s_lock);
        }
    }
}

/* Start the task unless it is running. Call with s_lock held. */
static esp_err_t start_task_locked(void)
{
    if (s_task_running) {
        return ESP_OK;
    }
    if (xTaskCreate(cloud_task, "cloud", TASK_STACK, NULL, TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_task_running = true;
    return ESP_OK;
}

esp_err_t cloud_pair(void)
{
    if (cloud_state_is_linked()) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = start_task_locked();
    if (err == ESP_OK) {
        s_want_pair = true;
        s_status = (cloud_status_t){ .phase = CLOUD_STARTING };
    }
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_wake);
    return err;
}

esp_err_t cloud_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_wake = xSemaphoreCreateBinary();
    if (!s_lock || !s_wake) {
        return ESP_ERR_NO_MEM;
    }
    s_pair_every_s = PAIR_EVERY_S;
    s_status.phase = cloud_state_is_linked() ? CLOUD_LINKED : CLOUD_LOCAL;
    return ESP_OK;
}
