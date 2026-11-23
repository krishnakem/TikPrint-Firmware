#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define CLOCK_TZ_MAX 64   // longest timezone name or rule we keep, including the ending 0

/** Where the current time came from. */
typedef enum {
    CLOCK_SOURCE_NONE = 0,   // not set since power-on
    CLOCK_SOURCE_BROWSER,    // a computer or phone with the web UI open
    CLOCK_SOURCE_INTERNET,   // an internet time server (SNTP), over Wi-Fi
} clock_source_t;

typedef struct {
    bool set;                      // false until the browser or the internet has set the time
    clock_source_t source;
    char now[32];                  // local time, e.g. "2026-11-20T22:05:31-05:00". "" if not set
    char timezone[CLOCK_TZ_MAX];   // IANA name, e.g. "America/Detroit". "UTC" until one is saved
    char zone[12];                 // short name of the zone right now, e.g. "EST" or "EDT". "" if not set
    int32_t last_sync_s_ago;       // seconds since the time was last set. -1 if not set
} clock_status_t;

/** What happened to a time sent by the browser. */
typedef enum {
    CLOCK_BROWSER_APPLIED = 0,   // the board's clock now follows it
    CLOCK_BROWSER_IGNORED,       // internet time is fresh, so that one was kept
    CLOCK_BROWSER_INVALID,       // not a believable time
} clock_browser_result_t;

/**
 * Apply the saved timezone and get ready to fetch internet time whenever Wi-Fi connects.
 * Call after NVS, esp_netif and the default event loop are up, and before Wi-Fi starts.
 */
esp_err_t clock_init(void);

/** Offer the browser's time (Unix time in milliseconds). Internet time wins while it is fresh. */
clock_browser_result_t clock_set_from_browser(int64_t unix_ms);

/**
 * Set the timezone and remember it across reboots.
 *   iana   e.g. "America/Detroit"          (only shown to people)
 *   posix  e.g. "EST5EDT,M3.2.0,M11.1.0"   (the rule the board follows)
 * Returns ESP_ERR_INVALID_ARG if either looks wrong.
 */
esp_err_t clock_set_timezone(const char *iana, const char *posix);

void clock_get_status(clock_status_t *out);

const char *clock_source_name(clock_source_t source);   // "browser", "internet" or "none"

/**
 * Write the local time into out using a strftime() pattern, e.g. "%b %d %H:%M".
 * Returns false (and writes "") if the clock is not set.
 */
bool clock_format_now(char *out, size_t size, const char *format);

/** A day as a number like 20261121 (local time), for "has this already happened today?" checks.
 *  unix_s is a time in seconds, as time(NULL) gives it. */
int32_t clock_day_number(int64_t unix_s);
