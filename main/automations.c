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
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "nvs.h"

#include "automations.h"
#include "clock.h"
#include "printer.h"
#include "settings.h"
#include "ticket_draw.h"
#include "weather_icons.h"
#include "weather_render.h"

static const char *TAG = "automations";

#define TASK_STACK    12288   // HTTPS needs a deep stack
#define TASK_PRIORITY 4
#define MAX_JOBS      4       // jobs that can wait in the mailbox
#define TICK_MS          10000   // how often the task looks at the clock (and retries a town lookup)
#define LOCATE_RETRIES   30      // town lookups that may fail in a row (5 minutes), then wait for the next print or ZIP code change
#define SLOT_WINDOW_MIN  15      // a scheduled print that cannot get the weather keeps trying this long, then is skipped
#define SLOT_RETRY_MS    60000   // ... trying this often
#define NVS_NS           "auto"

typedef enum { JOB_WEATHER_LOCATE, JOB_WEATHER_PRINT, JOB_WEATHER_SLOT } job_type_t;

typedef struct {
    job_type_t type;
    weather_kind_t kind;
    int slot;
} job_t;

/* How a try at a weather ticket ended. */
typedef enum { PRINT_OK, PRINT_TRY_LATER, PRINT_FAILED } print_result_t;

/* What each slot of the schedule prints. */
static const weather_kind_t SLOT_KIND[SETTINGS_WEATHER_SLOTS] = { WEATHER_TODAY, WEATHER_REST_OF_TODAY, WEATHER_TOMORROW };

/* Which scheduled prints are done, so that none prints twice and the daily limit holds even
 * if the board restarts. Saved on the board as one block. Days are numbers like 20261121. */
typedef struct {
    int32_t slot_day[SETTINGS_WEATHER_SLOTS];   // the day each slot last printed or was skipped
    int32_t count_day;                          // the day "count" is about
    int32_t count;                              // scheduled tickets printed that day
} fired_t;

static QueueHandle_t s_jobs;
static SemaphoreHandle_t s_lock;               // guards the three below
static automations_weather_status_t s_weather;
static int64_t s_weather_done_us;
static bool s_print_pending;                   // a "print now" is in the mailbox or being worked on
static fired_t s_fired;                        // guarded by s_lock too

/* Note how a weather print ended. manual: it was asked for with "print now". */
static void weather_finished(bool manual, bool ok, const char *error, const char *message, uint32_t ticket)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_weather.ran = true;
    s_weather.ok = ok;
    strlcpy(s_weather.error, error, sizeof(s_weather.error));
    strlcpy(s_weather.message, message, sizeof(s_weather.message));
    s_weather.ticket = ticket;
    s_weather_done_us = esp_timer_get_time();
    if (manual) {
        s_print_pending = false;
    }
    xSemaphoreGive(s_lock);
    if (ok) {
        ESP_LOGI(TAG, "weather ticket #%04u joined the print queue", (unsigned)ticket);
    } else {
        ESP_LOGW(TAG, "no weather ticket: %s (%s)", error, message);
    }
}

/* Fetch the weather and put a ticket in the print queue.
 *
 * The ticket is drawn here, as dots, in the font chosen on the page (weather_render.c). If
 * that font is "Printer" (or the drawing cannot be done), the printer's own built-in font
 * prints the text instead, with the symbol as a small picture above the title. */
static print_result_t weather_print(weather_kind_t kind, bool manual)
{
    print_result_t result = PRINT_FAILED;
    settings_weather_look_t look;
    settings_t header;
    settings_get_weather_look(&look);
    settings_get(&header);
    int font = ticket_font_id(look.font);
    int header_font = ticket_font_id(header.header_font);

    // The text is built in PSRAM (the big external RAM); the print queue then takes its own copy.
    char source[WEATHER_SOURCE_MAX], title[WEATHER_TITLE_MAX];
    char *body = heap_caps_malloc(WEATHER_BODY_MAX, MALLOC_CAP_SPIRAM);
    char *rich = heap_caps_malloc(WEATHER_BODY_MAX, MALLOC_CAP_SPIRAM);
    uint8_t *strip = heap_caps_malloc(WEATHER_ICON_STRIP_BYTES, MALLOC_CAP_SPIRAM);   // the weather symbol, as an image
    ticket_canvas_t canvas = { 0 };
    weather_icon_t icon;
    if (!body || !rich || !strip) {
        free(body);
        free(rich);
        free(strip);
        weather_finished(manual, false, "no_memory", "The printer is out of memory", 0);
        return PRINT_TRY_LATER;
    }
    ESP_LOGI(TAG, "making a weather ticket (%s)", weather_kind_name(kind));
    weather_result_t res = weather_make_ticket(kind, look.hours, source, title, body, rich, &icon);
    if (res != WEATHER_OK) {
        weather_finished(manual, false, weather_result_name(res), weather_result_message(res), 0);
        result = (res == WEATHER_NO_WIFI || res == WEATHER_UNREACHABLE) ? PRINT_TRY_LATER : PRINT_FAILED;
    } else {
        printer_job_t job = { .title = title, .body = body, .source = source };
        bool drawn = font >= 0 && ticket_canvas_init(&canvas, WEATHER_RENDER_MAX_ROWS) &&
                     weather_render(&canvas, font, look.size, header_font >= 0 ? header_font : font, source, icon, title, rich);
        if (drawn) {
            job.image = canvas.bits;
            job.image_rows = (uint16_t)canvas.rows;
            job.text_in_image = true;   // header, title and text are all in the picture
        } else if (icon != WEATHER_ICON_NONE) {
            weather_icon_strip(icon, strip);
            job.image = strip;
            job.image_rows = WEATHER_ICON_STRIP_ROWS;
            job.image_first = true;     // above the title, not below the text
        }
        printer_ticket_t ticket;
        if (printer_print_job(&job, &ticket) == ESP_OK) {
            weather_finished(manual, true, "", "", ticket.number);
            result = PRINT_OK;
        } else {
            weather_finished(manual, false, "queue_full", "The print queue is full, try again in a moment", 0);
            result = PRINT_TRY_LATER;
        }
    }
    ticket_canvas_free(&canvas);
    free(body);
    free(rich);
    free(strip);
    return result;
}

/* ---------- The schedule ---------- */

static void fired_save(void)
{
    fired_t copy;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    copy = s_fired;
    xSemaphoreGive(s_lock);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "wx_fired", &copy, sizeof(copy));
        nvs_commit(h);
        nvs_close(h);
    }
}

static int32_t day_number(time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

/* Look at the clock: is it time for a scheduled weather ticket? Called every TICK_MS.
 *
 * A slot is "due" from its time until SLOT_WINDOW_MIN minutes later. In that window it is tried
 * until a ticket prints. If the weather cannot be had (Wi-Fi down, no answer), it is tried again
 * every minute, and skipped when the window ends. Each slot prints at most once a day, and all
 * slots together at most AUTOMATIONS_WEATHER_MAX_PER_DAY times. */
static void schedule_check(void)
{
    static int64_t retry_at_us[SETTINGS_WEATHER_SLOTS];   // not before this (board uptime)

    clock_status_t clk;
    settings_weather_schedule_t sched;
    clock_get_status(&clk);
    settings_get_weather_schedule(&sched);
    if (!clk.set || !sched.on) {
        return;   // without the time there is no schedule; it catches up once the clock is set
    }
    for (int i = 0; i < SETTINGS_WEATHER_SLOTS; i++) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        int late_min = (tm.tm_hour * 60 + tm.tm_min - sched.slot[i].minute + 24 * 60) % (24 * 60);
        if (!sched.slot[i].on || late_min >= SLOT_WINDOW_MIN) {
            continue;
        }
        int32_t day = day_number(now - late_min * 60);   // the day the slot started (matters just after midnight)
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool done = s_fired.slot_day[i] == day;
        bool limit = s_fired.count_day == day && s_fired.count >= AUTOMATIONS_WEATHER_MAX_PER_DAY;
        xSemaphoreGive(s_lock);
        if (done || limit || esp_timer_get_time() < retry_at_us[i]) {
            continue;
        }
        ESP_LOGI(TAG, "scheduled weather print %d (%s)", i + 1, weather_kind_name(SLOT_KIND[i]));
        print_result_t res = weather_print(SLOT_KIND[i], false);
        if (res == PRINT_TRY_LATER && late_min < SLOT_WINDOW_MIN - 1) {
            retry_at_us[i] = esp_timer_get_time() + SLOT_RETRY_MS * 1000LL;
            continue;
        }
        if (res != PRINT_OK) {
            ESP_LOGW(TAG, "scheduled weather print %d skipped for today", i + 1);
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_fired.slot_day[i] = day;
        if (res == PRINT_OK) {
            if (s_fired.count_day != day) {
                s_fired.count_day = day;
                s_fired.count = 0;
            }
            s_fired.count++;
        }
        xSemaphoreGive(s_lock);
        fired_save();
    }
}

/* Look up the town of the saved ZIP code. Returns how many more tries are left. */
static int locate(int locate_tries_left)
{
    weather_result_t res = weather_locate();
    bool done = res == WEATHER_OK || res == WEATHER_NO_ZIP;
    if (done || res == WEATHER_REFUSED) {
        locate_tries_left = 0;   // found, nothing to find, or refused (asking again would not help)
    } else {
        locate_tries_left--;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_weather.locating = locate_tries_left > 0;
    strlcpy(s_weather.locate_error, done ? "" : weather_result_name(res), sizeof(s_weather.locate_error));
    strlcpy(s_weather.locate_message, done ? "" : weather_result_message(res), sizeof(s_weather.locate_message));
    xSemaphoreGive(s_lock);
    if (!done) {
        ESP_LOGW(TAG, "could not look up the ZIP code's town (%s): %d more tries", weather_result_name(res),
                 locate_tries_left);
    }
    return locate_tries_left;
}

static void automations_task(void *arg)
{
    // At start-up: if a ZIP code is saved but its town is not known yet, look it up once Wi-Fi is there.
    int locate_tries_left = LOCATE_RETRIES;
    for (;;) {
        // Wait for a job, but never long: the schedule has to look at the clock.
        job_t job = { .type = JOB_WEATHER_LOCATE };
        bool got_job = xQueueReceive(s_jobs, &job, pdMS_TO_TICKS(TICK_MS)) == pdTRUE;
        if (got_job && job.type == JOB_WEATHER_PRINT) {
            weather_print(job.kind, true);
        } else if (got_job && job.type == JOB_WEATHER_SLOT) {
            weather_print(SLOT_KIND[job.slot], false);   // a test of a scheduled print
        } else if (got_job || locate_tries_left > 0) {
            locate_tries_left = locate(got_job ? LOCATE_RETRIES : locate_tries_left);   // a new ZIP code starts afresh
        }
        schedule_check();
    }
}

void automations_weather_slot_changed(int slot)
{
    if (slot < 0 || slot >= SETTINGS_WEATHER_SLOTS) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_fired.slot_day[slot] = 0;
    xSemaphoreGive(s_lock);
    fired_save();
}

esp_err_t automations_weather_run_slot(int slot)
{
    if (slot < 0 || slot >= SETTINGS_WEATHER_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }
    job_t job = { .type = JOB_WEATHER_SLOT, .slot = slot };
    return xQueueSend(s_jobs, &job, 0) == pdTRUE ? ESP_OK : ESP_ERR_INVALID_STATE;
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
    bool sent = xQueueSend(s_jobs, &job, 0) == pdTRUE;   // if the mailbox is full, the place is looked up with the next print instead
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_weather.locating = sent;   // so the page can say "finding..." from this very moment
    s_weather.locate_error[0] = s_weather.locate_message[0] = 0;
    xSemaphoreGive(s_lock);
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
    s_weather.locating = true;   // the task's first job is to look up a saved ZIP code's town
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        fired_t saved;
        size_t n = sizeof(saved);
        if (nvs_get_blob(h, "wx_fired", &saved, &n) == ESP_OK && n == sizeof(saved)) {
            s_fired = saved;
        }
        nvs_close(h);
    }
    if (xTaskCreate(automations_task, "automations", TASK_STACK, NULL, TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
