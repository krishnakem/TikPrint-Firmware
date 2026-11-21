#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Paper: 80 mm thermal, 576 dots wide, 203 dpi. */
#define PRINTER_WIDTH_DOTS   576
#define PRINTER_WIDTH_BYTES  (PRINTER_WIDTH_DOTS / 8)
#define PRINTER_MAX_ROWS     8192   // one print: up to 4096 rows of text drawn by the page + a 4096-row file

/** One thing to print. Pointers only need to stay valid during printer_print_job(): the queue takes a copy. */
typedef struct {
    uint32_t number;        // ticket number (#0001, #0002, ...). Filled in by the print queue
    const char *title;      // may be ""
    const char *body;       // may be ""
    const char *source;     // e.g. "TODOIST · TODAY", may be ""
    const uint8_t *image;   // 1 bit per dot, 72 bytes per row, leftmost dot = top bit, 1 = black. NULL if none
    uint16_t image_rows;    // number of rows in image (width is always PRINTER_WIDTH_DOTS)
    bool text_in_image;     // the page already drew number, source, title and body at the top of the image
                            // (in its own font), so the printer must not print them again in its built-in font
} printer_job_t;

/** What a job was given when it joined the print queue. */
typedef struct {
    uint32_t number;        // its ticket number
    int position;           // its place in line: 0 = printing now, 1 = one ticket ahead of it, ...
} printer_ticket_t;

/** What state the printer is in, worst problem first when there are several. */
typedef enum {
    PRINTER_STATUS_UNKNOWN = 0,   // no answer: the status wire is not connected, or the printer is off
    PRINTER_STATUS_OK,
    PRINTER_STATUS_PAPER_LOW,     // still prints, but the roll is nearly finished
    PRINTER_STATUS_OUT_OF_PAPER,
    PRINTER_STATUS_COVER_OPEN,
    PRINTER_STATUS_OFFLINE,       // the printer says it is offline, without saying why
    PRINTER_STATUS_ERROR,         // cutter jam or another fault
} printer_status_t;

#define PRINTER_STATUS_QUERIES 4   // the printer is asked four questions (ESC/POS "DLE EOT 1..4")

/** Everything one status check found out. */
typedef struct {
    int raw[PRINTER_STATUS_QUERIES];      // the byte each question got back, or -1 if no answer came
    bool valid[PRINTER_STATUS_QUERIES];   // that byte looks like a real status byte (not line noise)
    int answered;                         // how many valid answers came (0 = status unknown)
    bool online, cover_open, paper_out, paper_low, cutter_error, other_error;
    printer_status_t status;              // the summary of all of the above
} printer_status_report_t;

/** A printer driver. There are two: the real thermal printer and a fake one that only logs. */
typedef struct {
    const char *name;
    esp_err_t (*init)(void);
    /** Print one job, start to finish. Returns when the whole ticket has been sent. */
    esp_err_t (*print)(const printer_job_t *job);
    /** Optional: the last job that started printing, or NULL. Only call between the two below. */
    const printer_job_t *(*last_job)(void);
    /** Optional: stop the driver replacing its last job while someone reads it, and let it again. */
    void (*last_job_lock)(void);
    void (*last_job_unlock)(void);
    /** Optional: ask the printer how it is. Waits up to wait_ms for the serial line to be free.
     *  ESP_OK = *out is filled in (also when the printer stayed silent), ESP_ERR_TIMEOUT = line busy. */
    esp_err_t (*read_status)(printer_status_report_t *out, uint32_t wait_ms);
} printer_driver_t;

/** Pick the driver, initialise it, and start the print queue. */
esp_err_t printer_init(void);

/**
 * Put a job in the print queue (see print_queue.c). Returns at once; the ticket prints in the background.
 *   ESP_OK                 queued; *ticket holds its number and its place in line
 *   ESP_ERR_INVALID_STATE  the queue is full
 *   ESP_ERR_NO_MEM         out of memory
 */
esp_err_t printer_print_job(const printer_job_t *job, printer_ticket_t *ticket);

/** How many jobs are waiting in the queue, whether one is printing right now, and whether
 *  the queue is paused (waiting for paper, or for the cover to be closed). */
void printer_queue_info(int *waiting, bool *printing, bool *paused);

/**
 * The printer's state as of the last check, without asking it again. The print queue checks
 * before and after every ticket and every few seconds in between.
 * *checked_s_ago = seconds since that check, or -1 if there has been none yet.
 */
printer_status_t printer_get_status(int *checked_s_ago);

/** "ok", "out_of_paper", "paper_low", "cover_open", "offline", "error" or "unknown". */
const char *printer_status_name(printer_status_t status);

/**
 * Ask the printer now and report every detail (for the diagnostic endpoint).
 *   ESP_ERR_TIMEOUT        a ticket is being sent: try again when it is done
 *   ESP_ERR_NOT_SUPPORTED  the fake printer is in use
 */
esp_err_t printer_read_status(printer_status_report_t *out);

/**
 * The last job that STARTED printing, or NULL if none yet (or the driver doesn't keep one).
 * The job stays untouched until you call printer_last_job_unlock(), which you must
 * always do afterwards (also when this returned NULL). Be quick: the next ticket
 * cannot start printing while you hold it.
 */
const printer_job_t *printer_last_job_lock(void);
void printer_last_job_unlock(void);

const char *printer_name(void);

/**
 * Print one test line (thermal printer only). See printer_thermal_test() for the settings.
 *   ESP_ERR_INVALID_STATE  a ticket is printing or waiting
 *   ESP_ERR_NOT_SUPPORTED  the fake printer is in use
 */
esp_err_t printer_test(int baud, int pin, int invert);

/** One pace test strip: up to PRINTER_PACE_VARIANTS blocks, each sent with its own settings. */
#define PRINTER_PACE_VARIANTS 8
typedef struct {
    int count;                                // how many blocks
    int band_rows[PRINTER_PACE_VARIANTS];     // rows per command for each block
    int us_per_dot[PRINTER_PACE_VARIANTS];    // pause per black dot for each block, in microseconds
    int rows;                                 // height of every block, in rows
    int black_percent;                        // how black the blocks are (100 = solid)
} printer_pace_test_t;

/**
 * Print a strip that compares ways of sending a dark image (see printer_thermal_pace_test).
 *   ESP_ERR_INVALID_STATE  a ticket is printing or waiting
 *   ESP_ERR_NOT_SUPPORTED  the fake printer is in use
 *   ESP_ERR_INVALID_ARG    settings out of range
 */
esp_err_t printer_pace_test(const printer_pace_test_t *test);

/* Available drivers */
extern const printer_driver_t printer_fake_driver;
extern const printer_driver_t printer_thermal_driver;

/** Thermal driver only: print one test line. The settings given are changed first and stay until reboot:
 *  baud > 0 = serial speed, pin >= 0 = board pin, invert 0/1 = flip the signal (-1 = leave as is).
 *  Call it through printer_test(), which makes sure no ticket is printing at the same time. */
esp_err_t printer_thermal_test(int baud, int pin, int invert);

/** Thermal driver only: the pace test. Call it through printer_pace_test(). */
esp_err_t printer_thermal_pace_test(const printer_pace_test_t *test);
