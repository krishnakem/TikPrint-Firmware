/* Draws QR tickets with the board's own code (main/qr_ticket.c) and checks its limits. See README.txt. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "printer.h"
#include "qr_ticket.h"
#include "settings.h"
#include "ticket_header.h"

/* Stand-ins for the parts of the board qr_ticket.c leans on. */
void settings_get(settings_t *out) { memset(out, 0, sizeof(*out)); strcpy(out->header_font, "Inter"); }
int ticket_header_draw(ticket_canvas_t *c, int font, int y)
{
    const ticket_face_t *f = ticket_face(font, true, 20);
    ticket_draw_text(c, f, 0, y, "TikPrint | Ann Arbor, MI", 24);
    ticket_draw_right(c, f, PRINTER_WIDTH_DOTS, y, "NOV 23, 2026 | 8:00 AM EST", 26);
    return y + ticket_face_line(f) + 8;
}

static int failures;
#define CHECK(what, ok) do { if (!(ok)) { printf("FAILED: %s\n", what); failures++; } else { printf("ok: %s\n", what); } } while (0)

static void save(const char *dir, const char *name, const ticket_canvas_t *c)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/qr_%s.pbm", dir, name);
    FILE *f = fopen(path, "wb");
    fprintf(f, "P4\n%d %d\n", PRINTER_WIDTH_DOTS, c->rows);
    fwrite(c->bits, PRINTER_WIDTH_BYTES, (size_t)c->rows, f);
    fclose(f);
}

/* The leftmost and rightmost black dot of the rows from `from` on. */
static void span(const ticket_canvas_t *c, int from, int to, int *left, int *right)
{
    *left = PRINTER_WIDTH_DOTS; *right = -1;
    for (int y = from; y < to; y++) {
        for (int x = 0; x < PRINTER_WIDTH_DOTS; x++) {
            if (c->bits[y * PRINTER_WIDTH_BYTES + (x >> 3)] & (0x80 >> (x & 7))) {
                if (x < *left) *left = x;
                if (x > *right) *right = x;
            }
        }
    }
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    const char *url = "https://example.com/menu?table=12";
    const char *names[] = { "small", "medium", "full" };
    ticket_canvas_t c;
    bool lettered;
    int widths[3];

    for (int i = 0; i < 3; i++) {
        qr_size_t size;
        CHECK("the size has a name", qr_size_from_name(names[i], &size) && (int)size == i);
        qr_job_t job = { .data = url, .size = size, .caption = i == 1 ? "Our menu: scan to order from table 12, or ask at the counter for a paper copy" : "" };
        CHECK(names[i], qr_ticket_render(&job, NULL, 0, true, &c, &lettered) == ESP_OK && lettered && c.rows > 0 && c.rows <= QR_BLOCK_ROWS);
        int l, r, cap = i == 1 ? 80 : 0;
        span(&c, 40, c.rows - cap, &l, &r);
        widths[i] = r - l + 1;
        CHECK("the code is in the middle of the paper", abs(l - (PRINTER_WIDTH_DOTS - 1 - r)) <= 1);
        save(dir, names[i], &c);
        ticket_canvas_free(&c);
    }
    printf("code widths: %d, %d, %d dots\n", widths[0], widths[1], widths[2]);
    CHECK("small < medium < full", widths[0] < widths[1] && widths[1] < widths[2]);
    CHECK("full leaves the white edge", widths[2] <= PRINTER_WIDTH_DOTS - 2 * 4 * 3);
    CHECK("no such size", !qr_size_from_name("huge", &(qr_size_t){ 0 }));

    /* Wi-Fi, as the page builds it. */
    qr_job_t wifi = { .data = "WIFI:S:Lake\\;house;T:WPA;P:pa\\\\ss\\:word;;", .size = QR_SIZE_MEDIUM, .caption = "Wi-Fi: Lake;house" };
    CHECK("wifi", qr_ticket_render(&wifi, NULL, 0, true, &c, &lettered) == ESP_OK);
    save(dir, "wifi", &c);
    ticket_canvas_free(&c);

    /* Under a picture: the picture's rows stay as they are, the code comes below, and no header is drawn. */
    uint8_t above[50 * PRINTER_WIDTH_BYTES];
    memset(above, 0xAA, sizeof(above));
    qr_job_t under = { .data = url, .size = QR_SIZE_SMALL, .caption = "Fix the feed" };
    CHECK("under a picture", qr_ticket_render(&under, above, 50, false, &c, &lettered) == ESP_OK && c.rows > 50 &&
          memcmp(c.bits, above, sizeof(above)) == 0);
    int blank = 1;
    for (int i = 0; i < 4 * 3 * PRINTER_WIDTH_BYTES; i++) blank &= c.bits[50 * PRINTER_WIDTH_BYTES + i] == 0;
    CHECK("white edge between the picture and the code", blank);
    save(dir, "under", &c);
    ticket_canvas_free(&c);

    /* The limits: 2331 bytes is the most a QR code holds at medium error correction. */
    static char big[3001];
    memset(big, 'a', sizeof(big) - 1);
    big[2331] = 0;
    qr_job_t most = { .data = big, .size = QR_SIZE_SMALL, .caption = "" };
    CHECK("2331 bytes fit", qr_ticket_render(&most, NULL, 0, true, &c, &lettered) == ESP_OK);
    int l, r;
    span(&c, 40, c.rows, &l, &r);
    CHECK("the largest code still has 3-dot squares and its white edge", r - l + 1 == 177 * 3);
    save(dir, "largest", &c);
    ticket_canvas_free(&c);
    big[2331] = 'a'; big[2332] = 0;
    CHECK("2332 bytes are too long", qr_ticket_render(&most, NULL, 0, true, &c, &lettered) == ESP_ERR_INVALID_SIZE && c.bits == NULL);

    /* The pairing ticket's code: no white edge added, never wider than asked. */
    ticket_canvas_init(&c, 600);
    int h = qr_draw(&c, "https://tikprint-cloud.vercel.app/link?code=K7QM-3XPD", 10, 432, 12);
    c.rows = 10 + h;
    span(&c, 0, c.rows, &l, &r);
    CHECK("pairing code", h > 0 && h <= 432 && r - l + 1 == h);
    ticket_canvas_free(&c);

    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
