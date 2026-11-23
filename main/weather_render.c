/*
 * Drawing a weather ticket in a chosen font (see weather_render.h).
 *
 * Top to bottom:
 *   TikPrint | Cupertino, CA              NOV 21, 2026 | 1:49 PM PST     header line, small and bold
 *   WEATHER · CUPERTINO, CA                                               source line, small and bold
 *   (symbol)
 *   Today                                                                 title, big and bold
 *   NEXT HOURS                                                            a heading, bold
 *   2 PM   53°    0%  Mostly Cloudy                                       hours, in columns
 *   THIS AFTERNOON                                 54°F  Precip 23%       a heading with text at the right edge
 *   Slight Chance Light Rain                                              plain text, broken to fit
 */
#include <string.h>

#include "printer.h"
#include "ticket_header.h"
#include "weather_icons.h"
#include "weather_render.h"

#define WIDTH       PRINTER_WIDTH_DOTS
#define HEADER_SIZE 20   // header and source line; HEADER_SMALL when the header does not fit in one line
#define HEADER_SMALL 17
static const int TEXT_SIZE[3]  = { 22, 25, 29 };   // small, medium, large (sizes made by tools/make_ticket_fonts.py)
static const int TITLE_SIZE[3] = { 44, 52, 60 };

/* How many bytes of text fit in `room` dots (whole characters only). */
static size_t fitting(const ticket_face_t *face, const char *text, size_t len, int room)
{
    size_t n = 0;
    while (n < len) {
        size_t next = n + 1;
        while (next < len && ((unsigned char)text[next] & 0xC0) == 0x80) {
            next++;   // the rest of a character stored as several bytes
        }
        if (ticket_text_width(face, text, next) > room) {
            break;
        }
        n = next;
    }
    return n;
}

bool weather_render(ticket_canvas_t *c, int font, int size, int header_font, const char *source,
                    weather_icon_t icon, const char *title, const char *rich_body)
{
    size = size < 0 ? 0 : size > 2 ? 2 : size;
    const ticket_face_t *text = ticket_face(font, false, TEXT_SIZE[size]);
    const ticket_face_t *bold = ticket_face(font, true, TEXT_SIZE[size]);
    const ticket_face_t *big = ticket_face(font, true, TITLE_SIZE[size]);
    const ticket_face_t *small = ticket_face(font, true, HEADER_SIZE);
    const ticket_face_t *head = ticket_face(header_font, true, HEADER_SIZE);
    const ticket_face_t *head_small = ticket_face(header_font, true, HEADER_SMALL);
    if (!text || !bold || !big || !small || !head || !head_small || c->max_rows < WEATHER_RENDER_MAX_ROWS) {
        return false;
    }
    int y = 2;

    // Header line: left half at the left edge, right half (in capitals, as the page draws it) at the right edge.
    // No location in it: a weather ticket already says which place it is for.
    char left[TICKET_HEADER_MAX], right[TICKET_HEADER_MAX];
    ticket_header_text(left, right, false);
    for (char *p = right; *p; p++) {
        *p = (*p >= 'a' && *p <= 'z') ? (char)(*p - 'a' + 'A') : *p;
    }
    if (ticket_text_width(head, left, strlen(left)) + ticket_text_width(head, right, strlen(right)) + HEADER_SIZE > WIDTH) {
        head = head_small;
    }
    ticket_draw_text(c, head, 0, y, left, strlen(left));
    ticket_draw_right(c, head, WIDTH, y, right, strlen(right));
    y += ticket_face_line(head) + 8;

    if (source[0]) {
        ticket_draw_wrapped(c, small, 0, &y, source, strlen(source));
        y += 6;
    }
    if (icon != WEATHER_ICON_NONE) {
        weather_icon_strip(icon, c->bits + (size_t)y * PRINTER_WIDTH_BYTES);   // the strip is as wide as the canvas
        y += WEATHER_ICON_STRIP_ROWS + 8;
    }
    if (title[0]) {
        ticket_draw_wrapped(c, big, 0, &y, title, strlen(title));
        y += 6;
    }

    // Columns of the hourly list: hour, temperature and chance end at these x; the words start after them.
    int gap = TEXT_SIZE[size] * 2 / 3;
    int hour_end = ticket_text_width(text, "12 PM", 5);
    int temp_end = hour_end + gap + ticket_text_width(text, "100\xC2\xB0", 5);
    int chance_end = temp_end + gap + ticket_text_width(text, "100%", 4);
    int line = ticket_face_line(text);

    for (const char *p = rich_body; *p && y + 2 * line < WEATHER_RENDER_MAX_ROWS;) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len == 0) {
            y += line / 2;                                    // an empty line: half a line of air
        } else if (*p == WEATHER_MARK_HOUR[0]) {
            const char *cell[4] = { p + 1, NULL, NULL, NULL };
            size_t cell_len[4] = { 0 };
            int cells = 1;
            for (const char *q = p + 1; q < p + len; q++) {
                if (*q == '\t' && cells < 4) {
                    cell_len[cells - 1] = (size_t)(q - cell[cells - 1]);
                    cell[cells++] = q + 1;
                }
            }
            cell_len[cells - 1] = (size_t)(p + len - cell[cells - 1]);
            const int ends[3] = { hour_end, temp_end, chance_end };
            for (int i = 0; i < cells && i < 3; i++) {
                ticket_draw_right(c, text, ends[i], y, cell[i], cell_len[i]);
            }
            if (cells == 4) {
                int x = chance_end + gap;
                ticket_draw_text(c, text, x, y, cell[3], fitting(text, cell[3], cell_len[3], WIDTH - x));
            }
            y += line;
        } else if (*p == WEATHER_MARK_LABEL[0]) {
            const char *tab = memchr(p + 1, '\t', len - 1);
            size_t left_len = tab ? (size_t)(tab - p - 1) : len - 1;
            size_t right_len = tab ? (size_t)(p + len - tab - 1) : 0;
            int left_w = ticket_text_width(bold, p + 1, left_len);
            int right_w = tab ? ticket_text_width(text, tab + 1, right_len) : 0;
            if (tab && left_w + gap + right_w <= WIDTH) {
                ticket_draw_right(c, text, WIDTH, y, tab + 1, right_len);   // both fit on one line
                right_len = 0;
            }
            ticket_draw_wrapped(c, bold, 0, &y, p + 1, left_len);
            if (right_len) {
                ticket_draw_wrapped(c, text, 0, &y, tab + 1, right_len);
            }
        } else {
            int indent = 0;
            while ((size_t)indent < len && p[indent] == ' ') {
                indent++;
            }
            ticket_draw_wrapped(c, text, indent * ticket_text_width(text, " ", 1), &y, p, len);
        }
        p += len + (eol ? 1 : 0);
    }
    c->rows = y + 8;
    return true;
}
