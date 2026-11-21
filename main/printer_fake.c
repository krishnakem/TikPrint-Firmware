/*
 * Fake printer: stands in until the thermal printer mechanism arrives.
 *
 * - "Prints" by writing the ticket to the UART log, with an ASCII-art preview
 *   of any image.
 * - Keeps a copy of the last job in PSRAM so GET /api/last-ticket can return it.
 *   The print queue calls print() on its own task while the web server may be
 *   reading that copy, so the copy is guarded by a lock (s_last_lock).
 *
 * The thermal driver also calls print() here for every ticket, to get the log
 * and the "last ticket" copy.
 */
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "printer.h"

static const char *TAG = "fake_printer";

/* ASCII preview: each character covers a block of CELL_W x CELL_H dots. */
#define CELL_W 8
#define CELL_H 16
#define PREVIEW_COLS (PRINTER_WIDTH_DOTS / CELL_W)   // 72 characters wide
#define PREVIEW_MAX_LINES 48

static printer_job_t s_last;     // pointers below are owned by this file
static char *s_title, *s_body, *s_source;
static uint8_t *s_image;
static bool s_have_last;
static SemaphoreHandle_t s_last_lock;   // held while the last job is being read or replaced

static void *psram_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
}

static char *psram_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = psram_alloc(n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

static void log_text_lines(const char *text)
{
    // Log each line of a multi-line string on its own log line.
    while (*text) {
        const char *nl = strchr(text, '\n');
        int len = nl ? (int)(nl - text) : (int)strlen(text);
        ESP_LOGI(TAG, "| %.*s", len, text);
        text += len + (nl ? 1 : 0);
    }
}

static void log_image_preview(const uint8_t *img, uint16_t rows)
{
    static const char shades[] = " .:-=+*#%@";   // light -> dark
    char line[PREVIEW_COLS + 1];
    int lines = (rows + CELL_H - 1) / CELL_H;
    int shown = lines > PREVIEW_MAX_LINES ? PREVIEW_MAX_LINES : lines;

    ESP_LOGI(TAG, "| [image %d x %d dots]", PRINTER_WIDTH_DOTS, rows);
    for (int cy = 0; cy < shown; cy++) {
        for (int cx = 0; cx < PREVIEW_COLS; cx++) {
            int black = 0, total = 0;
            for (int y = cy * CELL_H; y < (cy + 1) * CELL_H && y < rows; y++) {
                // CELL_W is 8, so each cell is exactly one byte per row
                black += __builtin_popcount(img[y * PRINTER_WIDTH_BYTES + cx]);
                total += 8;
            }
            line[cx] = shades[total ? (black * 9 + total / 2) / total : 0];
        }
        line[PREVIEW_COLS] = 0;
        ESP_LOGI(TAG, "|%s|", line);
    }
    if (shown < lines) {
        ESP_LOGI(TAG, "| ... %d more preview lines not shown", lines - shown);
    }
}

/* The lock only exists after init(). Without it (the driver used on its own, from one task) there is nothing to guard. */
static void fake_last_job_lock(void)
{
    if (s_last_lock) {
        xSemaphoreTake(s_last_lock, portMAX_DELAY);
    }
}

static void fake_last_job_unlock(void)
{
    if (s_last_lock) {
        xSemaphoreGive(s_last_lock);
    }
}

static void remember(const printer_job_t *job)
{
    fake_last_job_lock();   // waits here if the web server is sending the old copy to a browser
    free(s_title);
    free(s_body);
    free(s_source);
    free(s_image);
    s_title = psram_strdup(job->title);
    s_body = psram_strdup(job->body);
    s_source = psram_strdup(job->source);
    s_image = NULL;
    uint16_t rows = 0;
    if (job->image && job->image_rows) {
        size_t n = (size_t)job->image_rows * PRINTER_WIDTH_BYTES;
        s_image = psram_alloc(n);
        if (s_image) {
            memcpy(s_image, job->image, n);
            rows = job->image_rows;
        }
    }
    s_last = (printer_job_t) {
        .number = job->number,
        .title = s_title ? s_title : "",
        .body = s_body ? s_body : "",
        .source = s_source ? s_source : "",
        .image = s_image,
        .image_rows = rows,
        .text_in_image = s_image && job->text_in_image,
        .image_first = s_image && job->image_first,
    };
    s_have_last = true;
    fake_last_job_unlock();
}

static esp_err_t fake_init(void)
{
    if (!s_last_lock) {
        s_last_lock = xSemaphoreCreateMutex();
    }
    return s_last_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t fake_print(const printer_job_t *job)
{
    ESP_LOGI(TAG, "+------------------------ TICKET #%04u ------------------------", (unsigned)job->number);
    if (job->source[0]) {
        ESP_LOGI(TAG, "| (%s)", job->source);
    }
    if (job->title[0]) {
        ESP_LOGI(TAG, "| TITLE: %s", job->title);
    }
    if (job->body[0]) {
        log_text_lines(job->body);
    }
    if (job->image && job->image_rows) {
        log_image_preview(job->image, job->image_rows);
    }
    ESP_LOGI(TAG, "+--------------------------- cut here --------------------------");

    remember(job);
    return ESP_OK;
}

static const printer_job_t *fake_last_job(void)
{
    return s_have_last ? &s_last : NULL;
}

const printer_driver_t printer_fake_driver = {
    .name = "fake (prints to the UART log)",
    .init = fake_init,
    .print = fake_print,
    .last_job = fake_last_job,
    .last_job_lock = fake_last_job_lock,
    .last_job_unlock = fake_last_job_unlock,
};
