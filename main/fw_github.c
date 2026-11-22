/*
 * Firmware updates from GitHub.
 *
 * The board asks GitHub for the latest release of FW_GITHUB_REPO and compares its version
 * with its own. If the release is newer, it can download the firmware file attached to that
 * release straight into the spare firmware slot and restart into it. From there on it is
 * the same as any update: the file is checked, and the new firmware is on trial for a few
 * seconds (see fw_update.c).
 *
 *   check    GET https://api.github.com/repos/<repo>/releases/latest   (a small JSON answer)
 *   install  GET https://github.com/<repo>/releases/download/v0.2.0/tikprint_firmware.bin
 *
 * Beta builds: with "Beta updates" switched on, the check asks for the newest few releases
 * instead (GET .../releases), which also lists the pre-releases, and takes the highest version
 * among them. A beta is numbered after the release it leads up to: 0.4.0-beta.1, 0.4.0-beta.2, ...
 * are all older than 0.4.0 itself, so the finished release replaces them like any other update.
 * Switching the betas off again does not go back to an older version: the board keeps the beta
 * it has until a finished release overtakes it.
 *
 * - Both go over HTTPS and GitHub's certificate is verified, so the file really comes from
 *   that repository. Whoever can publish a release there decides what the printer runs.
 * - It only ever moves to a NEWER version, and only installs when asked to.
 * - It checks by itself once a day (AUTO_CHECK_S) so the page can show that an update is waiting.
 * - Needs Wi-Fi with internet. Over the USB cable alone there is no way to reach GitHub.
 *
 * All of this runs on its own task, because a download takes about a minute and the web
 * server must keep answering meanwhile. The shared status is guarded by s_lock.
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "fw_github.h"
#include "fw_update.h"
#include "wifi_mgr.h"
#include "printer.h"
#include "settings.h"

static const char *TAG = "fw_github";

#define LATEST_URL       "https://api.github.com/repos/" FW_GITHUB_REPO "/releases/latest"
#define RECENT_URL       "https://api.github.com/repos/" FW_GITHUB_REPO "/releases?per_page=5"   // newest first, betas included
#define DOWNLOAD_URL_FMT "https://github.com/" FW_GITHUB_REPO "/releases/download/%s/" FW_GITHUB_ASSET
#define USER_AGENT       "TikPrint-Firmware"   // GitHub refuses requests that do not say who is asking

#define FIRST_CHECK_S    30              // first check this long after boot (once Wi-Fi is up)
#define AUTO_CHECK_S     (24 * 60 * 60)  // then once a day
#define AUTO_RETRY_S     (60 * 60)       // after a check that failed, try again in an hour
#define HTTP_TIMEOUT_MS  15000
#define RELEASE_JSON_MAX (96 * 1024)     // one release is a few KB, a list of five a few times that; anything bigger is not what we asked for
#define NOT_A_BETA       1000000         // where a finished release sorts among its betas: after all of them
#define WAIT_FOR_PRINTS_S 30             // before restarting, let tickets that are printing finish (at most this long)
#define TASK_STACK       12288           // HTTPS needs a deep stack

typedef enum { CMD_NONE, CMD_CHECK, CMD_INSTALL } command_t;

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static fw_github_status_t s_status;
static command_t s_command;
static char s_tag[40];             // the release's tag exactly as GitHub gave it, e.g. "v0.2.0" or "v0.4.0-beta.7"
static int64_t s_checked_at_us;    // when GitHub last answered
static int64_t s_next_auto_us;     // when to check next without being asked

const char *fw_github_state_name(fw_github_state_t state)
{
    switch (state) {
    case FW_GITHUB_CHECKING:    return "checking";
    case FW_GITHUB_DOWNLOADING: return "downloading";
    case FW_GITHUB_RESTARTING:  return "restarting";
    default:                    return "idle";
    }
}

void fw_github_get_status(fw_github_status_t *out)
{
    bool beta = settings_get_beta_updates();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    out->checked_s_ago = s_status.checked ? (int)((esp_timer_get_time() - s_checked_at_us) / 1000000) : 0;
    out->beta = beta;
    xSemaphoreGive(s_lock);
}

static void set_state(fw_github_state_t state)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = state;
    xSemaphoreGive(s_lock);
}

static void set_error(const char *message)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_status.error, message, sizeof(s_status.error));
    xSemaphoreGive(s_lock);
    if (message[0]) {
        ESP_LOGW(TAG, "%s", message);
    }
}

static esp_err_t ask(command_t command)
{
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.state != FW_GITHUB_IDLE || s_command != CMD_NONE) {
        err = ESP_ERR_INVALID_STATE;
    } else if (command == CMD_INSTALL && !s_status.available) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        s_command = command;
        s_status.error[0] = 0;
        // Show the new state at once, so the page never sees "idle" between asking and starting.
        s_status.state = command == CMD_CHECK ? FW_GITHUB_CHECKING : FW_GITHUB_DOWNLOADING;
        s_status.progress = 0;
    }
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        xTaskNotifyGive(s_task);
    }
    return err;
}

esp_err_t fw_github_check_now(void)
{
    return ask(CMD_CHECK);
}

esp_err_t fw_github_install(void)
{
    return ask(CMD_INSTALL);
}

esp_err_t fw_github_set_beta(bool on)
{
    // The setting goes in before the check is asked for, because the check reads it as it starts.
    bool before = settings_get_beta_updates();
    settings_set_beta_updates(on);
    esp_err_t err = ask(CMD_CHECK);
    if (err != ESP_OK) {
        settings_set_beta_updates(before);   // busy: the switch stays as it was
        return err;
    }
    // What was found before belongs to the other setting: forget it until GitHub has answered again.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.available = false;
    s_status.checked = false;
    s_status.latest[0] = 0;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static bool wifi_up(void)
{
    wifi_mgr_status_t w;
    wifi_mgr_get_status(&w);
    return w.connected;
}

/* "0.2.0" or "v0.2.0" -> {0, 2, 0, NOT_A_BETA}; "v0.4.0-beta.7" -> {0, 4, 0, 7}.
 * The fourth number puts the betas of a version in order, and the finished release after them.
 * False if it does not look like a version. */
static bool parse_version(const char *text, int out[4])
{
    if (*text == 'v' || *text == 'V') {
        text++;
    }
    out[0] = out[1] = out[2] = 0;
    out[3] = NOT_A_BETA;
    return sscanf(text, "%d.%d.%d-beta.%d", &out[0], &out[1], &out[2], &out[3]) >= 2;
}

static bool is_newer(const char *candidate, const char *than)
{
    int a[4], b[4];
    if (!parse_version(candidate, a) || !parse_version(than, b)) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (a[i] != b[i]) {
            return a[i] > b[i];
        }
    }
    return false;
}

/* A tag ends up inside a web address, so only plain characters are accepted. */
static bool tag_is_plain(const char *tag)
{
    size_t n = strlen(tag);
    if (n == 0 || n >= sizeof(s_tag)) {
        return false;
    }
    for (const char *p = tag; *p; p++) {
        bool ok = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  *p == '.' || *p == '-' || *p == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* Fetch GitHub's description of the latest release (or, at RECENT_URL, of the newest few) into
 * buf, as text. Returns the HTTP status (200 = fine, 404 = the repository has no release yet),
 * or -1 if GitHub could not be reached. */
static int fetch_releases(const char *url, char *buf, size_t max)
{
    const esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,   // verify GitHub's certificate
        .timeout_ms = HTTP_TIMEOUT_MS,
        .user_agent = USER_AGENT,
        .buffer_size = 4096,                          // GitHub sends long headers
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return -1;
    }
    int status = -1;
    size_t got = 0;
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");
    if (esp_http_client_open(client, 0) == ESP_OK && esp_http_client_fetch_headers(client) >= 0) {
        status = esp_http_client_get_status_code(client);
        while (got < max) {
            int n = esp_http_client_read(client, buf + got, max - got);
            if (n <= 0) {
                break;
            }
            got += n;
        }
    }
    buf[got] = 0;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return status;
}

/* One release as GitHub describes it. If it carries the firmware file, has a readable tag and is
 * newer than the one in `best` (or `best` is still empty), it becomes the new `best`.
 * Returns false if the description could not be read at all. */
static bool consider_release(const cJSON *release, char best[sizeof(s_tag)])
{
    const cJSON *jtag = cJSON_GetObjectItem(release, "tag_name");
    if (!cJSON_IsString(jtag) || !tag_is_plain(jtag->valuestring)) {
        return false;
    }
    bool has_file = false;
    const cJSON *asset;
    cJSON_ArrayForEach(asset, cJSON_GetObjectItem(release, "assets")) {
        const cJSON *name = cJSON_GetObjectItem(asset, "name");
        if (cJSON_IsString(name) && strcmp(name->valuestring, FW_GITHUB_ASSET) == 0) {
            has_file = true;
        }
    }
    if (has_file && (!best[0] || is_newer(jtag->valuestring, best))) {
        strlcpy(best, jtag->valuestring, sizeof(s_tag));
    }
    return true;
}

static void do_check(void)
{
    set_state(FW_GITHUB_CHECKING);
    int64_t now = esp_timer_get_time();
    s_next_auto_us = now + (int64_t)AUTO_RETRY_S * 1000000;   // pushed out to a day below, if this works
    if (!wifi_up()) {
        set_error("The printer is not on Wi-Fi, so it cannot reach GitHub");
        set_state(FW_GITHUB_IDLE);
        return;
    }
    char *buf = heap_caps_malloc(RELEASE_JSON_MAX + 1, MALLOC_CAP_SPIRAM);
    if (!buf) {
        set_error("Out of memory");
        set_state(FW_GITHUB_IDLE);
        return;
    }
    bool beta = settings_get_beta_updates();
    int http = fetch_releases(beta ? RECENT_URL : LATEST_URL, buf, RELEASE_JSON_MAX);

    char tag[sizeof(s_tag)] = "";   // the newest release that carries the firmware file; "" if there is none
    const char *problem = NULL;
    if (http == 200) {
        // Without betas the answer is one release; with them it is a list, and the highest version wins.
        cJSON *json = cJSON_Parse(buf);
        bool readable = json != NULL;
        if (cJSON_IsArray(json)) {
            const cJSON *release;
            cJSON_ArrayForEach(release, json) {
                readable = consider_release(release, tag) && readable;
            }
        } else if (json) {
            readable = consider_release(json, tag);
        }
        if (!readable) {
            problem = "GitHub's answer could not be read";
        }
        cJSON_Delete(json);
    } else if (http == 404) {
        // The repository has no release yet: nothing newer, and nothing wrong.
    } else if (http == 403 || http == 429) {
        problem = "GitHub is not taking more requests right now. Try again in an hour";
    } else {
        problem = "Could not reach GitHub. Check that the Wi-Fi has internet";
    }
    free(buf);

    if (problem) {
        set_error(problem);
        set_state(FW_GITHUB_IDLE);
        return;
    }
    const char *running = esp_app_get_description()->version;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_tag, tag, sizeof(s_tag));
    strlcpy(s_status.latest, (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag, sizeof(s_status.latest));
    s_status.available = tag[0] && is_newer(tag, running);
    s_status.checked = true;
    s_status.error[0] = 0;
    s_status.state = FW_GITHUB_IDLE;
    s_checked_at_us = esp_timer_get_time();
    bool available = s_status.available;
    xSemaphoreGive(s_lock);
    s_next_auto_us = esp_timer_get_time() + (int64_t)AUTO_CHECK_S * 1000000;
    ESP_LOGI(TAG, "running v%s, latest release%s: %s%s", running, beta ? " (betas included)" : "",
             tag[0] ? tag : "none", available ? " (newer: an update is available)" : "");
}

static void do_install(void)
{
    char url[160];
    snprintf(url, sizeof(url), DOWNLOAD_URL_FMT, s_tag);
    ESP_LOGI(TAG, "downloading %s", url);

    // GitHub answers with a redirect to a very long address on its file servers,
    // which is why both buffers are larger than usual.
    const esp_http_client_config_t http = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .user_agent = USER_AGENT,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .max_redirection_count = 5,
        .keep_alive_enable = true,
    };
    const esp_https_ota_config_t config = { .http_config = &http };
    esp_https_ota_handle_t ota = NULL;
    const char *problem = NULL;

    esp_err_t err = esp_https_ota_begin(&config, &ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not start the download: %s", esp_err_to_name(err));
        set_error(wifi_up() ? "Could not download the update from GitHub. Nothing was changed"
                            : "The printer is not on Wi-Fi, so it cannot reach GitHub");
        set_state(FW_GITHUB_IDLE);
        return;
    }

    // The start of the file says what it is: refuse anything that is not TikPrint firmware.
    esp_app_desc_t desc;
    if (esp_https_ota_get_img_desc(ota, &desc) != ESP_OK ||
        strncmp(desc.project_name, esp_app_get_description()->project_name, sizeof(desc.project_name)) != 0) {
        problem = "The file in that release is not TikPrint firmware. Nothing was changed";
    } else {
        ESP_LOGI(TAG, "it is v%.*s, built %.*s %.*s", (int)sizeof(desc.version), desc.version,
                 (int)sizeof(desc.date), desc.date, (int)sizeof(desc.time), desc.time);
        int size = esp_https_ota_get_image_size(ota), last = -1;
        while ((err = esp_https_ota_perform(ota)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            int percent = size > 0 ? (int)((int64_t)esp_https_ota_get_image_len_read(ota) * 100 / size) : 0;
            if (percent != last) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_status.progress = percent;
                xSemaphoreGive(s_lock);
                if (percent / 10 != last / 10) {
                    ESP_LOGI(TAG, "downloaded %d%%", percent);
                }
                last = percent;
            }
        }
        if (err != ESP_OK || !esp_https_ota_is_complete_data_received(ota)) {
            ESP_LOGE(TAG, "download broke off: %s", esp_err_to_name(err));
            problem = "The download broke off. Nothing was changed";
        }
    }
    if (problem) {
        esp_https_ota_abort(ota);
        set_error(problem);
        set_state(FW_GITHUB_IDLE);
        return;
    }

    // Checks the whole file and tells the bootloader to start it next time.
    err = esp_https_ota_finish(ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the downloaded firmware did not pass its check: %s", esp_err_to_name(err));
        set_error("The download arrived damaged. Nothing was changed");
        set_state(FW_GITHUB_IDLE);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.progress = 100;
    s_status.state = FW_GITHUB_RESTARTING;
    xSemaphoreGive(s_lock);

    // A restart throws away tickets still in the queue: give them a moment to print.
    for (int waited = 0; waited < WAIT_FOR_PRINTS_S; waited++) {
        int waiting;
        bool printing, paused;
        printer_queue_info(&waiting, &printing, &paused);
        if (!printing && waiting == 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "update installed");
    fw_update_restart_soon();
}

static void github_task(void *arg)
{
    s_next_auto_us = (int64_t)FIRST_CHECK_S * 1000000;
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));   // woken when asked; otherwise look every 10 s
        xSemaphoreTake(s_lock, portMAX_DELAY);
        command_t command = s_command;
        s_command = CMD_NONE;
        fw_github_state_t state = s_status.state;
        xSemaphoreGive(s_lock);

        if (command == CMD_CHECK) {
            do_check();
        } else if (command == CMD_INSTALL) {
            do_install();
        } else if (state == FW_GITHUB_IDLE && esp_timer_get_time() >= s_next_auto_us && wifi_up()) {
            do_check();   // the quiet daily look. A failure here is not shown as an error
            set_error("");
        }
    }
}

esp_err_t fw_github_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(github_task, "fw_github", TASK_STACK, NULL, 4, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
