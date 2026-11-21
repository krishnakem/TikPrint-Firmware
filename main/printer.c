/*
 * Printer interface. The rest of the firmware only calls the functions here,
 * so swapping between the thermal printer and the fake one is a one-line change.
 */
#include "esp_log.h"

#include "printer.h"

static const char *TAG = "printer";

// To run without a printer attached, point this at printer_fake_driver instead.
static const printer_driver_t *s_driver = &printer_thermal_driver;

esp_err_t printer_init(void)
{
    ESP_LOGI(TAG, "using printer driver: %s", s_driver->name);
    return s_driver->init ? s_driver->init() : ESP_OK;
}

esp_err_t printer_print_job(const printer_job_t *job)
{
    return s_driver->print(job);
}

const printer_job_t *printer_last_job(void)
{
    return s_driver->last_job ? s_driver->last_job() : NULL;
}

const char *printer_name(void)
{
    return s_driver->name;
}
