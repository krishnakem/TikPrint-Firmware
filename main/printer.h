#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Paper: 80 mm thermal, 576 dots wide, 203 dpi. */
#define PRINTER_WIDTH_DOTS   576
#define PRINTER_WIDTH_BYTES  (PRINTER_WIDTH_DOTS / 8)
#define PRINTER_MAX_ROWS     8192   // one print: up to 4096 rows of text drawn by the page + a 4096-row file

/** One thing to print. Pointers only need to stay valid during printer_print_job(). */
typedef struct {
    uint32_t number;        // ticket number (#0001, #0002, ...)
    const char *title;      // may be ""
    const char *body;       // may be ""
    const char *source;     // e.g. "TODOIST · TODAY", may be ""
    const uint8_t *image;   // 1 bit per dot, 72 bytes per row, leftmost dot = top bit, 1 = black. NULL if none
    uint16_t image_rows;    // number of rows in image (width is always PRINTER_WIDTH_DOTS)
    bool text_in_image;     // the page already drew number, source, title and body at the top of the image
                            // (in its own font), so the printer must not print them again in its built-in font
} printer_job_t;

/** A printer driver. There are two: the real thermal printer and a fake one that only logs. */
typedef struct {
    const char *name;
    esp_err_t (*init)(void);
    esp_err_t (*print)(const printer_job_t *job);
    /** Optional: the most recent job, or NULL. The returned pointers stay valid until the next print. */
    const printer_job_t *(*last_job)(void);
} printer_driver_t;

/** Pick the driver and initialise it. */
esp_err_t printer_init(void);

/** Print a job on the active printer. */
esp_err_t printer_print_job(const printer_job_t *job);

/** The last job that was printed, or NULL if none yet (or the driver doesn't keep one). */
const printer_job_t *printer_last_job(void);

const char *printer_name(void);

/* Available drivers */
extern const printer_driver_t printer_fake_driver;
extern const printer_driver_t printer_thermal_driver;

/** Thermal driver only: print one test line. The settings given are changed first and stay until reboot:
 *  baud > 0 = serial speed, pin >= 0 = board pin, invert 0/1 = flip the signal (-1 = leave as is). */
esp_err_t printer_thermal_test(int baud, int pin, int invert);
