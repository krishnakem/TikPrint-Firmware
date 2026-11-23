/*
 * The pairing ticket (see cloud_ticket.h):
 *
 *   TikPrint | Ann Arbor, MI            NOV 22, 2026 | 8:00 AM EST     header line
 *   TIKPRINT CLOUD                                                      source line
 *   Link this printer                                                   title, big
 *
 *                        [ QR code ]
 *
 *                         K7QM-3XPD                                     the code, big
 *                  Scan the code with your phone,
 *                or type it at <the site>/link
 *                     Good for 10 minutes.
 *
 * It is drawn as dots in the font of the ticket header, like the tickets of the connectors. If the
 * board has no letters for that font, the words print in the printer's built-in font instead,
 * with the QR code below them.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "qrcode.h"

#include "cloud_ticket.h"
#include "config.h"
#include "printer.h"
#include "settings.h"
#include "ticket_draw.h"
#include "ticket_header.h"

static const char *TAG = "cloud_ticket";

#define WIDTH        PRINTER_WIDTH_DOTS
#define MAX_ROWS     1000
#define HEADER_SIZE  20    // the sizes the board has letters for: see tools/make_ticket_fonts.py
#define HEADER_SMALL 17
#define TITLE_SIZE   44
#define TEXT_SIZE    22
#define QR_MAX_VERSION 10  // up to 57 x 57 squares: room for an address of about 200 letters
#define QR_MAX_SIDE  57
#define QR_MAX_DOT   12    // biggest square, in dots (1.5 mm). An address of 50 letters then makes a code 50 mm wide
#define QR_ROOM      432   // the code is never wider than this (54 mm), so white paper is left on both sides

/* The QR code as squares: 1 = black. */
typedef struct {
    int side;
    uint8_t square[QR_MAX_SIDE][QR_MAX_SIDE];
} qr_t;

/* Called by the QR code library with the finished code. */
static void keep_qr(esp_qrcode_handle_t code, void *arg)
{
    qr_t *qr = arg;
    int side = esp_qrcode_get_size(code);
    if (side > QR_MAX_SIDE) {
        return;
    }
    qr->side = side;
    for (int y = 0; y < side; y++) {
        for (int x = 0; x < side; x++) {
            qr->square[y][x] = esp_qrcode_get_module(code, x, y);
        }
    }
}

/* Draw the QR code in the middle of the paper with its top at y. Returns its height in dots. */
static int draw_qr(ticket_canvas_t *c, const qr_t *qr, int y)
{
    int dot = QR_ROOM / qr->side;
    dot = dot > QR_MAX_DOT ? QR_MAX_DOT : dot;
    int left = (WIDTH - qr->side * dot) / 2;
    for (int row = 0; row < qr->side * dot && y + row < c->max_rows; row++) {
        uint8_t *line = c->bits + (size_t)(y + row) * PRINTER_WIDTH_BYTES;
        for (int col = 0; col < qr->side * dot; col++) {
            if (qr->square[row / dot][col / dot]) {
                int x = left + col;
                line[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
            }
        }
    }
    return qr->side * dot;
}

/* Draw one line of text in the middle of the paper and move *y below it. */
static void draw_centred(ticket_canvas_t *c, const ticket_face_t *face, int *y, const char *text)
{
    int w = ticket_text_width(face, text, strlen(text));
    if (w > WIDTH) {
        ticket_draw_wrapped(c, face, 0, y, text, strlen(text));
        return;
    }
    ticket_draw_text(c, face, (WIDTH - w) / 2, *y, text, strlen(text));
    *y += ticket_face_line(face);
}

/* The whole ticket as dots. False if the board has no letters for the font. */
static bool draw_ticket(ticket_canvas_t *c, int font, const qr_t *qr, const char *code, const char *site, int minutes)
{
    const ticket_face_t *head = ticket_face(font, true, HEADER_SIZE);
    const ticket_face_t *head_small = ticket_face(font, true, HEADER_SMALL);
    const ticket_face_t *big = ticket_face(font, true, TITLE_SIZE);
    const ticket_face_t *text = ticket_face(font, false, TEXT_SIZE);
    const ticket_face_t *bold = ticket_face(font, true, TEXT_SIZE);
    if (!head || !head_small || !big || !text || !bold) {
        return false;
    }
    int y = 2;
    char left[TICKET_HEADER_MAX], right[TICKET_HEADER_MAX], line[96];
    ticket_header_text(left, right, true);
    for (char *p = right; *p; p++) {
        *p = (*p >= 'a' && *p <= 'z') ? (char)(*p - 'a' + 'A') : *p;
    }
    const ticket_face_t *h = head;
    if (ticket_text_width(h, left, strlen(left)) + ticket_text_width(h, right, strlen(right)) + HEADER_SIZE > WIDTH) {
        h = head_small;
    }
    ticket_draw_text(c, h, 0, y, left, strlen(left));
    ticket_draw_right(c, h, WIDTH, y, right, strlen(right));
    y += ticket_face_line(h) + 8;
    ticket_draw_wrapped(c, head, 0, &y, "TIKPRINT CLOUD", 14);
    y += 6;
    ticket_draw_wrapped(c, big, 0, &y, "Link this printer", 17);

    y += 40;                      // white paper around the code: a phone needs it to find the code
    y += draw_qr(c, qr, y) + 40;

    draw_centred(c, big, &y, code);
    y += 10;
    draw_centred(c, text, &y, "Scan the code with your phone,");
    snprintf(line, sizeof(line), "%s/link", site);
    draw_centred(c, text, &y, "or type it at");
    draw_centred(c, bold, &y, line);
    y += 6;
    snprintf(line, sizeof(line), "Good for %d minutes.", minutes);
    draw_centred(c, text, &y, line);
    c->rows = y + 8;
    return true;
}

esp_err_t cloud_ticket_print(const char *link_url, const char *code, int minutes)
{
    static qr_t qr;   // only the cloud task comes here, one ticket at a time
    qr.side = 0;
    esp_qrcode_config_t cfg = {
        .display_func_with_cb = keep_qr,
        .max_qrcode_version = QR_MAX_VERSION,
        .qrcode_ecc_level = ESP_QRCODE_ECC_MED,   // still reads with 15% of it damaged or badly printed
        .user_data = &qr,
    };
    if (esp_qrcode_generate(&cfg, link_url) != ESP_OK || qr.side == 0) {
        ESP_LOGW(TAG, "could not make a QR code of the link");
        return ESP_ERR_INVALID_ARG;
    }
    ticket_canvas_t canvas;
    if (!ticket_canvas_init(&canvas, MAX_ROWS)) {
        return ESP_ERR_NO_MEM;
    }
    const char *site = strstr(CLOUD_URL, "://");
    site = site ? site + 3 : CLOUD_URL;
    char body[200];
    printer_job_t job = { .title = "Link this printer", .body = body, .source = "TIKPRINT CLOUD" };
    settings_t settings;
    settings_get(&settings);
    int font = ticket_font_id(settings.header_font);
    font = font >= 0 ? font : ticket_font_id("Inter");
    if (font >= 0 && draw_ticket(&canvas, font, &qr, code, site, minutes)) {
        snprintf(body, sizeof(body), "Code %s", code);   // for the log and /api/last-ticket: the words are in the picture
        job.text_in_image = true;
    } else {
        // No letters to draw with: the words in the printer's own font, the code as a picture below them.
        snprintf(body, sizeof(body), "Scan the code below with your phone,\nor type %s at\n%s/link\nGood for %d minutes.",
                 code, site, minutes);
        memset(canvas.bits, 0, (size_t)MAX_ROWS * PRINTER_WIDTH_BYTES);
        canvas.rows = 24 + draw_qr(&canvas, &qr, 24) + 24;
    }
    job.image = canvas.bits;
    job.image_rows = (uint16_t)canvas.rows;
    printer_ticket_t ticket;
    esp_err_t err = printer_print_job(&job, &ticket);
    ticket_canvas_free(&canvas);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "pairing ticket is #%04u (QR code of %d x %d squares)", (unsigned)ticket.number, qr.side, qr.side);
    }
    return err;
}
