/*
 * The clock: what time it is, and in which timezone.
 *
 * The board has no battery-backed clock, so it forgets the time at every power-off.
 * Two things can set it again:
 *   - the browser: the web UI sends the computer's time (works over USB, no internet needed)
 *   - the internet: once Wi-Fi has an IP address, a time server is asked (SNTP)
 * Internet time is the more trusted of the two. While it is fresh, browser time is ignored.
 *
 * - Only the timezone is saved (NVS namespace "clock", keys "iana" / "posix"). The time
 *   itself is never saved: after a reboot the clock counts as "not set" until one of the
 *   two sources sets it.
 * - Three tasks come through here: the web server (browser time, status), the network
 *   task (SNTP callback) and the printer (ticket time). Everything they share, and every
 *   use of the C library's timezone, is guarded by one lock (s_lock).
 */
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "clock.h"

static const char *TAG = "clock";

/* ---- Settings ---- */
#define SNTP_SERVER            "pool.ntp.org"
#define SNTP_RESYNC_MS         (60 * 60 * 1000)   // ask the time server again this often while on Wi-Fi
#define INTERNET_FRESH_S       (12 * 60 * 60)     // internet time outranks the browser for this long after a sync
#define BROWSER_DIFF_WARN_S    5                  // log it if the browser disagrees with internet time by more than this
#define EARLIEST_VALID_UNIX_S  1735689600LL       // 1 Jan 2025: anything earlier is not a real "now"
#define DEFAULT_TZ_IANA        "UTC"
#define DEFAULT_TZ_POSIX       "UTC0"
#define NVS_NS                 "clock"

static SemaphoreHandle_t s_lock;          // guards everything below, plus setenv/tzset/localtime
static clock_source_t s_source;           // who set the time last (NONE = not set since boot)
static int64_t s_last_sync_us;            // board uptime when the time was last set
static int64_t s_last_internet_us;        // board uptime at the last internet sync
static bool s_internet_synced;            // internet time has arrived at least once since boot
static char s_tz_iana[CLOCK_TZ_MAX] = DEFAULT_TZ_IANA;
static char s_tz_posix[CLOCK_TZ_MAX] = DEFAULT_TZ_POSIX;

static void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

/* ---------- Timezone ---------- */

/* True if text is 1..CLOCK_TZ_MAX-1 characters long and made only of letters, digits and `extra`. */
static bool made_of(const char *text, const char *extra)
{
    size_t n = text ? strlen(text) : 0;
    if (n == 0 || n >= CLOCK_TZ_MAX) {
        return false;
    }
    for (const char *p = text; *p; p++) {
        bool alnum = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9');
        if (!alnum && !strchr(extra, *p)) {
            return false;
        }
    }
    return true;
}

/* Hand the timezone rule to the C library. Call with the lock held. */
static void apply_timezone_locked(void)
{
    setenv("TZ", s_tz_posix, 1);
    tzset();
}

static void timezone_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;   // nothing saved yet: stay on the default
    }
    char iana[CLOCK_TZ_MAX], posix[CLOCK_TZ_MAX];
    size_t ni = sizeof(iana), np = sizeof(posix);
    if (nvs_get_str(h, "iana", iana, &ni) == ESP_OK && nvs_get_str(h, "posix", posix, &np) == ESP_OK) {
        strlcpy(s_tz_iana, iana, sizeof(s_tz_iana));
        strlcpy(s_tz_posix, posix, sizeof(s_tz_posix));
    }
    nvs_close(h);
}

static void timezone_save(const char *iana, const char *posix)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "could not save the timezone");
        return;
    }
    nvs_set_str(h, "iana", iana);
    nvs_set_str(h, "posix", posix);
    nvs_commit(h);
    nvs_close(h);
}

esp_err_t clock_set_timezone(const char *iana, const char *posix)
{
    if (!made_of(iana, "_+-/") || !made_of(posix, "<>+-,./:")) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    bool changed = strcmp(iana, s_tz_iana) != 0 || strcmp(posix, s_tz_posix) != 0;
    if (changed) {
        strlcpy(s_tz_iana, iana, sizeof(s_tz_iana));
        strlcpy(s_tz_posix, posix, sizeof(s_tz_posix));
        apply_timezone_locked();
    }
    unlock();
    if (changed) {
        // Flash is only written when the timezone really changes (the page re-sends it every few minutes).
        timezone_save(iana, posix);
        ESP_LOGI(TAG, "timezone is now %s (%s)", iana, posix);
    }
    return ESP_OK;
}

/* ---------- Setting the time ---------- */

/* Internet time arrived less than INTERNET_FRESH_S ago. Call with the lock held. */
static bool internet_is_fresh_locked(void)
{
    return s_internet_synced && esp_timer_get_time() - s_last_internet_us < INTERNET_FRESH_S * 1000000LL;
}

clock_browser_result_t clock_set_from_browser(int64_t unix_ms)
{
    if (unix_ms / 1000 < EARLIEST_VALID_UNIX_S) {
        return CLOCK_BROWSER_INVALID;
    }
    clock_browser_result_t result = CLOCK_BROWSER_APPLIED;
    long long diff_s = 0;
    lock();
    if (internet_is_fresh_locked()) {
        struct timeval now;
        gettimeofday(&now, NULL);
        diff_s = (long long)(unix_ms / 1000) - (long long)now.tv_sec;
        result = CLOCK_BROWSER_IGNORED;
    } else {
        struct timeval tv = { .tv_sec = unix_ms / 1000, .tv_usec = (unix_ms % 1000) * 1000 };
        settimeofday(&tv, NULL);
        bool first = s_source != CLOCK_SOURCE_BROWSER;
        s_source = CLOCK_SOURCE_BROWSER;
        s_last_sync_us = esp_timer_get_time();
        if (first) {
            ESP_LOGI(TAG, "time set from the browser");
        }
    }
    unlock();
    if (result == CLOCK_BROWSER_IGNORED && llabs(diff_s) > BROWSER_DIFF_WARN_S) {
        ESP_LOGW(TAG, "browser time is %lld s away from internet time: keeping internet time", diff_s);
    }
    return result;
}

/* Called on the network task each time the time server answers. By then the system clock
 * has already been set to *tv. */
static void on_internet_time(struct timeval *tv)
{
    int64_t arrived_us = esp_timer_get_time();
    lock();
    // A browser time may have slipped in while we waited for the lock: put the internet
    // time back, moved forward by however long the wait was.
    int64_t late_us = esp_timer_get_time() - arrived_us;
    int64_t usec = (int64_t)tv->tv_usec + late_us;
    struct timeval fixed = { .tv_sec = tv->tv_sec + usec / 1000000, .tv_usec = usec % 1000000 };
    settimeofday(&fixed, NULL);
    bool first = !s_internet_synced;
    s_source = CLOCK_SOURCE_INTERNET;
    s_internet_synced = true;
    s_last_internet_us = s_last_sync_us = esp_timer_get_time();
    unlock();
    if (first) {
        ESP_LOGI(TAG, "time set from the internet (%s)", SNTP_SERVER);
    }
}

/* Wi-Fi got an IP address: (re)start SNTP, which asks the time server straight away
 * and then again every SNTP_RESYNC_MS. */
static void on_wifi_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_err_t err = esp_netif_sntp_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not start SNTP: %s", esp_err_to_name(err));
    }
}

/* ---------- Reading the time ---------- */

const char *clock_source_name(clock_source_t source)
{
    return source == CLOCK_SOURCE_BROWSER ? "browser" : source == CLOCK_SOURCE_INTERNET ? "internet" : "none";
}

/* Local time as "2026-11-20T22:05:31-05:00". Call with the lock held. */
static void format_iso_locked(char *out, size_t size)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char offset[8] = "";                                    // strftime gives "-0500"; ISO wants "-05:00"
    size_t n = strftime(out, size, "%Y-%m-%dT%H:%M:%S", &tm);
    if (strftime(offset, sizeof(offset), "%z", &tm) == 5 && n + 7 <= size) {
        snprintf(out + n, size - n, "%.3s:%s", offset, offset + 3);
    }
}

void clock_get_status(clock_status_t *out)
{
    memset(out, 0, sizeof(*out));
    lock();
    out->set = s_source != CLOCK_SOURCE_NONE;
    out->source = s_source;
    strlcpy(out->timezone, s_tz_iana, sizeof(out->timezone));
    out->last_sync_s_ago = -1;
    if (out->set) {
        format_iso_locked(out->now, sizeof(out->now));
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(out->zone, sizeof(out->zone), "%Z", &tm);
        out->last_sync_s_ago = (int32_t)((esp_timer_get_time() - s_last_sync_us) / 1000000);
    }
    unlock();
}

bool clock_format_now(char *out, size_t size, const char *format)
{
    out[0] = 0;
    lock();
    bool set = s_source != CLOCK_SOURCE_NONE;
    if (set) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        if (strftime(out, size, format, &tm) == 0) {
            out[0] = 0;
        }
    }
    unlock();
    return set && out[0];
}

/* ---------- Start ---------- */

esp_err_t clock_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    timezone_load();
    lock();
    apply_timezone_locked();
    unlock();

    // Prepare SNTP but don't start it: there is no network yet. on_wifi_got_ip starts it.
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(SNTP_SERVER);
    sntp.start = false;
    sntp.wait_for_sync = false;
    sntp.sync_cb = on_internet_time;
    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&sntp), TAG, "sntp init");
    sntp_set_sync_interval(SNTP_RESYNC_MS);
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_got_ip, NULL),
                        TAG, "wifi event");

    ESP_LOGI(TAG, "timezone %s (%s); the time is not set until a browser or the internet sets it",
             s_tz_iana, s_tz_posix);
    return ESP_OK;
}

int32_t clock_day_number(int64_t unix_s)
{
    time_t t = (time_t)unix_s;
    struct tm tm;
    lock();
    localtime_r(&t, &tm);
    unlock();
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}
