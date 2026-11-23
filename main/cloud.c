/*
 * The board's side of TikPrint Cloud (see cloud.h). The contract is docs/device-api.md in the
 * TikPrint Cloud repo.
 *
 * Calling the cloud takes seconds and needs a deep stack, so it all happens on one background
 * task, "cloud". That task only exists while there is something to do: while pairing, and for as
 * long as the printer is linked. A printer that is not linked has no such task and makes no calls.
 *
 * The web server asks for things by setting a flag (s_want_pair, s_want_unlink) and waking the
 * task; it reads how things stand from s_status. All of that is guarded by s_lock.
 *
 * Printing from anywhere, once linked:
 *   every 10 s   GET jobs          "anything for me?" (this is also what shows the printer as online)
 *   for a job    GET its body      the same bytes the page POSTs to /api/print
 *                api_print_body    into the print queue, the very path a local print takes
 *   when the ticket is out         POST ack "printed" (or "failed" if the printer gave up)
 * One cloud job is in the print queue at a time, oldest first, so cloud jobs never crowd out
 * local prints. A job that is only waiting (for its turn, for paper, for the cover) is not
 * reported: the report goes out when it has really printed.
 * Jobs already finished are remembered (cloud_jobs.c): if one is offered again, its report was
 * lost, so it is reported again and not printed again.
 */
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "api.h"
#include "cloud.h"
#include "cloud_http.h"
#include "cloud_jobs.h"
#include "cloud_state.h"
#include "cloud_ticket.h"

static const char *TAG = "cloud";

#define TASK_STACK       12288   // HTTPS needs a deep stack
#define TASK_PRIORITY    3       // below the web server, the print queue and the automations: it never holds them up
#define JSON_MAX         8192    // the biggest JSON answer accepted
#define PAIR_EVERY_S     3       // "am I linked yet?" this often, unless the cloud says otherwise
#define PAIR_LIFETIME_S  600     // how long a pairing code is good for, unless the cloud says otherwise
#define PAIR_GRACE_S     20      // keep asking this long after the code should have run out: the cloud has the last word
#define POLL_EVERY_S     10      // check in for jobs this often, unless the cloud says otherwise
#define POLL_SLOWEST_S   60      // when the cloud cannot be reached, the wait doubles each time, up to this
#define DOWNLOAD_TRIES   5       // a print body that will not download is given up on after this many check-ins

#define MSG_UNREACHABLE "Can't reach TikPrint Cloud. Check the Wi-Fi."
#define MSG_REFUSED     "TikPrint Cloud did not accept this printer."

static SemaphoreHandle_t s_lock;      // guards everything below
static SemaphoreHandle_t s_wake;      // given whenever the task should look again
static cloud_status_t s_status;
static bool s_task_running;
static bool s_want_pair;              // Upgrade was pressed
static bool s_want_unlink;            // Reset / Unlink was pressed: tell the cloud, with s_unlink_token
static char s_unlink_token[CLOUD_TOKEN_MAX];   // the token that was just deleted, kept only until the cloud has been told
static int64_t s_pair_until_us;       // when the pairing code runs out (board uptime)
static int s_pair_every_s;
/* The print queue's task tells the cloud task that the cloud job in the queue has ended: */
static bool s_done_ready;             // there is news
static uint32_t s_done_ticket;        // which ticket it is about (s_in_queue.ticket)
static esp_err_t s_done_result;       // ESP_OK = printed

/* The rest is only touched by the cloud task. */
static int s_poll_every_s = POLL_EVERY_S;
static int s_poll_failures;           // check-ins in a row that got no answer
static struct {
    bool active;                      // a cloud job is in the print queue (waiting or printing)
    char id[CLOUD_JOB_ID_MAX];
    uint32_t ticket;                  // a number of our own for it, so that old news is not taken for new
} s_in_queue;
static uint32_t s_ticket_counter;
static struct {
    char id[CLOUD_JOB_ID_MAX];        // the job whose print body would not download
    int tries;
} s_stuck;

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

/* ---------- Printing from anywhere ---------- */

/* The cloud says this token is not valid any more: the owner unlinked the printer on the website,
 * or linked it again elsewhere. Delete the token and go back to local only: from here the board
 * calls nobody until Upgrade is pressed again. */
static void go_local(const char *why)
{
    ESP_LOGW(TAG, "%s: token deleted, back to local only", why);
    cloud_state_clear();
    cloud_jobs_clear();
    s_in_queue.active = false;   // if that ticket is still in the queue it prints; nobody is told
    s_stuck.id[0] = 0;
    set_phase(CLOUD_LOCAL, NULL, NULL);
}

/* Is this answer the cloud saying "you are not linked any more"? */
static bool says_unlinked(const cloud_reply_t *reply)
{
    cJSON *json = reply->status == 401 ? reply_json(reply) : NULL;
    bool unlinked = json && strcmp(text_of(json, "error"), "invalid_token") == 0;
    cJSON_Delete(json);
    return unlinked;
}

/* A job ID goes into an address: letters, digits and dashes only. */
static bool job_id_is_plain(const char *id)
{
    size_t n = strlen(id);
    if (n < 8 || n >= CLOUD_JOB_ID_MAX) {
        return false;
    }
    for (const char *p = id; *p; p++) {
        bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* Called by the print queue, on its task, when the cloud job has printed or failed. */
static void on_print_done(void *ctx, esp_err_t result)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_done_ready = true;
    s_done_ticket = (uint32_t)(uintptr_t)ctx;
    s_done_result = result;
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_wake);   // report back now, not at the next check-in
}

/* Has the cloud job in the print queue ended? Then note how, to be reported. */
static void collect_print_done(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ready = s_done_ready;
    uint32_t ticket = s_done_ticket;
    esp_err_t result = s_done_result;
    s_done_ready = false;
    xSemaphoreGive(s_lock);
    if (ready && s_in_queue.active && ticket == s_in_queue.ticket) {
        ESP_LOGI(TAG, "cloud job %.8s %s", s_in_queue.id, result == ESP_OK ? "printed" : "failed to print");
        cloud_jobs_finished(s_in_queue.id, result == ESP_OK ? CLOUD_JOB_PRINTED : CLOUD_JOB_NOT_PRINTED);
        s_in_queue.active = false;
    }
}

/* Report every finished job the cloud has not heard about. A report that does not get through
 * stays for the next check-in. False if the printer turned out to be unlinked. */
static bool send_reports(const char *token)
{
    char id[CLOUD_JOB_ID_MAX], path[96], json[160];
    cloud_job_end_t end;
    while (cloud_jobs_next_unreported(id, &end)) {
        snprintf(path, sizeof(path), "/api/device/jobs/%s/ack", id);
        if (end == CLOUD_JOB_PRINTED) {
            strcpy(json, "{\"status\":\"printed\"}");
        } else {
            snprintf(json, sizeof(json), "{\"status\":\"failed\",\"error\":\"%s\"}", cloud_job_end_message(end));
        }
        cloud_reply_t reply;
        cloud_http(&(cloud_request_t){ .post = true, .path = path, .auth = CLOUD_AUTH_TOKEN, .token = token,
                                       .json = json, .max_bytes = JSON_MAX }, &reply);
        bool unlinked = says_unlinked(&reply);
        int status = reply.status;
        cloud_reply_free(&reply);
        if (unlinked) {
            go_local("the cloud refused the token");
            return false;
        }
        if (status == 200 || status == 404 || status == 400) {
            cloud_jobs_reported(id);   // heard; or the cloud has dropped the job (404) and there is nobody to tell
        } else {
            ESP_LOGW(TAG, "the report on job %.8s did not get through (%d): it goes again at the next check-in", id, status);
            break;
        }
    }
    return true;
}

/* A job's print body would not download. Give up on the job after DOWNLOAD_TRIES check-ins. */
static void download_failed(const char *id)
{
    if (strcmp(s_stuck.id, id) != 0) {
        strlcpy(s_stuck.id, id, sizeof(s_stuck.id));
        s_stuck.tries = 0;
    }
    if (++s_stuck.tries >= DOWNLOAD_TRIES) {
        ESP_LOGW(TAG, "cloud job %.8s: giving up on the download", id);
        cloud_jobs_finished(id, CLOUD_JOB_NO_DOWNLOAD);
        s_stuck.id[0] = 0;
    }
}

/* Download a new job and put it in the print queue. False if the printer turned out to be unlinked. */
static bool start_job(const char *token, const char *id, int size)
{
    if (size > API_PRINT_BODY_MAX) {
        cloud_jobs_finished(id, CLOUD_JOB_TOO_LARGE);
        return true;
    }
    char path[96];
    snprintf(path, sizeof(path), "/api/device/jobs/%s/body", id);
    cloud_reply_t reply;
    cloud_http(&(cloud_request_t){ .path = path, .auth = CLOUD_AUTH_TOKEN, .token = token,
                                   .max_bytes = API_PRINT_BODY_MAX }, &reply);
    if (says_unlinked(&reply)) {
        cloud_reply_free(&reply);
        go_local("the cloud refused the token");
        return false;
    }
    if (reply.status == 404) {
        ESP_LOGI(TAG, "cloud job %.8s is gone (finished or dropped): forgotten", id);
    } else if (reply.status != 200) {
        download_failed(id);
    } else if (reply.too_big) {
        cloud_jobs_finished(id, CLOUD_JOB_TOO_LARGE);
    } else if (size > 0 && reply.len != (size_t)size) {
        // The connection ended early, or these are not the bytes the job list promised.
        ESP_LOGW(TAG, "cloud job %.8s: got %u bytes, expected %d", id, (unsigned)reply.len, size);
        download_failed(id);
    } else {
        // The same bytes the page sends, into the same code: a cloud print and a local print come out alike.
        printer_ticket_t ticket;
        const char *error = "", *message = "";
        uint32_t ours = ++s_ticket_counter;
        esp_err_t err = api_print_body((const uint8_t *)reply.body, reply.len, on_print_done, (void *)(uintptr_t)ours,
                                       &ticket, &error, &message);
        if (err == ESP_OK) {
            s_in_queue.active = true;
            s_in_queue.ticket = ours;
            strlcpy(s_in_queue.id, id, sizeof(s_in_queue.id));
            ESP_LOGI(TAG, "cloud job %.8s (%u bytes) is ticket #%04u, position %d in the queue", id,
                     (unsigned)reply.len, (unsigned)ticket.number, ticket.position);
        } else if (err == ESP_ERR_INVALID_ARG) {
            ESP_LOGW(TAG, "cloud job %.8s cannot print: %s (%s)", id, message, error);
            cloud_jobs_finished(id, CLOUD_JOB_UNREADABLE);
        } else {
            // The queue is full or memory is short: the job stays on the cloud, and the next check-in tries again.
            ESP_LOGW(TAG, "cloud job %.8s has to wait: %s", id, message);
        }
    }
    cloud_reply_free(&reply);
    return true;
}

/* One check-in. Returns how many seconds to wait before the next. */
static int check_in(void)
{
    char token[CLOUD_TOKEN_MAX];
    if (!cloud_state_get_token(token, sizeof(token))) {
        set_phase(CLOUD_LOCAL, NULL, NULL);   // the token is gone: nothing to check in with
        return 0;
    }
    collect_print_done();
    if (!send_reports(token)) {
        return 0;
    }
    cloud_reply_t reply;
    cloud_http(&(cloud_request_t){ .path = "/api/device/jobs", .auth = CLOUD_AUTH_TOKEN, .token = token,
                                   .max_bytes = JSON_MAX }, &reply);
    if (says_unlinked(&reply)) {
        cloud_reply_free(&reply);
        go_local("the cloud refused the token");
        return 0;
    }
    cJSON *json = reply.status == 200 ? reply_json(&reply) : NULL;
    cloud_reply_free(&reply);
    if (!json) {
        // No network, a timeout, a 5xx, a 429, an answer that is not JSON: nothing is wrong with the
        // link. Keep the token and try again, a little later each time.
        s_poll_failures = s_poll_failures < 8 ? s_poll_failures + 1 : 8;
        int wait = s_poll_every_s << (s_poll_failures - 1);
        return wait > POLL_SLOWEST_S ? POLL_SLOWEST_S : wait;
    }
    s_poll_failures = 0;
    s_poll_every_s = number_of(json, "poll_interval_s", POLL_EVERY_S, 3, 300);

    bool linked = true, more_to_report = false;
    const cJSON *job;
    cJSON_ArrayForEach(job, cJSON_GetObjectItem(json, "jobs")) {   // oldest first
        const char *id = text_of(job, "id");
        cloud_job_end_t end;
        if (!job_id_is_plain(id)) {
            continue;
        }
        if (cloud_jobs_find(id, &end)) {
            // Finished already, yet offered again: the report was lost. Report again; do not print again.
            ESP_LOGI(TAG, "cloud job %.8s is offered again: reporting it again, not printing it again", id);
            cloud_jobs_report_again(id);
            more_to_report = true;
        } else if (!s_in_queue.active) {
            linked = start_job(token, id, number_of(job, "size", 0, 0, API_PRINT_BODY_MAX + 1));
            if (!linked) {
                break;
            }
            more_to_report = more_to_report || cloud_jobs_find(id, &end);   // it could not print: say so now
        }
        // else: a cloud job is in the print queue. This one waits its turn on the cloud.
    }
    cJSON_Delete(json);
    if (linked && more_to_report) {
        linked = send_reports(token);
    }
    return linked ? s_poll_every_s : 0;
}

/* ---------- Unlinking ---------- */

static esp_err_t start_task_locked(void);

/* Reset / Unlink was pressed and the token is deleted already (cloud_unlink). Tell the cloud, so
 * that the website stops listing the printer. Whatever the answer, or with none at all, the
 * printer stays unlinked. */
static void tell_cloud_unlinked(void)
{
    char token[CLOUD_TOKEN_MAX];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(token, s_unlink_token, sizeof(token));
    memset(s_unlink_token, 0, sizeof(s_unlink_token));
    xSemaphoreGive(s_lock);
    cloud_jobs_clear();
    s_in_queue.active = false;
    s_stuck.id[0] = 0;
    if (token[0]) {
        cloud_reply_t reply;
        cloud_http(&(cloud_request_t){ .post = true, .path = "/api/device/unlink", .auth = CLOUD_AUTH_TOKEN,
                                       .token = token, .max_bytes = JSON_MAX }, &reply);
        ESP_LOGI(TAG, "unlinked: %s", reply.status == 200 || reply.status == 401 ? "the cloud knows"
                 : "the cloud was not reached, the website will still list the printer");
        cloud_reply_free(&reply);
        memset(token, 0, sizeof(token));
    }
}

void cloud_unlink(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // Keep the token for one last call, then delete it from the board before anything else happens.
    if (cloud_state_get_token(s_unlink_token, sizeof(s_unlink_token))) {
        s_want_unlink = true;
    }
    cloud_state_clear();
    s_want_pair = false;
    s_status = (cloud_status_t){ .phase = CLOUD_LOCAL };
    if (s_want_unlink) {
        start_task_locked();   // if it cannot start, the cloud is not told; the printer is unlinked all the same
    }
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_wake);
    ESP_LOGW(TAG, "Reset / Unlink pressed: token deleted, back to local only");
}

/* ---------- The task ---------- */

static void cloud_task(void *arg)
{
    ESP_LOGI(TAG, "cloud task started");
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool pair = s_want_pair, unlink = s_want_unlink;
        s_want_pair = s_want_unlink = false;
        cloud_phase_t phase = s_status.phase;
        int every_s = s_pair_every_s;
        xSemaphoreGive(s_lock);

        if (unlink) {
            tell_cloud_unlinked();
        }
        if (pair) {
            pair_start();
        } else if (unlink) {
            // look again: nothing else was asked for, so the task ends below
        } else if (phase == CLOUD_PAIRING) {
            wait_s(every_s);
            if (phase_now() == CLOUD_PAIRING) {
                pair_ask();
            }
        } else if (phase == CLOUD_LINKED) {
            int wait = check_in();
            if (wait > 0) {
                wait_s(wait);   // or until the ticket has printed, or Upgrade is pressed
            }
        } else {
            // Nothing left to do: close the connection and end the task. From here the board calls
            // nobody, until Upgrade is pressed again.
            cloud_http_close();
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (!s_want_pair && !s_want_unlink) {
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
    cloud_jobs_init();
    if (!cloud_state_is_linked()) {
        s_status.phase = CLOUD_LOCAL;   // no token: no task, no calls
        return ESP_OK;
    }
    s_status.phase = CLOUD_LINKED;      // linked: carry on checking in with the saved token
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = start_task_locked();
    xSemaphoreGive(s_lock);
    return err;
}
