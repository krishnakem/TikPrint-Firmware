/*
 * Automations: tickets the board makes by itself (weather for now; a schedule and a
 * connector come next).
 *
 * Fetching from the internet takes seconds and needs a deep stack, and the web server must
 * keep answering meanwhile. So all of it happens on one background task here. Others hand it
 * a job through a small mailbox (s_jobs) and read the outcome from s_weather, guarded by s_lock.
 *
 * Tickets made here go into the print queue like any other ticket (printer_print_job).
 * They print in the printer's built-in font: there is no browser to draw them.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "automations.h"
#include "printer.h"

static const char *TAG = "automations";

#define TASK_STACK    12288   // HTTPS needs a deep stack
#define TASK_PRIORITY 4
#define MAX_JOBS      4       // jobs that can wait in the mailbox

typedef enum { JOB_WEATHER_LOCATE, JOB_WEATHER_PRINT } job_type_t;

typedef struct {
    job_type_t type;
    weather_kind_t kind;
} job_t;

static QueueHandle_t s_jobs;
static SemaphoreHandle_t s_lock;               // guards the three below
static automations_weather_status_t s_weather;
static int64_t s_weather_done_us;
static bool s_print_pending;                   // a "print now" is in the mailbox or being worked on

/* Note how a weather print ended. */
static void weather_finished(bool ok, const char *error, const char *message, uint32_t ticket)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_weather.ran = true;
    s_weather.ok = ok;
    strlcpy(s_weather.error, error, sizeof(s_weather.error));
    strlcpy(s_weather.message, message, sizeof(s_weather.message));
    s_weather.ticket = ticket;
    s_weather_done_us = esp_timer_get_time();
    s_print_pending = false;
    xSemaphoreGive(s_lock);
    if (ok) {
        ESP_LOGI(TAG, "weather ticket #%04u joined the print queue", (unsigned)ticket);
    } else {
        ESP_LOGW(TAG, "no weather ticket: %s (%s)", error, message);
    }
}

static void weather_print(weather_kind_t kind, bool spread_load)
{
    // The text is built in PSRAM (the big external RAM); the print queue then takes its own copy.
    char source[WEATHER_SOURCE_MAX], title[WEATHER_TITLE_MAX];
    char *body = heap_caps_malloc(WEATHER_BODY_MAX, MALLOC_CAP_SPIRAM);
    if (!body) {
        weather_finished(false, "no_memory", "The printer is out of memory", 0);
        return;
    }
    ESP_LOGI(TAG, "making a weather ticket (%s)", weather_kind_name(kind));
    weather_result_t res = weather_make_ticket(kind, spread_load, source, title, body);
    if (res != WEATHER_OK) {
        weather_finished(false, weather_result_name(res), weather_result_message(res), 0);
    } else {
        printer_job_t job = { .title = title, .body = body, .source = source };
        printer_ticket_t ticket;
        if (printer_print_job(&job, &ticket) == ESP_OK) {
            weather_finished(true, "", "", ticket.number);
        } else {
            weather_finished(false, "queue_full", "The print queue is full, try again in a moment", 0);
        }
    }
    free(body);
}

static void automations_task(void *arg)
{
    for (;;) {
        job_t job;
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (job.type == JOB_WEATHER_PRINT) {
            weather_print(job.kind, false);   // asked for by a person: no random wait
        } else {
            weather_result_t res = weather_locate();
            if (res != WEATHER_OK && res != WEATHER_NO_ZIP) {
                ESP_LOGW(TAG, "could not look up the ZIP code's place yet: %s", weather_result_name(res));
            }
        }
    }
}

esp_err_t automations_weather_print_now(weather_kind_t kind)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool busy = s_print_pending;
    s_print_pending = true;
    xSemaphoreGive(s_lock);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }
    job_t job = { .type = JOB_WEATHER_PRINT, .kind = kind };
    if (xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_print_pending = false;
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

void automations_weather_zip_changed(void)
{
    job_t job = { .type = JOB_WEATHER_LOCATE };
    xQueueSend(s_jobs, &job, 0);   // if the mailbox is full, the place is looked up with the next print instead
}

void automations_get_weather_status(automations_weather_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_weather;
    out->busy = s_print_pending;
    out->s_ago = s_weather.ran ? (int)((esp_timer_get_time() - s_weather_done_us) / 1000000) : 0;
    xSemaphoreGive(s_lock);
}

esp_err_t automations_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_jobs = xQueueCreate(MAX_JOBS, sizeof(job_t));
    if (!s_lock || !s_jobs) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(automations_task, "automations", TASK_STACK, NULL, TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
