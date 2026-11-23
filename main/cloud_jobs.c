/*
 * The cloud jobs this printer has finished lately (see cloud_jobs.h).
 * Kept in memory and saved as one block: NVS namespace "cloud", key "jobs_done".
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "cloud_jobs.h"

static const char *TAG = "cloud_jobs";

#define NVS_NS   "cloud"
#define KEY_JOBS "jobs_done"
#define VERSION  1

typedef struct {
    char id[CLOUD_JOB_ID_MAX];
    uint8_t end;        // a cloud_job_end_t; 0 = this place is empty
    uint8_t reported;   // the cloud has heard
} done_job_t;

/* The saved block: a ring. `next` is where the next finished job goes (and so the oldest one sits). */
static struct {
    uint8_t version;
    uint8_t next;
    done_job_t job[CLOUD_JOBS_KEPT];
} s_done;

const char *cloud_job_end_message(cloud_job_end_t end)
{
    switch (end) {
    case CLOUD_JOB_UNREADABLE:  return "The printer could not read this ticket";
    case CLOUD_JOB_TOO_LARGE:   return "The ticket is too large for the printer";
    case CLOUD_JOB_NO_DOWNLOAD: return "The printer could not download this ticket";
    default:                    return "The printer could not print this ticket";
    }
}

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "could not save the finished jobs");
        return;
    }
    nvs_set_blob(h, KEY_JOBS, &s_done, sizeof(s_done));
    nvs_commit(h);
    nvs_close(h);
}

static void reset(void)
{
    memset(&s_done, 0, sizeof(s_done));
    s_done.version = VERSION;
}

void cloud_jobs_init(void)
{
    nvs_handle_t h;
    size_t n = sizeof(s_done);
    bool loaded = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        loaded = nvs_get_blob(h, KEY_JOBS, &s_done, &n) == ESP_OK && n == sizeof(s_done) &&
                 s_done.version == VERSION && s_done.next < CLOUD_JOBS_KEPT;
        nvs_close(h);
    }
    if (!loaded) {
        reset();
    }
    for (int i = 0; i < CLOUD_JOBS_KEPT; i++) {
        s_done.job[i].id[CLOUD_JOB_ID_MAX - 1] = 0;   // whatever was saved, an ID always ends
    }
}

static done_job_t *find(const char *id)
{
    for (int i = 0; i < CLOUD_JOBS_KEPT; i++) {
        if (s_done.job[i].end && strcmp(s_done.job[i].id, id) == 0) {
            return &s_done.job[i];
        }
    }
    return NULL;
}

bool cloud_jobs_find(const char *id, cloud_job_end_t *end)
{
    const done_job_t *job = find(id);
    if (job) {
        *end = (cloud_job_end_t)job->end;
    }
    return job != NULL;
}

void cloud_jobs_finished(const char *id, cloud_job_end_t end)
{
    done_job_t *job = find(id);
    if (!job) {
        job = &s_done.job[s_done.next];
        s_done.next = (uint8_t)((s_done.next + 1) % CLOUD_JOBS_KEPT);
    }
    memset(job, 0, sizeof(*job));
    strlcpy(job->id, id, sizeof(job->id));
    job->end = (uint8_t)end;
    save();
}

static void set_reported(const char *id, bool reported)
{
    done_job_t *job = find(id);
    if (job && (job->reported != 0) != reported) {
        job->reported = reported;
        save();
    }
}

void cloud_jobs_reported(const char *id)
{
    set_reported(id, true);
}

void cloud_jobs_report_again(const char *id)
{
    set_reported(id, false);
}

bool cloud_jobs_next_unreported(char id[CLOUD_JOB_ID_MAX], cloud_job_end_t *end)
{
    for (int k = 0; k < CLOUD_JOBS_KEPT; k++) {
        const done_job_t *job = &s_done.job[(s_done.next + k) % CLOUD_JOBS_KEPT];   // from the oldest on
        if (job->end && !job->reported) {
            strlcpy(id, job->id, CLOUD_JOB_ID_MAX);
            *end = (cloud_job_end_t)job->end;
            return true;
        }
    }
    return false;
}

void cloud_jobs_clear(void)
{
    reset();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, KEY_JOBS);   // "there was none" is fine too
        nvs_commit(h);
        nvs_close(h);
    }
}
