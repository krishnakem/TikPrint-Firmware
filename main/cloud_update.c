/*
 * The "update" command of TikPrint Cloud (see cloud_update.h).
 *
 * One update at a time. Its life:
 *
 *   command arrives     waiting to start (a few seconds at most, if the updater is busy)
 *   "downloading"       fw_github.c is fetching the release
 *   "installing"        the file is complete and being checked
 *   "rebooting"         saved to flash, then the board restarts
 *   ... restart ...
 *   "done"              the new firmware runs and has passed its trial (fw_update.c)
 *   "rolled_back"       it did not: the old firmware is running again
 *   "failed"            given up before the restart; nothing was changed
 *
 * Each step is a line to report to the cloud. The Realtime task picks it up with
 * cloud_update_next_report() and writes it to the printer's row.
 */
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "cloud_jobs.h"
#include "cloud_realtime.h"
#include "cloud_update.h"
#include "fw_github.h"
#include "fw_update.h"

static const char *TAG = "cloud_update";

#define NVS_NS       "cloud"
#define KEY_UPDATE   "update"
#define VERSION      1
#define START_WITHIN_S   60     // a command that cannot start within this long (the updater stays busy) is given up on
#define TELL_INSTALLING_MS 4000 // the install waits this long, at most, for "installing" to reach the cloud
#define TELL_REBOOTING_MS  6000 // ... and the restart this long for "rebooting"
#define SETTLED_AFTER_S  20     // after a restart, how the update ended is only looked at once the board has run this
                                // long: by then fw_update.c has started, and new firmware has passed its trial (15 s)

typedef enum {
    STAGE_NONE,
    STAGE_DOWNLOADING,
    STAGE_INSTALLING,
    STAGE_REBOOTING,
    STAGE_DONE,
    STAGE_FAILED,
    STAGE_ROLLED_BACK,
} stage_t;

/* The update in hand, or the last one that ended. This is the block saved to flash. */
typedef struct {
    uint8_t version;
    uint8_t stage;                        // a stage_t
    uint8_t reported;                     // the cloud has been told about this stage
    char id[CLOUD_JOB_ID_MAX];            // the command's id
    char ver[32];                         // the version it asks for, e.g. "0.4.2"
    char error[CLOUD_UPDATE_ERROR_MAX];   // why it failed, "" if it did not
} update_t;

static SemaphoreHandle_t s_lock;          // guards everything below
static update_t s_update;
static uint32_t s_report;                 // goes up with every new stage: which one a report is about
static bool s_after_restart;              // s_update was "rebooting" when the board started: see how it ended
static struct {
    char id[CLOUD_JOB_ID_MAX];            // a command waiting to start, "" = none
    char ver[32];
    int64_t until_us;
} s_waiting;

static const char *stage_name(stage_t stage)
{
    switch (stage) {
    case STAGE_DOWNLOADING: return "downloading";
    case STAGE_INSTALLING:  return "installing";
    case STAGE_REBOOTING:   return "rebooting";
    case STAGE_DONE:        return "done";
    case STAGE_FAILED:      return "failed";
    case STAGE_ROLLED_BACK: return "rolled_back";
    default:                return "idle";
    }
}

static bool in_progress(stage_t stage)
{
    return stage == STAGE_DOWNLOADING || stage == STAGE_INSTALLING || stage == STAGE_REBOOTING;
}

/* Call with s_lock held. */
static void save_locked(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "could not save the update's progress");
        return;
    }
    nvs_set_blob(h, KEY_UPDATE, &s_update, sizeof(s_update));
    nvs_commit(h);
    nvs_close(h);
}

/* Move to a new stage, to be reported. Call with s_lock held. */
static void set_stage_locked(stage_t stage, const char *error, bool save)
{
    s_update.stage = (uint8_t)stage;
    s_update.reported = 0;
    strlcpy(s_update.error, error ? error : "", sizeof(s_update.error));
    s_report++;
    if (save) {
        save_locked();
    }
    ESP_LOGI(TAG, "update to v%s: %s%s%s", s_update.ver, stage_name(stage), s_update.error[0] ? ": " : "", s_update.error);
}

/* Take a command as the update in hand, at this stage. Call with s_lock held. */
static void begin_locked(const char *id, const char *ver, stage_t stage, const char *error)
{
    memset(&s_update, 0, sizeof(s_update));
    s_update.version = VERSION;
    strlcpy(s_update.id, id, sizeof(s_update.id));
    strlcpy(s_update.ver, ver, sizeof(s_update.ver));
    s_after_restart = false;
    set_stage_locked(stage, error, true);
}

/* The updater (fw_github.c) says how the install is going. Runs on its task, and holds it up for a
 * few seconds at most, so that "installing" and "rebooting" reach the cloud before the restart. */
static void on_install_step(fw_github_step_t step, const char *error)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // Only an install that a cloud command started is reported. One started on the printer's own
    // page is none of the cloud's business.
    bool ours = in_progress((stage_t)s_update.stage) && !s_after_restart;
    int wait_ms = 0;
    if (ours) {
        switch (step) {
        case FW_GITHUB_STEP_INSTALLING:
            set_stage_locked(STAGE_INSTALLING, NULL, false);
            wait_ms = TELL_INSTALLING_MS;
            break;
        case FW_GITHUB_STEP_RESTARTING:
            set_stage_locked(STAGE_REBOOTING, NULL, true);   // saved: this is what the board finds after the restart
            wait_ms = TELL_REBOOTING_MS;
            break;
        default:
            set_stage_locked(STAGE_FAILED, error, true);
            break;
        }
    }
    uint32_t report = s_report;
    xSemaphoreGive(s_lock);
    if (!ours) {
        return;
    }
    cloud_realtime_wake();   // report it now
    for (int waited = 0; waited < wait_ms; waited += 100) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool told = s_update.reported || s_report != report;
        xSemaphoreGive(s_lock);
        if (told) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void cloud_update_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock);
    nvs_handle_t h;
    size_t n = sizeof(s_update);
    bool loaded = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        loaded = nvs_get_blob(h, KEY_UPDATE, &s_update, &n) == ESP_OK && n == sizeof(s_update) && s_update.version == VERSION;
        nvs_close(h);
    }
    if (!loaded) {
        memset(&s_update, 0, sizeof(s_update));
    }
    s_update.id[sizeof(s_update.id) - 1] = 0;   // whatever was saved, every text ends
    s_update.ver[sizeof(s_update.ver) - 1] = 0;
    s_update.error[sizeof(s_update.error) - 1] = 0;
    if (s_update.stage == STAGE_REBOOTING) {
        s_after_restart = true;   // cloud_update_tick sees how it ended, once the trial is over
    } else if (in_progress((stage_t)s_update.stage)) {
        // The board restarted (power cut, a crash) while the file was still coming in.
        set_stage_locked(STAGE_FAILED, "The printer restarted before the update was installed", true);
    }
    fw_github_set_listener(on_install_step);
}

/* A command's id is letters, digits and dashes (the same rule as a job's). */
static bool id_is_plain(const char *id)
{
    size_t n = id ? strlen(id) : 0;
    if (n < 8 || n >= CLOUD_JOB_ID_MAX) {
        return false;
    }
    for (const char *p = id; *p; p++) {
        bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

void cloud_update_command(const char *id, const char *version)
{
    if (!id_is_plain(id) || !version) {
        return;
    }
    if (*version == 'v' || *version == 'V') {
        version++;
    }
    bool taken = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool seen_before = s_update.stage != STAGE_NONE && strcmp(s_update.id, id) == 0;
    // Seen before (it is in hand, or it has ended and the cloud has yet to close it), or another
    // one is installing or waiting to start: ignore it.
    if (!seen_before && !in_progress((stage_t)s_update.stage) && !s_waiting.id[0]) {
        strlcpy(s_waiting.id, id, sizeof(s_waiting.id));
        strlcpy(s_waiting.ver, version, sizeof(s_waiting.ver));
        // A version too long to keep cannot be a real one: it is refused when it is looked at.
        if (strlen(version) >= sizeof(s_waiting.ver)) {
            s_waiting.ver[0] = 0;
        }
        s_waiting.until_us = esp_timer_get_time() + (int64_t)START_WITHIN_S * 1000000;
        taken = true;
    }
    xSemaphoreGive(s_lock);
    if (taken) {
        ESP_LOGI(TAG, "the cloud asks for an update to v%s", version);
        cloud_realtime_wake();
    }
}

/* Start the command that is waiting. Call with s_lock held. */
static void start_waiting_locked(void)
{
    const char *running = esp_app_get_description()->version;
    const char *id = s_waiting.id, *ver = s_waiting.ver;
    if (strcmp(ver, running) == 0) {
        begin_locked(id, ver, STAGE_DONE, NULL);   // this version is running already: nothing to install
    } else {
        esp_err_t err = fw_github_install_version(ver);
        if (err == ESP_OK) {
            begin_locked(id, ver, STAGE_DOWNLOADING, NULL);
        } else if (err == ESP_ERR_INVALID_STATE && esp_timer_get_time() < s_waiting.until_us) {
            return;   // the updater is busy (its daily look, most likely): try again at the next tick
        } else {
            begin_locked(id, ver, STAGE_FAILED,
                         err == ESP_ERR_INVALID_ARG ? "Not a version number"
                         : err == ESP_ERR_INVALID_VERSION ? "Already newer"
                         : "The printer is busy with another update");
        }
    }
    s_waiting.id[0] = 0;
}

void cloud_update_tick(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_after_restart && esp_timer_get_time() > (int64_t)SETTLED_AFTER_S * 1000000 && !fw_update_on_trial()) {
        // The board restarted into the new firmware. If that firmware had not held up, the
        // bootloader would have gone back to the old one, and this would be the old one talking.
        s_after_restart = false;
        if (strcmp(s_update.ver, esp_app_get_description()->version) == 0) {
            set_stage_locked(STAGE_DONE, NULL, true);
        } else {
            set_stage_locked(STAGE_ROLLED_BACK, "The new firmware did not start properly. The old one is running again", true);
        }
    }
    if (s_waiting.id[0]) {
        start_waiting_locked();
    }
    xSemaphoreGive(s_lock);
}

bool cloud_update_next_report(char status[CLOUD_UPDATE_STATUS_MAX], char error[CLOUD_UPDATE_ERROR_MAX], uint32_t *report)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // While the new firmware is still on trial there is nothing to say yet: "rebooting" was said before the restart.
    bool news = s_update.stage != STAGE_NONE && !s_update.reported && !s_after_restart;
    if (news) {
        strlcpy(status, stage_name((stage_t)s_update.stage), CLOUD_UPDATE_STATUS_MAX);
        strlcpy(error, s_update.error, CLOUD_UPDATE_ERROR_MAX);
        *report = s_report;
    }
    xSemaphoreGive(s_lock);
    return news;
}

void cloud_update_reported(uint32_t report)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (report == s_report && !s_update.reported) {
        s_update.reported = 1;
        // How it ended is kept, so that it is not said twice and the command is not taken twice.
        if (!in_progress((stage_t)s_update.stage)) {
            save_locked();
        }
    }
    xSemaphoreGive(s_lock);
}

void cloud_update_clear(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_update, 0, sizeof(s_update));
    memset(&s_waiting, 0, sizeof(s_waiting));
    s_after_restart = false;
    s_report++;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, KEY_UPDATE);   // "there was none" is fine too
        nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(s_lock);
}
