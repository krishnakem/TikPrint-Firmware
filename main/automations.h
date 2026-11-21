#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#include "weather.h"

/** How the last weather print went (for /api/status and the web UI). */
typedef struct {
    bool busy;              // a weather ticket is being fetched right now
    bool ran;               // one has been tried since the board started; the fields below are about it
    bool ok;                // it joined the print queue
    char error[16];         // "" if ok, else "no_zip", "no_wifi", "unreachable", "bad_answer", "queue_full"
    char message[96];       // the same, as a sentence
    uint32_t ticket;        // its ticket number, if ok
    int s_ago;              // seconds since it finished
    // Looking up the town of the ZIP code (separate from printing):
    bool locating;          // the board is looking it up, or will try again shortly
    char locate_error[16];  // "" = no problem so far, else why the last try failed ("no_wifi", "unreachable", "refused", ...)
    char locate_message[96];// the same, as a sentence
} automations_weather_status_t;

/** Start the background task that fetches and prints automations. Call after printer_init(). */
esp_err_t automations_start(void);

/**
 * Fetch the weather and put a ticket in the print queue, now. Returns at once; the work
 * happens in the background and the outcome shows up in automations_get_weather_status().
 * This is a manual print: it does not count towards the limit on scheduled prints.
 *   ESP_ERR_INVALID_STATE  the task is still busy with an earlier request
 */
esp_err_t automations_weather_print_now(weather_kind_t kind);

/** The weather ZIP code changed: look up its place name in the background, for the web UI. */
void automations_weather_zip_changed(void);

void automations_get_weather_status(automations_weather_status_t *out);
