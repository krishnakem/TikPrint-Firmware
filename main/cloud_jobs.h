#pragma once

#include <stdbool.h>

/*
 * The cloud jobs this printer has finished lately, saved on the board (NVS namespace "cloud").
 *
 * The cloud keeps offering a job until the printer has reported back on it ("ack"). If that report
 * gets lost, the job shows up again, and it must be reported again, not printed again. So the
 * board remembers the last CLOUD_JOBS_KEPT jobs it finished, how each ended, and whether the cloud
 * has heard. It is saved, so a restart between printing and reporting does not print twice.
 *
 * Only the cloud task uses this (after cloud_jobs_init at startup): there is no lock.
 */

#define CLOUD_JOBS_KEPT   20
#define CLOUD_JOB_ID_MAX  40   // room for a job ID (36 characters), including the ending 0

/* How a job ended. Anything but CLOUD_JOB_PRINTED is reported as "failed", with these words. */
typedef enum {
    CLOUD_JOB_PRINTED = 1,
    CLOUD_JOB_UNREADABLE,     // the print body is not a ticket
    CLOUD_JOB_TOO_LARGE,
    CLOUD_JOB_NO_DOWNLOAD,    // the print body could not be fetched, after several tries
    CLOUD_JOB_NOT_PRINTED,    // the printer gave up on it
} cloud_job_end_t;

/** What to tell the cloud about a job that did not print. */
const char *cloud_job_end_message(cloud_job_end_t end);

/** Load the list. Call once at startup. */
void cloud_jobs_init(void);

/** Is this job finished already? If so *end says how. */
bool cloud_jobs_find(const char *id, cloud_job_end_t *end);

/** A job has ended: remember it (the oldest one is forgotten to make room). Not yet reported. */
void cloud_jobs_finished(const char *id, cloud_job_end_t end);

/** The cloud has heard about this job (or does not want to hear). */
void cloud_jobs_reported(const char *id);

/** The cloud is offering this finished job again, so it has not heard: report it again. */
void cloud_jobs_report_again(const char *id);

/** A finished job the cloud has not heard about yet, oldest first. False if there is none. */
bool cloud_jobs_next_unreported(char id[CLOUD_JOB_ID_MAX], cloud_job_end_t *end);

/** Forget everything (the printer is no longer linked). */
void cloud_jobs_clear(void);
