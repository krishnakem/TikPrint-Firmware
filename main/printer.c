/*
 * Printer interface. The rest of the firmware only calls the functions here,
 * so swapping between the thermal printer and the fake one is a one-line change.
 *
 * Tickets do not go straight to the driver: they join the print queue
 * (print_queue.c), which hands them to the driver one at a time.
 *
 * The printer's state (paper, cover, ...) is asked for by the queue's task and by
 * the diagnostic endpoint, and read by /api/status. The last answer is kept here,
 * guarded by s_status_mux because those run on different tasks.
 */
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "printer.h"
#include "print_queue.h"

static const char *TAG = "printer";

// To run without a printer attached, point this at printer_fake_driver instead.
static const printer_driver_t *s_driver = &printer_thermal_driver;

#define STATUS_WAIT_FOREVER_MS UINT32_MAX   // the queue's task can wait for the serial line as long as it takes
#define DIAG_WAIT_MS            300          // the web server cannot: it answers "busy" instead

static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
static printer_status_t s_status = PRINTER_STATUS_UNKNOWN;
static int64_t s_status_checked_us = -1;   // board uptime at the last check, -1 = never

const char *printer_status_name(printer_status_t status)
{
    switch (status) {
    case PRINTER_STATUS_OK:           return "ok";
    case PRINTER_STATUS_PAPER_LOW:    return "paper_low";
    case PRINTER_STATUS_OUT_OF_PAPER: return "out_of_paper";
    case PRINTER_STATUS_COVER_OPEN:   return "cover_open";
    case PRINTER_STATUS_OFFLINE:      return "offline";
    case PRINTER_STATUS_ERROR:        return "error";
    default:                          return "unknown";
    }
}

/* Keep the result of a check for /api/status, and log it when it is news. */
static void remember_status(printer_status_t status)
{
    taskENTER_CRITICAL(&s_status_mux);
    printer_status_t before = s_status;
    s_status = status;
    s_status_checked_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_status_mux);
    if (status != before) {
        ESP_LOGI(TAG, "printer status: %s (was %s)", printer_status_name(status), printer_status_name(before));
    }
}

printer_status_t printer_get_status(int *checked_s_ago)
{
    taskENTER_CRITICAL(&s_status_mux);
    printer_status_t status = s_status;
    int64_t checked_us = s_status_checked_us;
    taskEXIT_CRITICAL(&s_status_mux);
    *checked_s_ago = checked_us < 0 ? -1 : (int)((esp_timer_get_time() - checked_us) / 1000000);
    return status;
}

/* Ask the driver, if it can be asked. */
static esp_err_t read_status(printer_status_report_t *out, uint32_t wait_ms)
{
    if (!s_driver->read_status) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t err = s_driver->read_status(out, wait_ms);
    if (err == ESP_OK) {
        remember_status(out->status);
    }
    return err;
}

esp_err_t printer_read_status(printer_status_report_t *out)
{
    return read_status(out, DIAG_WAIT_MS);
}

/*
 * Called by the print queue, on its own task: may the next ticket be sent?
 * Only two answers hold tickets back: out of paper and cover open. Anything else prints,
 * above all "unknown" (no status wire, printer silent, fake printer): then the board
 * simply sends the ticket, as it did before it could ask.
 */
static bool printer_is_ready(void)
{
    printer_status_report_t report;
    if (read_status(&report, STATUS_WAIT_FOREVER_MS) != ESP_OK) {
        return true;
    }
    return report.status != PRINTER_STATUS_OUT_OF_PAPER && report.status != PRINTER_STATUS_COVER_OPEN;
}

/* Called by the print queue, on its own task, for each ticket in turn. */
static esp_err_t print_now(const printer_job_t *job)
{
    return s_driver->print(job);
}

esp_err_t printer_init(void)
{
    ESP_LOGI(TAG, "using printer driver: %s", s_driver->name);
    if (s_driver->init) {
        ESP_RETURN_ON_ERROR(s_driver->init(), TAG, "driver init");
    }
    return print_queue_init(print_now, printer_is_ready);
}

esp_err_t printer_print_job(const printer_job_t *job, printer_ticket_t *ticket)
{
    return print_queue_add(job, ticket);
}

void printer_queue_info(int *waiting, bool *printing, bool *paused)
{
    print_queue_get_info(waiting, printing, paused);
}

const printer_job_t *printer_last_job_lock(void)
{
    if (!s_driver->last_job) {
        return NULL;
    }
    if (s_driver->last_job_lock) {
        s_driver->last_job_lock();
    }
    return s_driver->last_job();
}

void printer_last_job_unlock(void)
{
    if (s_driver->last_job && s_driver->last_job_unlock) {
        s_driver->last_job_unlock();
    }
}

const char *printer_name(void)
{
    return s_driver->name;
}

esp_err_t printer_test(int baud, int pin, int invert)
{
    if (s_driver != &printer_thermal_driver) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!print_queue_claim()) {
        return ESP_ERR_INVALID_STATE;   // a ticket is printing or waiting
    }
    esp_err_t err = printer_thermal_test(baud, pin, invert);
    print_queue_release();
    return err;
}

esp_err_t printer_pace_test(const printer_pace_test_t *test)
{
    if (s_driver != &printer_thermal_driver) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!print_queue_claim()) {
        return ESP_ERR_INVALID_STATE;   // a ticket is printing or waiting
    }
    esp_err_t err = printer_thermal_pace_test(test);
    print_queue_release();
    return err;
}
