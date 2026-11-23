/*
 * QR codes drawn as dots (see qr_ticket.h).
 *
 * A ticket that is only a code:
 *
 *   TikPrint | Ann Arbor, MI            NOV 23, 2026 | 8:00 AM EST     header line
 *
 *                        [ QR code ]                                    with 4 white squares on every side
 *
 *                      Guest Wi-Fi: Lakehouse                           the caption, if there is one
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "qrcode.h"

#include "printer.h"
#include "qr_ticket.h"
#include "settings.h"
#include "ticket_draw.h"
#include "ticket_header.h"

#define WIDTH        PRINTER_WIDTH_DOTS
#define QUIET        4     // white squares around the code (the "quiet zone" every reader expects)
#define MIN_DOT      3     // smallest square, in dots (0.375 mm): below this a thermal print is hard to read
#define CAPTION_SIZE 22    // a size the board has letters for: see tools/make_ticket_fonts.py

/* What on_code needs to draw the code it is handed. */
typedef struct {
    ticket_canvas_t *canvas;
    int y, room, min_dot, max_dot, quiet;
    int height;   // set by on_code: rows used, white edge included. 0 = not drawn
} draw_t;

/* Called by the QR code library with the finished code: draw it straight onto the canvas. */
static void on_code(esp_qrcode_handle_t code, void *arg)
{
    draw_t *d = arg;
    int side = esp_qrcode_get_size(code), cells = side + 2 * d->quiet;
    int dot = d->room / cells;
    dot = dot < d->min_dot ? d->min_dot : dot;
    dot = dot > d->max_dot ? d->max_dot : dot;
    dot = dot * cells > WIDTH ? WIDTH / cells : dot;   // the largest code is 177 squares: 3 dots each still fit
    int left = (WIDTH - side * dot) / 2, top = d->y + d->quiet * dot;
    if (dot < 1 || d->y + cells * dot > d->canvas->max_rows) {
        return;
    }
    for (int my = 0; my < side; my++) {
        uint8_t *line = d->canvas->bits + (size_t)(top + my * dot) * PRINTER_WIDTH_BYTES;
        for (int mx = 0; mx < side; mx++) {
            if (esp_qrcode_get_module(code, mx, my)) {
                for (int x = left + mx * dot; x < left + (mx + 1) * dot; x++) {
                    line[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
                }
            }
        }
        for (int k = 1; k < dot; k++) {   // every row of dots in a row of squares is the same
            memcpy(line + (size_t)k * PRINTER_WIDTH_BYTES, line, PRINTER_WIDTH_BYTES);
        }
    }
    d->height = cells * dot;
}

/* Make the code and draw it. Returns the rows used, or 0. */
static int draw(draw_t *d, const char *text)
{
    static bool quiet_log;
    if (!quiet_log) {
        // The library logs the text of every code it makes. A code can hold a Wi-Fi password or a pairing link.
        esp_log_level_set("QRCODE", ESP_LOG_WARN);
        quiet_log = true;
    }
    esp_qrcode_config_t cfg = {
        .display_func_with_cb = on_code,
        .max_qrcode_version = 40,                 // the largest there is: 177 x 177 squares
        .qrcode_ecc_level = ESP_QRCODE_ECC_MED,   // always medium, see qr_ticket.h
        .user_data = d,
    };
    d->height = 0;
    return esp_qrcode_generate(&cfg, text) == ESP_OK ? d->height : 0;
}

int qr_draw(ticket_canvas_t *canvas, const char *text, int y, int room, int max_dot)
{
    draw_t d = { .canvas = canvas, .y = y, .room = room, .min_dot = 1, .max_dot = max_dot, .quiet = 0 };
    return draw(&d, text);
}

bool qr_size_from_name(const char *name, qr_size_t *size)
{
    static const char *const names[] = { "small", "medium", "full" };
    for (int i = 0; i < 3; i++) {
        if (strcmp(name, names[i]) == 0) {
            *size = (qr_size_t)i;
            return true;
        }
    }
    return false;
}

/* Draw text in the middle of the paper, breaking it between words, and move *y below the last line. */
static void draw_centred(ticket_canvas_t *c, const ticket_face_t *face, int *y, const char *text)
{
    const char *line = NULL, *line_end = NULL;
    int line_w = 0, space = ticket_text_width(face, " ", 1);
    for (const char *p = text;;) {
        while (*p == ' ') {
            p++;
        }
        const char *word = p;
        while (*p && *p != ' ') {
            p++;
        }
        bool done = p == word;
        int w = done ? 0 : ticket_text_width(face, word, (size_t)(p - word));
        if (line && (done || line_w + space + w > WIDTH)) {
            ticket_draw_text(c, face, line_w < WIDTH ? (WIDTH - line_w) / 2 : 0, *y, line, (size_t)(line_end - line));
            *y += ticket_face_line(face);
            line = NULL;
        }
        if (done) {
            return;
        }
        line = line ? line : word;
        line_end = p;
        line_w = ticket_text_width(face, line, (size_t)(line_end - line));
    }
}

esp_err_t qr_ticket_render(const qr_job_t *qr, const uint8_t *above, int above_rows, bool with_header,
                           ticket_canvas_t *out, bool *lettered)
{
    static const int room[] = { WIDTH / 3, WIDTH * 2 / 3, WIDTH };
    if (!ticket_canvas_init(out, above_rows + QR_BLOCK_ROWS)) {
        return ESP_ERR_NO_MEM;
    }
    if (above_rows > 0) {
        memcpy(out->bits, above, (size_t)above_rows * PRINTER_WIDTH_BYTES);
    }
    settings_t settings;
    settings_get(&settings);
    int font = ticket_font_id(settings.header_font);
    font = font >= 0 ? font : ticket_font_id("Inter");
    const ticket_face_t *text = font >= 0 ? ticket_face(font, false, CAPTION_SIZE) : NULL;
    int y = above_rows;
    *lettered = text != NULL;
    if (with_header && text) {
        int below = ticket_header_draw(out, font, y + 2);
        *lettered = below > 0;
        y = below > 0 ? below : y;
    }

    draw_t d = { .canvas = out, .y = y, .room = room[qr->size], .min_dot = MIN_DOT, .max_dot = WIDTH, .quiet = QUIET };
    int height = draw(&d, qr->data);
    if (height == 0) {
        ticket_canvas_free(out);
        return ESP_ERR_INVALID_SIZE;   // with this much room, the only way to fail is a text no QR code can hold
    }
    y += height;

    if (text && qr->caption[0]) {
        char caption[QR_CAPTION_MAX + 1];
        strlcpy(caption, qr->caption, sizeof(caption));
        for (char *p = caption; *p; p++) {
            *p = (unsigned char)*p < 0x20 ? ' ' : *p;   // a line break is a space
        }
        draw_centred(out, text, &y, caption);
        y += 12;
    }
    out->rows = y > out->max_rows ? out->max_rows : y;
    return ESP_OK;
}
