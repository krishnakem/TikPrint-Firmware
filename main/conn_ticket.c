/*
 * Turning a connector's list into a ticket (see conn_ticket.h).
 */
#include <stdio.h>
#include <string.h>

#include "conn_ticket.h"
#include "printer.h"
#include "ticket_header.h"

#define WIDTH        PRINTER_WIDTH_DOTS
#define HEADER_SIZE  20   // header and source line; HEADER_SMALL when the header does not fit in one line
#define HEADER_SMALL 17
#define NOTE_SIZE    17   // the small note under an item
static const int TEXT_SIZE[3]  = { 22, 25, 29 };   // small, medium, large: the same sizes as weather tickets
static const int TITLE_SIZE[3] = { 44, 52, 60 };

/* ---------- Making text printable ---------- */

/* Letters with accents, and what they become. One table per block of Unicode. */
static const char LATIN_1[] =        // U+00C0 .. U+00FF
    "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYTs" "aaaaaaaceeeeiiii" "dnooooo/ouuuuyty";
static const char LATIN_A[] =        // U+0100 .. U+017F
    "AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg" "GgGgHhHhIiIiIiIi" "IiIiJjKkkLlLlLlL"
    "lLlNnNnNnnNnOoOo" "OoOoRrRrRrSsSsSs" "SsTtTtTtUuUuUuUu" "UuUuWwYyYZzZzZzs";

/* Read one character of UTF-8 text and move *p past it. */
static unsigned next_char(const unsigned char **p)
{
    unsigned c = *(*p)++;
    int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
    c = extra == 1 ? c & 0x1F : extra == 2 ? c & 0x0F : extra == 3 ? c & 0x07 : c;
    while (extra-- > 0 && (**p & 0xC0) == 0x80) {
        c = (c << 6) | (*(*p)++ & 0x3F);
    }
    return c;
}

/* The plain character to print for c, or 0 to leave it out. */
static char plain_char(unsigned c)
{
    if (c >= 0x20 && c < 0x7F) {
        return (char)c;
    }
    if (c >= 0xC0 && c <= 0xFF) {
        return LATIN_1[c - 0xC0];
    }
    if (c >= 0x100 && c <= 0x17F) {
        return LATIN_A[c - 0x100];
    }
    switch (c) {
    case '\t': case '\n': case '\r': case 0xA0: return ' ';
    case 0x2018: case 0x2019: return '\'';
    case 0x201C: case 0x201D: return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: return '-';
    case 0x2026: return '.';     // "…" becomes one dot
    case 0x2022: case 0xB7: return '-';
    default: return 0;           // emoji, other alphabets: the board has no picture of them
    }
}

void conn_clean_text(char *out, size_t size, const char *in)
{
    const unsigned char *p = (const unsigned char *)in;
    size_t n = 0;
    while (*p && n + 1 < size) {
        // Markdown link "[words](address)": keep the words.
        if (*p == '[') {
            const char *close = strstr((const char *)p, "](");
            const char *end = close ? strchr(close, ')') : NULL;
            const char *other = strchr((const char *)p + 1, '[');
            if (close && end && (!other || other > close)) {
                char words[CONN_ITEM_TEXT_MAX];
                size_t len = (size_t)(close - (const char *)p - 1);
                len = len < sizeof(words) ? len : sizeof(words) - 1;
                memcpy(words, p + 1, len);
                words[len] = 0;
                conn_clean_text(out + n, size - n, words);
                n += strlen(out + n);
                p = (const unsigned char *)end + 1;
                continue;
            }
        }
        // Markdown marks for bold, italics and code.
        if ((p[0] == '*' && p[1] == '*') || (p[0] == '_' && p[1] == '_')) {
            p += 2;
            continue;
        }
        if (*p == '`') {
            p++;
            continue;
        }
        char c = plain_char(next_char(&p));
        if (!c || (c == ' ' && (n == 0 || out[n - 1] == ' '))) {
            continue;   // nothing to print, or a second space in a row
        }
        out[n++] = c;
    }
    while (n > 0 && out[n - 1] == ' ') {
        n--;
    }
    out[n] = 0;
}

/* ---------- The ticket ---------- */

void conn_ticket_title(char *out, size_t size, const connector_t *connector, const conn_list_t *list)
{
    if (list->total == 0) {
        strlcpy(out, connector->nothing, size);
    } else {
        snprintf(out, size, "%d %s", list->total, list->total == 1 ? connector->one : connector->many);
    }
}

/* Add text to out, broken between words into lines of at most PRINTER_TEXT_COLS; every line starts with `lead`
 * (the first with `first`, which is as long). */
static void add_wrapped(char *out, size_t size, const char *first, const char *lead, const char *text)
{
    size_t n = strlen(out), col = 0;
    const char *p = text;
#define PUT(s) do { n += strlcpy(out + n, (s), size - n); if (n >= size) { return; } } while (0)
    PUT(first);
    col = strlen(first);
    while (*p) {
        while (*p == ' ') {
            p++;
        }
        size_t len = strcspn(p, " ");
        if (len == 0) {
            break;
        }
        if (col > strlen(lead) && col + 1 + len > PRINTER_TEXT_COLS) {
            PUT("\n");
            PUT(lead);
            col = strlen(lead);
        } else if (col > strlen(lead)) {
            PUT(" ");
            col++;
        }
        char word[PRINTER_TEXT_COLS + 1];
        size_t take = len < PRINTER_TEXT_COLS - strlen(lead) ? len : PRINTER_TEXT_COLS - strlen(lead);   // a very long word is cut
        memcpy(word, p, take);
        word[take] = 0;
        PUT(word);
        col += take;
        p += len;
    }
    PUT("\n");
#undef PUT
}

void conn_ticket_text(char *out, size_t size, const conn_list_t *list)
{
    out[0] = 0;
    for (int i = 0; i < list->count; i++) {
        add_wrapped(out, size, "[ ] ", "    ", list->item[i].text);
        if (list->item[i].note[0]) {
            add_wrapped(out, size, "    ", "    ", list->item[i].note);
        }
    }
    if (list->total > list->count) {
        char more[24];
        snprintf(more, sizeof(more), "+ %d more", list->total - list->count);
        add_wrapped(out, size, "", "", more);
    }
}

/* Draw an empty box to tick: its top-left corner at (x, y), `side` dots wide and high, lines 2 dots thick. */
static void draw_box(ticket_canvas_t *c, int x, int y, int side)
{
    for (int r = 0; r < side; r++) {
        int cy = y + r;
        if (cy < 0 || cy >= c->max_rows) {
            continue;
        }
        uint8_t *line = c->bits + (size_t)cy * PRINTER_WIDTH_BYTES;
        bool edge_row = r < 2 || r >= side - 2;
        for (int k = 0; k < side; k++) {
            int cx = x + k;
            if ((edge_row || k < 2 || k >= side - 2) && cx >= 0 && cx < WIDTH) {
                line[cx >> 3] |= (uint8_t)(0x80 >> (cx & 7));
            }
        }
    }
}

bool conn_ticket_render(ticket_canvas_t *c, int font, int size, int header_font, const char *source,
                        const char *title, const conn_list_t *list)
{
    size = size < 0 ? 0 : size > 2 ? 2 : size;
    const ticket_face_t *text = ticket_face(font, false, TEXT_SIZE[size]);
    const ticket_face_t *big = ticket_face(font, true, TITLE_SIZE[size]);
    const ticket_face_t *small = ticket_face(font, true, HEADER_SIZE);
    const ticket_face_t *note = ticket_face(font, true, NOTE_SIZE);
    const ticket_face_t *head = ticket_face(header_font, true, HEADER_SIZE);
    const ticket_face_t *head_small = ticket_face(header_font, true, HEADER_SMALL);
    if (!text || !big || !small || !note || !head || !head_small || c->max_rows < CONN_TICKET_MAX_ROWS) {
        return false;
    }
    int y = 2;

    // Header line: left half at the left edge, right half (in capitals, as the page draws it) at the right edge.
    char left[TICKET_HEADER_MAX], right[TICKET_HEADER_MAX];
    ticket_header_text(left, right, true);
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
    if (title[0]) {
        ticket_draw_wrapped(c, big, 0, &y, title, strlen(title));
        y += 10;
    }

    // The items: a box to tick, then the text beside it (further lines stay beside the box, not under it).
    int line = ticket_face_line(text);
    int box = TEXT_SIZE[size] * 3 / 4;
    int indent = box + TEXT_SIZE[size] / 2;
    for (int i = 0; i < list->count && y + 3 * line < CONN_TICKET_MAX_ROWS; i++) {
        const conn_item_t *item = &list->item[i];
        draw_box(c, 0, y + (line - box) / 2, box);
        ticket_draw_wrapped(c, text, indent, &y, item->text, strlen(item->text));
        if (item->note[0]) {
            ticket_draw_wrapped(c, note, indent, &y, item->note, strlen(item->note));
        }
        y += line / 3;
    }
    if (list->total > list->count) {
        char more[24];
        snprintf(more, sizeof(more), "+ %d more", list->total - list->count);
        ticket_draw_wrapped(c, text, indent, &y, more, strlen(more));
    }
    c->rows = y + 8;
    return true;
}
