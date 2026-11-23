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
 * Who owns the printer changes only with paper in hand (see cloud.h): the pairing code goes to the
 * ticket and nowhere else, and unlinking from the page waits for the code on a ticket of its own.
 *
 * Printing from anywhere, once linked:
 *   now and then GET jobs          "anything for me?" The cloud says how long to wait before the next
 *   for a job    GET its body      the same bytes the page POSTs to /api/print
 *                api_print_body    into the print queue, the very path a local print takes
 *   when the ticket is out         POST ack "printed" (or "failed" if the printer gave up)
 *
 * A linked printer also LISTENS (cloud_realtime.c): it keeps a socket open to the cloud and hears
 * of a new print the moment it is made. What it hears is only "there is a job with this id": the
 * job is then downloaded, printed and reported right here, by the same code as a job found at a
 * check-in (cloud_print_command -> offer_job -> start_job). While the socket is up, the check-ins
 * say so ("X-Realtime: 1") and the cloud spaces them 15 minutes apart: they are only the backup.
 * When the socket is down they go back to being the way prints arrive, as often as before.
 * One cloud job is in the print queue at a time, oldest first, so cloud jobs never crowd out
 * local prints. A job that is only waiting (for its turn, for paper, for the cover) is not
 * reported: the report goes out when it has really printed.
 * Jobs already finished are remembered (cloud_jobs.c): if one is offered again, its report was
 * lost, so it is reported again and not printed again.
 */
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "api.h"
#include "cloud.h"
#include "cloud_http.h"
#include "cloud_jobs.h"
#include "cloud_realtime.h"
#include "cloud_state.h"
#include "cloud_ticket.h"
#include "cloud_update.h"
#include "paper_code.h"

static const char *TAG = "cloud";

#define TASK_STACK       12288   // HTTPS needs a deep stack
#define TASK_PRIORITY    3       // below the web server, the print queue and the automations: it never holds them up
#define JSON_MAX         8192    // the biggest JSON answer accepted
#define JOBS_JSON_MAX    16384   // ... and for a check-in, which lists up to 5 jobs and 10 commands
#define HEARD_MAX        8       // prints heard on the socket that wait their turn; more than this are found at a check-in
#define PAIR_EVERY_S     3       // "am I linked yet?" this often, unless the cloud says otherwise
#define PAIR_LIFETIME_S  600     // how long a pairing code is good for, unless the cloud says otherwise
#define PAIR_GRACE_S     20      // keep asking this long after the code should have run out: the cloud has the last word
#define POLL_EVERY_S     10      // check in for jobs this often when the cloud's answer does not say how often
#define POLL_FASTEST_S   5       // what the cloud asks for ("next_poll_seconds") is kept within these two
#define POLL_SLOWEST_S   900     // ... and when the cloud cannot be reached, the wait doubles each time, up to this
#define POLL_RETRY_MIN_S 10      // after a check-in that failed, never try again sooner than this
#define REPORT_RETRY_S   30      // a report on a job that did not get through goes again after this (or at the next check-in, if sooner)
#define DOWNLOAD_TRIES   5       // a print body that will not download is given up on after this many check-ins

#define MSG_UNREACHABLE "Can't reach TikPrint Cloud. Check the Wi-Fi."
#define MSG_REFUSED     "TikPrint Cloud did not accept this printer."
#define MSG_TOO_MANY    "Too many tries. Wait a few minutes."
#define MSG_NO_TICKET   "The ticket could not print. Check the printer."

static SemaphoreHandle_t s_lock;      // guards everything below
static SemaphoreHandle_t s_wake;      // given whenever the task should look again
static cloud_status_t s_status;
static bool s_task_running;
static bool s_want_pair;              // the TikPrint Cloud button was pressed on a printer that is not linked
static bool s_want_unlink;            // Reset / Unlink was pressed: tell the cloud, with s_unlink_token
static char s_unlink_token[CLOUD_TOKEN_MAX];   // the token that was just deleted, kept only until the cloud has been told
static char s_pair_code[CLOUD_CODE_MAX];   // while pairing: the code on the ticket. It never leaves this file
static int64_t s_pair_until_us;       // when the pairing code runs out (board uptime)
static int s_pair_every_s;
static int64_t s_pair_asked_us;       // when a pairing ticket was last asked for (0 = never): see CLOUD_TICKET_EVERY_S
static paper_code_t s_unlink_code;    // the code on the unlink ticket, while one is out
static int64_t s_unlink_asked_us;     // when an unlink ticket was last asked for (0 = never)
/* The print queue's task tells the cloud task that the cloud job in the queue has ended: */
static bool s_done_ready;             // there is news
static uint32_t s_done_ticket;        // which ticket it is about (s_in_queue.ticket)
static esp_err_t s_done_result;       // ESP_OK = printed
/* The Realtime task tells the cloud task: */
static bool s_poll_now;               // check in now, not when the wait is over
static struct {
    char id[CLOUD_JOB_ID_MAX];
    int size;
} s_heard[HEARD_MAX];                 // prints heard on the socket, oldest first, not yet in the print queue
static int s_heard_count;

/* The rest is only touched by the cloud task. */
static int s_poll_every_s = POLL_EVERY_S;   // what the last good answer said: the cloud picks it, and may change it every time
static int s_poll_failures;           // check-ins in a row that got no answer
static bool s_poll_was_listening;     // s_poll_every_s came from a check-in that said "X-Realtime: 1" (so it is 15 minutes)
static int64_t s_next_poll_us;        // when the next check-in is due (board uptime). 0 = now
static bool s_poll_after_print;       // the last check-in handed out prints: check in again once one has printed
static bool s_retry_soon;             // a job could not be taken just now (queue full, download failed): look again soon
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
    out->task_running = s_task_running;
    if (out->phase == CLOUD_PAIRING) {
        int64_t left = (s_pair_until_us - esp_timer_get_time()) / 1000000;
        out->expires_in_s = left > 0 ? (int)left : 0;
    }
    xSemaphoreGive(s_lock);
}

/* Change where linking stands. code = the pairing code on the ticket (kept here, never told to the
 * page) and message = what the page is told went wrong; both may be NULL (= none). */
static void set_phase(cloud_phase_t phase, const char *code, const char *message)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cloud_phase_t before = s_status.phase;
    s_status.phase = phase;
    strlcpy(s_pair_code, code ? code : "", sizeof(s_pair_code));
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

/* A pairing code is letters, digits and dashes: it goes into an address and onto the ticket as it is. */
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

/* The TikPrint Cloud button was pressed: get a code and print the ticket. This is the first call a printer ever makes. */
static void pair_start(void)
{
    cloud_reply_t reply;
    cloud_http(&(cloud_request_t){ .name = "pair/start", .post = true, .path = "/api/device/pair/start", .auth = CLOUD_AUTH_DEVICE,
                                   .max_bytes = JSON_MAX }, &reply);
    cJSON *json = reply_json(&reply);
    const char *code = text_of(json, "code"), *link = text_of(json, "link_url");
    if (reply.status != 200) {
        ESP_LOGW(TAG, "pair/start: %d %s", reply.status, text_of(json, "error"));
        set_phase(CLOUD_FAILED, NULL, reply.status == 429 ? MSG_TOO_MANY : is_temporary(reply.status) ? MSG_UNREACHABLE : MSG_REFUSED);
    } else if (!code_is_plain(code) || strncmp(link, "https://", 8) != 0) {
        ESP_LOGW(TAG, "pair/start: the answer has no usable code or link");
        set_phase(CLOUD_FAILED, NULL, MSG_UNREACHABLE);
    } else {
        int lifetime_s = number_of(json, "expires_in_s", PAIR_LIFETIME_S, 30, 3600);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_pair_every_s = number_of(json, "poll_interval_s", PAIR_EVERY_S, 2, 30);
        s_pair_until_us = esp_timer_get_time() + (int64_t)lifetime_s * 1000000;
        xSemaphoreGive(s_lock);
        // The ticket is the only place the code is shown. If it cannot join the print queue (queue
        // full, no memory) there is nothing to scan, so pairing stops here and the TikPrint Cloud button starts it again.
        esp_err_t err = cloud_ticket_print(link, code, (lifetime_s + 30) / 60);
        if (err == ESP_OK) {
            set_phase(CLOUD_PAIRING, code, NULL);
        } else {
            ESP_LOGW(TAG, "the pairing ticket did not join the print queue: %s", esp_err_to_name(err));
            set_phase(CLOUD_FAILED, NULL, MSG_NO_TICKET);
        }
    }
    cJSON_Delete(json);
    cloud_reply_free(&reply);
}

/* "Am I linked yet?" Asked every few seconds while the ticket is out. */
static void pair_ask(void)
{
    char code[CLOUD_CODE_MAX];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(code, s_pair_code, sizeof(code));
    bool overdue = esp_timer_get_time() > s_pair_until_us + (int64_t)PAIR_GRACE_S * 1000000;
    xSemaphoreGive(s_lock);

    char path[80];
    snprintf(path, sizeof(path), "/api/device/pair/status?code=%s", code);
    cloud_reply_t reply;
    cloud_http(&(cloud_request_t){ .name = "pair/status", .path = path, .auth = CLOUD_AUTH_DEVICE, .max_bytes = JSON_MAX }, &reply);
    cJSON *json = reply_json(&reply);
    const char *status = text_of(json, "status"), *error = text_of(json, "error");

    if (reply.status == 200 && strcmp(status, "linked") == 0) {
        // Save the token at once. If this answer had been lost, the next one would carry a fresh
        // token and cancel this one: the newest always wins, and it is the only one ever saved.
        if (cloud_state_save_link(text_of(json, "device_token"), text_of(json, "printer_id")) == ESP_OK) {
            ESP_LOGI(TAG, "linked as \"%s\": device token saved", text_of(json, "name"));   // the token is never logged
            s_next_poll_us = 0;   // check in straight away
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
        // unknown_code, unknown_device, or linked with a token this board never got: start over with the TikPrint Cloud button
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

/* The printer is not linked any more: stop listening, and forget the jobs and commands of the
 * account it belonged to. */
static void forget_cloud_work(void)
{
    cloud_realtime_stop();       // closes the socket and wipes its session before anything else
    cloud_jobs_clear();
    cloud_update_clear();
    s_in_queue.active = false;   // if that ticket is still in the queue it prints; nobody is told
    s_stuck.id[0] = 0;
    s_next_poll_us = 0;
    s_poll_after_print = s_retry_soon = s_poll_was_listening = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_heard_count = 0;
    s_poll_now = false;
    xSemaphoreGive(s_lock);
}

/* The cloud says this token is not valid any more: the owner unlinked the printer on the website,
 * or linked it again elsewhere. Delete the token and go back to local only: from here the board
 * calls nobody until the TikPrint Cloud button is pressed again. */
static void go_local(const char *why)
{
    ESP_LOGW(TAG, "%s: token deleted, back to local only", why);
    cloud_state_clear();
    forget_cloud_work();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    paper_code_clear(&s_unlink_code);   // an unlink ticket that is out has nothing left to unlink
    xSemaphoreGive(s_lock);
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

/* Has the cloud job in the print queue ended? Then note how, to be reported. True if one has. */
static bool collect_print_done(void)
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
        return true;
    }
    return false;
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
        cloud_http(&(cloud_request_t){ .name = "jobs/ack", .post = true, .path = path, .auth = CLOUD_AUTH_TOKEN,
                                       .token = token, .json = json, .max_bytes = JSON_MAX }, &reply);
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
    } else {
        s_retry_soon = true;
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
    cloud_http(&(cloud_request_t){ .name = "jobs/body", .path = path, .auth = CLOUD_AUTH_TOKEN, .token = token,
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
            s_retry_soon = true;
        }
    }
    cloud_reply_free(&reply);
    return true;
}

/*
 * The cloud has a job for this printer: it was listed at a check-in, or heard on the socket. Both
 * end up here, so a job prints once however many times and ways it is offered.
 * *report: set if there is something to report at once. *waiting: set if the job has to wait its
 * turn. False if the printer turned out to be unlinked.
 */
static bool offer_job(const char *token, const char *id, int size, bool *report, bool *waiting)
{
    cloud_job_end_t end;
    if (!job_id_is_plain(id)) {
        return true;
    }
    if (cloud_jobs_find(id, &end)) {
        // Finished already, yet offered again: the report was lost. Report again; do not print again.
        ESP_LOGI(TAG, "cloud job %.8s is offered again: reporting it again, not printing it again", id);
        cloud_jobs_report_again(id);
        *report = true;
    } else if (s_in_queue.active) {
        // A cloud job is in the print queue. This one waits its turn on the cloud (unless it is that very job).
        *waiting = *waiting || strcmp(id, s_in_queue.id) != 0;
    } else {
        if (!start_job(token, id, size < 0 ? 0 : size > API_PRINT_BODY_MAX ? API_PRINT_BODY_MAX + 1 : size)) {
            return false;
        }
        *report = *report || cloud_jobs_find(id, &end);   // it could not print: say so now
        *waiting = true;                                  // (or it is printing: there may be more after it)
    }
    return true;
}

/* One check-in. Returns how many seconds to wait before the next; 0 if the printer turned out to be unlinked. */
static int check_in(const char *token)
{
    // While the socket is up, say so: the cloud then answers "come back in 15 minutes", and lists
    // every command that is still open, in case one was missed on the socket.
    bool listening = cloud_realtime_is_listening();
    cloud_reply_t reply;
    cloud_http(&(cloud_request_t){ .name = "jobs", .path = "/api/device/jobs", .auth = CLOUD_AUTH_TOKEN,
                                   .token = token, .listening = listening, .max_bytes = JOBS_JSON_MAX }, &reply);
    if (says_unlinked(&reply)) {
        cloud_reply_free(&reply);
        go_local("the cloud refused the token");
        return 0;
    }
    cJSON *json = reply.status == 200 ? reply_json(&reply) : NULL;
    cloud_reply_free(&reply);
    if (!json) {
        // No network, a timeout, a 5xx, a 429, an answer that is not JSON: nothing is wrong with the
        // link. Keep the token and try again, a little later each time. The first wait is what the
        // last good answer said (never sooner than that, and never sooner than POLL_RETRY_MIN_S).
        // The 15 minutes of a listening printer do not count once the socket is down: then the
        // check-ins are how prints arrive again, and the waits start from POLL_RETRY_MIN_S.
        int every_s = s_poll_was_listening && !listening ? POLL_RETRY_MIN_S : s_poll_every_s;
        s_poll_failures = s_poll_failures < 8 ? s_poll_failures + 1 : 8;
        int wait = (every_s > POLL_RETRY_MIN_S ? every_s : POLL_RETRY_MIN_S) << (s_poll_failures - 1);
        return wait > POLL_SLOWEST_S ? POLL_SLOWEST_S : wait;
    }
    s_poll_failures = 0;
    // How long to wait before the next check-in: the cloud says, in every answer ("next_poll_seconds").
    // Servers from before that only send "poll_interval_s". Neither there: POLL_EVERY_S.
    s_poll_every_s = number_of(json, "next_poll_seconds",
                               number_of(json, "poll_interval_s", POLL_EVERY_S, POLL_FASTEST_S, POLL_SLOWEST_S),
                               POLL_FASTEST_S, POLL_SLOWEST_S);
    s_poll_was_listening = listening;

    bool linked = true, report = false, waiting = false;
    const cJSON *commands = cJSON_GetObjectItem(json, "commands"), *item;
    if (cJSON_IsArray(commands)) {
        // Everything that is still open, oldest first: the same rows the socket delivers. The prints
        // among them are the same as in "jobs", so "jobs" is not read as well.
        cJSON_ArrayForEach(item, commands) {
            const char *type = text_of(item, "type");
            const cJSON *payload = cJSON_GetObjectItem(item, "payload");
            if (strcmp(type, "print") == 0) {
                const char *id = text_of(payload, "id");
                linked = offer_job(token, id[0] ? id : text_of(item, "id"), number_of(payload, "size", 0, 0, API_PRINT_BODY_MAX + 1),
                                   &report, &waiting);
            } else if (strcmp(type, "update") == 0) {
                cloud_update_command(text_of(item, "id"), text_of(payload, "version"));   // a version, never an address
            }
            if (!linked) {
                break;
            }
        }
    } else {
        // A site from before commands: only "jobs".
        cJSON_ArrayForEach(item, cJSON_GetObjectItem(json, "jobs")) {   // oldest first
            linked = offer_job(token, text_of(item, "id"), number_of(item, "size", 0, 0, API_PRINT_BODY_MAX + 1), &report, &waiting);
            if (!linked) {
                break;
            }
        }
    }
    cJSON_Delete(json);
    if (linked && report) {
        linked = send_reports(token);
    }
    s_poll_after_print = waiting;   // more may be waiting on the cloud: check in again when the ticket is out
    return linked ? s_poll_every_s : 0;
}

/* ---------- Listening (see cloud_realtime.h) ---------- */

void cloud_wake(void)
{
    xSemaphoreGive(s_wake);
}

void cloud_poll_now(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_poll_now = true;
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_wake);
}

void cloud_print_command(const char *id, int size)
{
    if (!id || !job_id_is_plain(id)) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool known = false;
    for (int i = 0; i < s_heard_count; i++) {
        known = known || strcmp(s_heard[i].id, id) == 0;
    }
    if (known) {
        // heard twice: once is enough
    } else if (s_heard_count < HEARD_MAX) {
        strlcpy(s_heard[s_heard_count].id, id, sizeof(s_heard[0].id));
        s_heard[s_heard_count].size = size;
        s_heard_count++;
    } else {
        s_poll_now = true;   // no room to remember it: the check-in lists it
    }
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_wake);
}

/* The oldest print heard on the socket that is not in the print queue yet. False if there is none. */
static bool take_heard(char id[CLOUD_JOB_ID_MAX], int *size)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool have = s_heard_count > 0;
    if (have) {
        strlcpy(id, s_heard[0].id, CLOUD_JOB_ID_MAX);
        *size = s_heard[0].size;
        s_heard_count--;
        memmove(&s_heard[0], &s_heard[1], (size_t)s_heard_count * sizeof(s_heard[0]));
    }
    xSemaphoreGive(s_lock);
    return have;
}

/* A text field of the cloud's answer into a fixed place. False if it is missing or does not fit. */
static bool take_text(const cJSON *json, const char *name, char *out, size_t size)
{
    const char *text = text_of(json, name);
    return text[0] && strlcpy(out, text, size) < size;
}

/* The Realtime task needs a session: the address of the socket and a short-lived token for it.
 * Only this task holds the device token, so it asks, and hands the answer over. Nothing of it is
 * saved to flash or logged. False if the printer turned out to be unlinked. */
static bool fetch_session(const char *token)
{
    cloud_reply_t reply;
    cloud_http(&(cloud_request_t){ .name = "realtime", .post = true, .path = "/api/device/realtime", .auth = CLOUD_AUTH_TOKEN,
                                   .token = token, .max_bytes = JSON_MAX }, &reply);
    if (says_unlinked(&reply)) {
        cloud_reply_free(&reply);
        go_local("the cloud refused the token");
        return false;
    }
    cJSON *json = reply_json(&reply);
    cloud_realtime_result_t result = CLOUD_REALTIME_SESSION_FAILED;
    cloud_realtime_session_t *session = NULL;
    if (reply.status == 200 && json && (session = heap_caps_calloc(1, sizeof(*session), MALLOC_CAP_SPIRAM)) != NULL) {
        char mine[CLOUD_PRINTER_ID_MAX];
        cloud_state_get_printer_id(mine, sizeof(mine));
        session->expires_in_s = number_of(json, "expires_in_s", 3600, 60, 24 * 3600);
        session->heartbeat_s = number_of(json, "heartbeat_s", 60, 15, 900);
        bool whole = take_text(json, "supabase_url", session->supabase_url, sizeof(session->supabase_url)) &&
                     take_text(json, "realtime_url", session->realtime_url, sizeof(session->realtime_url)) &&
                     take_text(json, "apikey", session->apikey, sizeof(session->apikey)) &&
                     take_text(json, "access_token", session->access_token, sizeof(session->access_token)) &&
                     take_text(json, "printer_id", session->printer_id, sizeof(session->printer_id)) &&
                     take_text(json, "topic", session->topic, sizeof(session->topic)) &&
                     take_text(json, "table", session->table, sizeof(session->table)) &&
                     take_text(json, "filter", session->filter, sizeof(session->filter));
        // The session must be for this printer (if the board knows its printer id).
        if (whole && (!mine[0] || strcmp(mine, session->printer_id) == 0)) {
            result = CLOUD_REALTIME_SESSION_OK;
        } else {
            ESP_LOGW(TAG, "realtime: the answer is not a whole session for this printer");
        }
    } else if (reply.status == 503 && strcmp(text_of(json, "error"), "realtime_off") == 0) {
        result = CLOUD_REALTIME_SESSION_OFF;
    }
    cloud_realtime_session(result, session);
    // The answer had a token in it: wipe every copy before the memory is given back.
    if (session) {
        memset(session, 0, sizeof(*session));
        free(session);
    }
    cJSON_Delete(json);
    if (reply.body) {
        memset(reply.body, 0, reply.len);
    }
    cloud_reply_free(&reply);
    return true;
}

/* What a linked printer does, over and over: report what has printed, fetch a session if the
 * Realtime task needs one, start the prints heard on the socket, check in when it is time, then
 * sleep until the next check-in is due or something wakes the task. */
static void linked_step(void)
{
    char token[CLOUD_TOKEN_MAX];
    if (!cloud_state_get_token(token, sizeof(token))) {
        set_phase(CLOUD_LOCAL, NULL, NULL);   // the token is gone: nothing to check in with
        return;
    }
    bool printed = collect_print_done();
    if (!send_reports(token)) {
        return;
    }
    if (cloud_realtime_wants_session() && !fetch_session(token)) {
        return;
    }

    // Prints heard on the socket: one at a time into the print queue, oldest first.
    char id[CLOUD_JOB_ID_MAX];
    int size;
    bool report = false, waiting = false;
    while (!s_in_queue.active && take_heard(id, &size)) {
        if (!offer_job(token, id, size, &report, &waiting)) {
            return;
        }
    }
    if (report && !send_reports(token)) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool poll_now = s_poll_now;
    s_poll_now = false;
    xSemaphoreGive(s_lock);
    int64_t now_us = esp_timer_get_time();
    if (printed && s_poll_after_print) {
        poll_now = true;   // the last check-in had prints in it: see if more are waiting
    }
    if (s_retry_soon) {
        // A job could not be taken just now. With the socket up the next check-in is 15 minutes
        // away, which is too long to leave it: look again shortly. (Without the socket, the
        // check-ins come often enough as they are.)
        s_retry_soon = false;
        int64_t soon_us = now_us + (int64_t)POLL_RETRY_MIN_S * 1000000;
        if (cloud_realtime_is_listening() && s_next_poll_us > soon_us) {
            s_next_poll_us = soon_us;
        }
    }
    if (poll_now || now_us >= s_next_poll_us) {
        int wait = check_in(token);
        if (wait <= 0) {
            return;   // not linked any more
        }
        now_us = esp_timer_get_time();
        s_next_poll_us = now_us + (int64_t)wait * 1000000;
        if (s_retry_soon) {
            return;   // go round once more, to bring the next check-in forward
        }
    }
    // Sleep until the next check-in, or until the ticket has printed, something is heard on the
    // socket, Wi-Fi comes back, or a button on the page is pressed.
    int64_t left_ms = (s_next_poll_us - now_us) / 1000;
    cloud_job_end_t end;
    if (cloud_jobs_next_unreported(id, &end) && left_ms > REPORT_RETRY_S * 1000) {
        left_ms = REPORT_RETRY_S * 1000;   // a report did not get through: do not leave it until a check-in that may be 15 minutes away
    }
    xSemaphoreTake(s_wake, pdMS_TO_TICKS(left_ms < 1 ? 1 : left_ms));
}

/* ---------- Unlinking ---------- */

static esp_err_t start_task_locked(void);

/* The code from the unlink ticket was typed in and the token is deleted already (unlink_now). Tell
 * the cloud, so
 * that the website stops listing the printer. Whatever the answer, or with none at all, the
 * printer stays unlinked. */
static void tell_cloud_unlinked(void)
{
    char token[CLOUD_TOKEN_MAX];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(token, s_unlink_token, sizeof(token));
    memset(s_unlink_token, 0, sizeof(s_unlink_token));
    xSemaphoreGive(s_lock);
    forget_cloud_work();
    if (token[0]) {
        cloud_reply_t reply;
        cloud_http(&(cloud_request_t){ .name = "unlink", .post = true, .path = "/api/device/unlink",
                                       .auth = CLOUD_AUTH_TOKEN, .token = token, .max_bytes = JSON_MAX }, &reply);
        ESP_LOGI(TAG, "unlinked: %s", reply.status == 200 || reply.status == 401 ? "the cloud knows"
                 : "the cloud was not reached, the website will still list the printer");
        cloud_reply_free(&reply);
        memset(token, 0, sizeof(token));
    }
}

/* Unlink, here and now. Call with s_lock held. */
static void unlink_now_locked(void)
{
    // Keep the token for one last call, then delete it from the board before anything else happens.
    if (cloud_state_get_token(s_unlink_token, sizeof(s_unlink_token))) {
        s_want_unlink = true;
    }
    cloud_state_clear();
    s_want_pair = false;
    s_pair_code[0] = 0;
    s_status = (cloud_status_t){ .phase = CLOUD_LOCAL };
    if (s_want_unlink) {
        start_task_locked();   // if it cannot start, the cloud is not told; the printer is unlinked all the same
    }
}

/* Seconds until a ticket last asked for at asked_us may be asked for again; 0 = now. */
static int ticket_wait_s(int64_t asked_us, int64_t now_us)
{
    int64_t left_us = asked_us ? asked_us + (int64_t)CLOUD_TICKET_EVERY_S * 1000000 - now_us : 0;
    return left_us > 0 ? (int)((left_us + 999999) / 1000000) : 0;
}

esp_err_t cloud_unlink_request(int *expires_in_s, int *wait_s)
{
    *expires_in_s = *wait_s = 0;
    if (!cloud_state_is_linked()) {
        return ESP_ERR_INVALID_STATE;
    }
    int64_t now_us = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *wait_s = ticket_wait_s(s_unlink_asked_us, now_us);
    if (*wait_s > 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FINISHED;
    }
    int64_t asked_before_us = s_unlink_asked_us;
    s_unlink_asked_us = now_us ? now_us : 1;   // taken now, so two requests at once print one ticket
    paper_code_clear(&s_unlink_code);          // the code before stops working, whether or not the new ticket prints
    xSemaphoreGive(s_lock);

    // The code is made and printed first, and only checked against once its ticket is in the queue.
    paper_code_t fresh;
    paper_code_new(&fresh, esp_random, now_us);
    esp_err_t err = cloud_ticket_print_unlink(fresh.code, (PAPER_CODE_LIFE_S + 30) / 60);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (err == ESP_OK) {
        s_unlink_code = fresh;
        *expires_in_s = paper_code_seconds_left(&fresh, now_us);
    } else {
        s_unlink_asked_us = asked_before_us;   // no paper was used: it may be asked for again at once
    }
    xSemaphoreGive(s_lock);
    paper_code_clear(&fresh);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "the unlink ticket did not join the print queue: %s", esp_err_to_name(err));
        return err == ESP_ERR_NO_MEM ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    ESP_LOGI(TAG, "Reset / Unlink pressed: a ticket with a code is printing, good for %d s", PAPER_CODE_LIFE_S);   // the code is never logged
    return ESP_OK;
}

esp_err_t cloud_unlink_confirm(const char *code, int *tries_left)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    paper_code_result_t result = paper_code_check(&s_unlink_code, code, esp_timer_get_time(), tries_left);
    if (result == PAPER_CODE_RIGHT) {
        unlink_now_locked();
    }
    xSemaphoreGive(s_lock);
    switch (result) {
    case PAPER_CODE_RIGHT:
        xSemaphoreGive(s_wake);
        ESP_LOGW(TAG, "Reset / Unlink confirmed with the code on the ticket: token deleted, back to local only");
        return ESP_OK;
    case PAPER_CODE_NONE:
        return ESP_ERR_NOT_FOUND;
    default:
        ESP_LOGW(TAG, "Reset / Unlink: wrong code, %d %s left%s", *tries_left, *tries_left == 1 ? "try" : "tries",
                 result == PAPER_CODE_CANCELLED ? ": the code is cancelled" : "");
        return ESP_ERR_INVALID_ARG;
    }
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
            cloud_realtime_start();   // listen for as long as the printer is linked (does nothing if already listening)
            linked_step();
        } else {
            // Nothing left to do: close the connection and end the task. From here the board calls
            // nobody, until the TikPrint Cloud button is pressed again.
            cloud_realtime_stop();   // (stopped already when the token was deleted: this is the belt to those braces)
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

esp_err_t cloud_pair(int *wait_s)
{
    *wait_s = 0;
    if (cloud_state_is_linked()) {
        return ESP_ERR_INVALID_STATE;
    }
    int64_t now_us = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // Every press prints a ticket, and anyone on the network can press it: not too often.
    *wait_s = ticket_wait_s(s_pair_asked_us, now_us);
    if (*wait_s > 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FINISHED;
    }
    esp_err_t err = start_task_locked();
    if (err == ESP_OK) {
        s_pair_asked_us = now_us ? now_us : 1;
        s_want_pair = true;
        s_pair_code[0] = 0;   // the code of the ticket before stops being asked about
        s_status = (cloud_status_t){ .phase = CLOUD_STARTING };
    }
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_wake);
    return err;
}

/* Wi-Fi is back (the board got an address): check in now, not when a long wait is over. This only
 * wakes the cloud task. On a printer that is not linked there is no task, so still nobody is called. */
static void on_wifi_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    cloud_poll_now();
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
    cloud_update_init();
    esp_err_t rt = cloud_realtime_init();
    if (rt != ESP_OK) {
        return rt;
    }
    esp_err_t heard = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_got_ip, NULL);
    if (heard != ESP_OK) {
        return heard;
    }
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
