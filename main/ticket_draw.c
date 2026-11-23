/*
 * Drawing text as dots (see ticket_draw.h).
 *
 * The letters come from data/ticket_fonts.bin, which is part of the firmware and read
 * straight from flash. Its layout is described in tools/make_ticket_fonts.py: a list of
 * faces, and for each face a list of 97 letters (space to ~, the degree sign, the middle
 * dot) and their dots.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "printer.h"
#include "ticket_draw.h"

extern const uint8_t fonts_start[] asm("_binary_ticket_fonts_bin_start");
extern const uint8_t fonts_end[]   asm("_binary_ticket_fonts_bin_end");

#define FILE_HEAD   6    // "TKF1" + the number of faces
#define FACE_BYTES  16
#define GLYPH_BYTES 8
#define GLYPHS      97

/* The order must match FONTS in tools/make_ticket_fonts.py. The names are the ones the page uses. */
static const char *const FONT_NAMES[] = { "Inter", "Source Serif", "Nunito", "Caveat", "JetBrains Mono" };

/* A face's entry in the file, as it lies in flash (read byte by byte: it is not aligned). */
struct ticket_face {
    uint8_t font, bold, size, glyphs, ascent, line, unused[2];
    uint8_t glyphs_at[4], dots_at[4];
};
_Static_assert(sizeof(struct ticket_face) == FACE_BYTES, "a face entry must be exactly 16 bytes");

static uint32_t number32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int ticket_font_id(const char *name)
{
    for (size_t i = 0; i < sizeof(FONT_NAMES) / sizeof(FONT_NAMES[0]); i++) {
        if (strcmp(name, FONT_NAMES[i]) == 0) {
            return (int)i;
        }
    }
    return -1;
}

const ticket_face_t *ticket_face(int font, bool bold, int size)
{
    if (fonts_end - fonts_start < FILE_HEAD || memcmp(fonts_start, "TKF1", 4) != 0) {
        return NULL;
    }
    int count = fonts_start[4] | fonts_start[5] << 8;
    for (int i = 0; i < count; i++) {
        const ticket_face_t *face = (const ticket_face_t *)(fonts_start + FILE_HEAD + (size_t)i * FACE_BYTES);
        if (face->font == font && face->bold == (bold ? 1 : 0) && face->size == size && face->glyphs == GLYPHS) {
            return face;
        }
    }
    return NULL;
}

int ticket_face_line(const ticket_face_t *face)
{
    return face->line;
}

/* Read one character of UTF-8 text and move *p past it. */
static uint32_t next_char(const unsigned char **p, const unsigned char *end)
{
    uint32_t c = *(*p)++;
    int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
    c = extra == 1 ? c & 0x1F : extra == 2 ? c & 0x0F : extra == 3 ? c & 0x07 : c;
    while (extra-- > 0 && *p < end && (**p & 0xC0) == 0x80) {
        c = (c << 6) | (*(*p)++ & 0x3F);
    }
    return c;
}

/* A letter's 8 bytes in the file. Characters the fonts were not made with become their closest plain one. */
static const uint8_t *glyph_of(const ticket_face_t *face, uint32_t c)
{
    int index;
    switch (c) {
    case 0x00B0: index = 95; break;                         // degree sign
    case 0x00B7: case 0x2022: index = 96; break;            // middle dot, bullet
    case 0x2018: case 0x2019: index = '\'' - 32; break;     // curly quotes
    case 0x201C: case 0x201D: index = '"' - 32; break;
    case 0x2013: case 0x2014: index = '-' - 32; break;      // dashes
    default:     index = (c >= 32 && c < 127) ? (int)c - 32 : '?' - 32;
    }
    return fonts_start + number32(face->glyphs_at) + (size_t)index * GLYPH_BYTES;
}

int ticket_text_width(const ticket_face_t *face, const char *text, size_t len)
{
    const unsigned char *p = (const unsigned char *)text, *end = p + len;
    int width = 0;
    while (p < end && *p) {
        width += glyph_of(face, next_char(&p, end))[0];
    }
    return width;
}

bool ticket_canvas_init(ticket_canvas_t *canvas, int max_rows)
{
    canvas->bits = heap_caps_calloc((size_t)max_rows, PRINTER_WIDTH_BYTES, MALLOC_CAP_SPIRAM);
    canvas->rows = 0;
    canvas->max_rows = canvas->bits ? max_rows : 0;
    return canvas->bits != NULL;
}

void ticket_canvas_free(ticket_canvas_t *canvas)
{
    free(canvas->bits);
    canvas->bits = NULL;
    canvas->rows = canvas->max_rows = 0;
}

int ticket_draw_text(ticket_canvas_t *canvas, const ticket_face_t *face, int x, int y, const char *text, size_t len)
{
    const unsigned char *p = (const unsigned char *)text, *end = p + len;
    const uint8_t *dots = fonts_start + number32(face->dots_at);
    while (p < end && *p) {
        // advance, width, height, x offset, top, unused, then where its dots start (2 bytes)
        const uint8_t *g = glyph_of(face, next_char(&p, end));
        int w = g[1], h = g[2], left = x + (int8_t)g[3], top = y + (int8_t)g[4];
        const uint8_t *rows = dots + (g[6] | g[7] << 8);
        int row_bytes = (w + 7) / 8;
        for (int r = 0; r < h; r++) {
            int cy = top + r;
            if (cy < 0 || cy >= canvas->max_rows) {
                continue;
            }
            uint8_t *line = canvas->bits + (size_t)cy * PRINTER_WIDTH_BYTES;
            for (int c = 0; c < w; c++) {
                int cx = left + c;
                if ((rows[r * row_bytes + (c >> 3)] & (0x80 >> (c & 7))) && cx >= 0 && cx < PRINTER_WIDTH_DOTS) {
                    line[cx >> 3] |= (uint8_t)(0x80 >> (cx & 7));
                }
            }
        }
        x += g[0];
    }
    return x;
}

void ticket_draw_wrapped(ticket_canvas_t *c, const ticket_face_t *face, int x, int *y, const char *text, size_t len)
{
    const char *p = text, *end = text + len;
    int space = ticket_text_width(face, " ", 1), pen = x;
    while (p < end) {
        while (p < end && *p == ' ') {
            p++;
        }
        const char *word = p;
        while (p < end && *p != ' ') {
            p++;
        }
        if (p == word) {
            break;
        }
        int w = ticket_text_width(face, word, (size_t)(p - word));
        if (pen > x && pen + space + w > PRINTER_WIDTH_DOTS) {
            *y += ticket_face_line(face);
            pen = x;
        }
        if (pen > x) {
            pen += space;
        }
        pen = ticket_draw_text(c, face, pen, *y, word, (size_t)(p - word));
    }
    *y += ticket_face_line(face);
}

void ticket_draw_right(ticket_canvas_t *c, const ticket_face_t *face, int x_right, int y, const char *text, size_t len)
{
    ticket_draw_text(c, face, x_right - ticket_text_width(face, text, len), y, text, len);
}
