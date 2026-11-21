/*
 * Print queue: tickets wait here in line (first in, first out) and a background
 * task prints them one at a time.
 *
 * - Each waiting ticket is one block of PSRAM holding its own copy of everything:
 *   the text and the image dots. The block is freed when the ticket has printed.
 * - A ticket gets its number the moment it joins the line, so numbers follow the
 *   order the tickets arrived in and never repeat.
 * - Before each ticket the task asks "is the printer ready?" (paper in, cover closed).
 *   If not, the queue pauses: tickets stay in line and the task asks again every few
 *   seconds until the answer is yes. A printer that cannot answer counts as ready.
 * - Two tasks touch the queue: the web server adds tickets, the "print_queue" task
 *   takes them out. Everything they share is guarded by one lock (s_lock).
 */
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "print_queue.h"
#include "counter.h"

static const char *TAG = "print_queue";

/* ---- Limits ---- */
#define QUEUE_MAX_WAITING     8                    // tickets waiting in line (not counting the one printing)
#define QUEUE_MAX_IMAGE_BYTES (4 * 1024 * 1024)    // image dots held by the queue, waiting + printing (PSRAM)
#define QUEUE_TASK_STACK      6144                 // the task logs each ticket and feeds the printer
#define QUEUE_TASK_PRIORITY   5
#define PAUSED_RECHECK_MS     3000                 // while the printer is not ready: ask again this often
#define IDLE_RECHECK_MS       10000                // nothing to print: still ask this often, so the UI stays current

/* One ticket in line. The text and image it points at live in the same block, right after it. */
typedef struct {
    printer_job_t job;
    size_t image_bytes;
} queued_job_t;

static print_queue_print_fn s_print;
static print_queue_ready_fn s_ready;
static SemaphoreHandle_t s_lock;      // guards everything below
static SemaphoreHandle_t s_wake;      // given whenever the task should look at the queue again
static queued_job_t *s_slots[QUEUE_MAX_WAITING];   // ring of waiting tickets
static int s_head;                    // slot of the ticket that prints next
static int s_waiting;                 // how many slots are in use
static size_t s_image_bytes;          // image dots held right now
static bool s_printing;               // a ticket (or the test line) is on its way to the printer
static bool s_paused;                 // tickets are waiting because the printer is not ready

/* Copy a string into the block at *at, move *at past it, and return the copy. */
static const char *copy_text(char **at, const char *text)
{
    size_t n = strlen(text) + 1;
    char *copy = memcpy(*at, text, n);
    *at += n;
    return copy;
}

/* Make the queue's own copy of a job: one PSRAM block with the text and the image in it. */
static queued_job_t *copy_job(const printer_job_t *job)
{
    bool has_image = job->image && job->image_rows;
    size_t image_bytes = has_image ? (size_t)job->image_rows * PRINTER_WIDTH_BYTES : 0;
    size_t text_bytes = strlen(job->title) + strlen(job->body) + strlen(job->source) + 3;
    queued_job_t *q = heap_caps_malloc(sizeof(*q) + text_bytes + image_bytes, MALLOC_CAP_SPIRAM);
    if (!q) {
        return NULL;
    }
    char *at = (char *)(q + 1);
    q->job = *job;
    q->job.title = copy_text(&at, job->title);
    q->job.body = copy_text(&at, job->body);
    q->job.source = copy_text(&at, job->source);
    q->job.image = has_image ? memcpy(at, job->image, image_bytes) : NULL;
    q->job.image_rows = has_image ? job->image_rows : 0;
    q->image_bytes = image_bytes;
    return q;
}

static bool has_room(const printer_job_t *job)
{
    size_t image_bytes = job->image ? (size_t)job->image_rows * PRINTER_WIDTH_BYTES : 0;
    return s_waiting < QUEUE_MAX_WAITING && s_image_bytes + image_bytes <= QUEUE_MAX_IMAGE_BYTES;
}

esp_err_t print_queue_add(const printer_job_t *job, printer_ticket_t *ticket)
{
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    queued_job_t *q = NULL;
    if (!has_room(job)) {
        ESP_LOGW(TAG, "queue full (%d waiting, %u image bytes): ticket refused", s_waiting, (unsigned)s_image_bytes);
        err = ESP_ERR_INVALID_STATE;
    } else if ((q = copy_job(job)) == NULL) {
        err = ESP_ERR_NO_MEM;
    } else {
        // Number it now, while holding the lock: numbers follow the order in line.
        q->job.number = counter_increment();
        ticket->number = q->job.number;
        ticket->position = s_waiting + (s_printing ? 1 : 0);   // 0 = nothing ahead of it
        s_slots[(s_head + s_waiting) % QUEUE_MAX_WAITING] = q;
        s_waiting++;
        s_image_bytes += q->image_bytes;
        ESP_LOGI(TAG, "ticket #%04u joined the queue at position %d", (unsigned)ticket->number, ticket->position);
    }
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        xSemaphoreGive(s_wake);
    }
    return err;
}

/* Take the next ticket out of the line, or NULL if there is none (or the printer is claimed). */
static queued_job_t *take_next(void)
{
    queued_job_t *q = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_printing && s_waiting > 0) {
        q = s_slots[s_head];
        s_head = (s_head + 1) % QUEUE_MAX_WAITING;
        s_waiting--;
        s_printing = true;
    }
    xSemaphoreGive(s_lock);
    return q;
}

/* The ticket has printed: give its memory back. */
static void finish(queued_job_t *q)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_image_bytes -= q->image_bytes;
    s_printing = false;
    int left = s_waiting;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "ticket #%04u finished and freed, %d still waiting", (unsigned)q->job.number, left);
    free(q);
}

/* Note whether tickets are being held back, and log it when that changes. */
static void set_paused(bool not_ready)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool paused = not_ready && s_waiting > 0;
    bool changed = paused != s_paused;
    int waiting = s_waiting;
    s_paused = paused;
    xSemaphoreGive(s_lock);
    if (changed && paused) {
        ESP_LOGW(TAG, "queue paused: the printer is not ready, %d ticket(s) kept waiting", waiting);
    } else if (changed) {
        ESP_LOGI(TAG, "queue resumed");
    }
}

/* Sleep until a ticket arrives (or the printer is released), but no longer than ms. */
static void sleep_until_woken(uint32_t ms)
{
    xSemaphoreTake(s_wake, pdMS_TO_TICKS(ms));
}

static void queue_task(void *arg)
{
    for (;;) {
        // Asked before every ticket, and again straight after it (the loop comes back here).
        bool ready = s_ready ? s_ready() : true;
        set_paused(!ready);
        if (!ready) {
            sleep_until_woken(PAUSED_RECHECK_MS);
            continue;
        }
        queued_job_t *q = take_next();
        if (!q) {
            sleep_until_woken(IDLE_RECHECK_MS);
            continue;
        }
        ESP_LOGI(TAG, "ticket #%04u starts printing", (unsigned)q->job.number);
        if (s_print(&q->job) != ESP_OK) {
            ESP_LOGE(TAG, "ticket #%04u failed to print", (unsigned)q->job.number);
        }
        finish(q);
    }
}

void print_queue_get_info(int *waiting, bool *printing, bool *paused)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *waiting = s_waiting;
    *printing = s_printing;
    *paused = s_paused;
    xSemaphoreGive(s_lock);
}

bool print_queue_claim(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool idle = !s_printing && s_waiting == 0;
    if (idle) {
        s_printing = true;
    }
    xSemaphoreGive(s_lock);
    return idle;
}

void print_queue_release(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_printing = false;
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_wake);   // tickets may have joined the line meanwhile
}

esp_err_t print_queue_init(print_queue_print_fn print_fn, print_queue_ready_fn ready_fn)
{
    s_print = print_fn;
    s_ready = ready_fn;
    s_lock = xSemaphoreCreateMutex();
    s_wake = xSemaphoreCreateBinary();
    if (!s_lock || !s_wake) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(queue_task, "print_queue", QUEUE_TASK_STACK, NULL, QUEUE_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "print queue ready: up to %d waiting, %d KB of image data", QUEUE_MAX_WAITING,
             QUEUE_MAX_IMAGE_BYTES / 1024);
    return ESP_OK;
}
