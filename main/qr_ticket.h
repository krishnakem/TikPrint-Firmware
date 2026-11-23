#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "ticket_draw.h"

/*
 * QR codes, made on the board and printed as dots like any other picture.
 *
 * The code is made here (Nayuki's qrcodegen, inside the espressif/qrcode component) and drawn into the
 * ticket's image. The printer module's own QR command is never used, so this works with any printer
 * that can print dots. Error correction is always "medium": the code still reads with 15% of it damaged.
 *
 * A QR job is {"data", "size", "caption"}: see "QR codes" in TikPrint Cloud's docs/device-api.md, the one
 * place the format is written down. It arrives in a print body (api_print_body in api.c), whether the
 * printer's own page sent it or TikPrint Cloud did.
 */

#define QR_DATA_MAX     2400   // most bytes of data read from a job. The largest code holds 2331: more can never fit
#define QR_CAPTION_MAX  120    // most bytes of caption: longer ones are cut
#define QR_BLOCK_ROWS   900    // the most rows a code and its caption take (a header line included)

/* How wide the code prints, its white edge included: a third of the paper, two thirds, or all of it.
 * A code with very many squares prints wider than asked, so that a phone can still read it. */
typedef enum { QR_SIZE_SMALL, QR_SIZE_MEDIUM, QR_SIZE_FULL } qr_size_t;

typedef struct {
    const char *data;      // the link or text in the code
    qr_size_t size;
    const char *caption;   // printed under the code; "" for none
} qr_job_t;

/** "small", "medium" or "full" -> the size. False for anything else. */
bool qr_size_from_name(const char *name, qr_size_t *size);

/**
 * Draw a QR code of text in the middle of the paper with its top at y: at most `room` dots wide, each
 * square at most max_dot dots. No white edge is added: leave white paper around it.
 * Returns its height in dots, or 0 if it could not be drawn (text too long for a QR code, no memory,
 * or no room left on the canvas).
 */
int qr_draw(ticket_canvas_t *canvas, const char *text, int y, int room, int max_dot);

/**
 * Make the picture for a QR job: the rows of `above` (the ticket's own picture; NULL and 0 for none),
 * then the code with its white edge, then the caption. with_header = also draw the ticket's header line
 * at the top (for a ticket that is the code and nothing else).
 * On ESP_OK *out is a new canvas (give it back with ticket_canvas_free) and *lettered says whether the
 * header and caption could be drawn (false: the board has no letters; the code is there all the same).
 *   ESP_ERR_INVALID_SIZE   the data is too long for a QR code
 *   ESP_ERR_NO_MEM         out of memory
 */
esp_err_t qr_ticket_render(const qr_job_t *qr, const uint8_t *above, int above_rows, bool with_header,
                           ticket_canvas_t *out, bool *lettered);
