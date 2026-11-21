#pragma once

#include <stdbool.h>
#include "esp_err.h"

#include "printer.h"

/** The function that really prints one job. It may take as long as it likes. */
typedef esp_err_t (*print_queue_print_fn)(const printer_job_t *job);

/**
 * Asked before every ticket, right after every ticket, and every few seconds in between:
 * "can the printer take a ticket now?". While the answer is no (out of paper, cover open)
 * the queue is paused: the waiting tickets are kept, and printing carries on by itself
 * once the answer is yes again. May be NULL (= always yes).
 */
typedef bool (*print_queue_ready_fn)(void);

/** Start the queue and its background task. Jobs are printed one at a time with print_fn. */
esp_err_t print_queue_init(print_queue_print_fn print_fn, print_queue_ready_fn ready_fn);

/**
 * Add a job to the end of the line. The queue takes its own copy, so the caller's
 * pointers can go away as soon as this returns. The ticket number is handed out here.
 *   ESP_OK                 queued; *ticket says its number and its place in line
 *   ESP_ERR_INVALID_STATE  the queue is full
 *   ESP_ERR_NO_MEM         no memory for the copy
 */
esp_err_t print_queue_add(const printer_job_t *job, printer_ticket_t *ticket);

/** How many jobs are waiting, whether one is printing right now, and whether the
 *  queue is paused (tickets are waiting for paper or for the cover to be closed). */
void print_queue_get_info(int *waiting, bool *printing, bool *paused);

/**
 * For things that use the printer without the queue (the test line).
 * print_queue_claim() succeeds only if nothing is printing or waiting, and then
 * holds back any new jobs until print_queue_release() is called.
 */
bool print_queue_claim(void);
void print_queue_release(void);
