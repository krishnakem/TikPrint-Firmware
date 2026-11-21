#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Drawing text as dots, for tickets the board makes by itself in a font of your choice.
 *
 * A ticket typed on the page is drawn by the browser. A scheduled ticket prints with no
 * browser open, so the board draws the letters itself, from pictures of every letter made
 * ahead of time (data/ticket_fonts.bin, see tools/make_ticket_fonts.py).
 *
 * A "face" is one font in one weight at one size, e.g. Inter, bold, 52 dots tall.
 * Only the sizes that tool made exist.
 */

typedef struct ticket_face ticket_face_t;

/** The number of a font by the name the page uses ("Inter", "Source Serif", "Nunito", "Caveat",
 *  "JetBrains Mono"), or -1 if the board has no letters for it (e.g. "Printer", the built-in font). */
int ticket_font_id(const char *name);

/** A font at one weight and size, or NULL if it was not made at that size. */
const ticket_face_t *ticket_face(int font, bool bold, int size);

int ticket_face_line(const ticket_face_t *face);   // height of a line of text, in dots

/** How wide the first len bytes of text (UTF-8) are, in dots. */
int ticket_text_width(const ticket_face_t *face, const char *text, size_t len);

/** A strip of paper to draw on: as wide as the paper, 1 bit per dot (1 = black), top row first. */
typedef struct {
    uint8_t *bits;   // PRINTER_WIDTH_BYTES per row
    int rows;        // rows used so far: set it to the height of the finished ticket
    int max_rows;    // rows there is room for
} ticket_canvas_t;

/** Get a blank canvas (in the big external RAM). False if there is no memory. Give it back with ticket_canvas_free(). */
bool ticket_canvas_init(ticket_canvas_t *canvas, int max_rows);
void ticket_canvas_free(ticket_canvas_t *canvas);

/** Draw text with its top-left corner at (x, y). Dots outside the canvas are left out. Returns the x after the text. */
int ticket_draw_text(ticket_canvas_t *canvas, const ticket_face_t *face, int x, int y, const char *text, size_t len);
