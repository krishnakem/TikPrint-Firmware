/*
 * Printer interface. The rest of the firmware only calls the functions here,
 * so swapping between the thermal printer and the fake one is a one-line change.
 *
 * Tickets do not go straight to the driver: they join the print queue
 * (print_queue.c), which hands them to the driver one at a time.
 */
#include "esp_log.h"
#include "esp_check.h"

#include "printer.h"
#include "print_queue.h"

static const char *TAG = "printer";

// To run without a printer attached, point this at printer_fake_driver instead.
static const printer_driver_t *s_driver = &printer_thermal_driver;

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
    return print_queue_init(print_now);
}

esp_err_t printer_print_job(const printer_job_t *job, printer_ticket_t *ticket)
{
    return print_queue_add(job, ticket);
}

void printer_queue_info(int *waiting, bool *printing)
{
    print_queue_get_info(waiting, printing);
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
