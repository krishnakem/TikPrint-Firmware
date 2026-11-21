#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define PIN_LEN 6   // a PIN is exactly this many digits

typedef enum {
    PIN_OK,       // right PIN, or no PIN is set
    PIN_WRONG,    // wrong PIN: one try used up
    PIN_LOCKED,   // too many wrong tries: nothing is accepted for a while
} pin_result_t;

/** Load the saved PIN, if there is one. Call once NVS is up. */
esp_err_t pin_init(void);

/** True if a PIN is set, which means firmware updates ask for it. */
bool pin_is_set(void);

/** True if text is PIN_LEN digits and nothing else. */
bool pin_format_ok(const char *text);

/**
 * Compare a PIN with the saved one.
 * tries_left: wrong tries still allowed before the lock (only meaningful for PIN_WRONG).
 * retry_s:    seconds until the lock ends (only meaningful for PIN_LOCKED).
 * Either pointer may be NULL.
 */
pin_result_t pin_check(const char *pin, int *tries_left, int *retry_s);

/** Save a new PIN. ESP_ERR_INVALID_ARG if it is not PIN_LEN digits. */
esp_err_t pin_set(const char *pin);

/** Remove the PIN: updates no longer ask for one. */
esp_err_t pin_clear(void);
