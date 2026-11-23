/*
 * Draw the ticket for an app's saved answer, on a computer (see README.txt).
 *   test_ticket <app> <reply.json> <out.pbm> [font] [size 0-2]
 * Prints the ticket as plain text too (what the printer's built-in font would print).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "conn_http.h"
#include "conn_ticket.h"
#include "conn_todoist.h"
#include "printer.h"
#include "ticket_header.h"

extern const connector_t conn_todoist;

/* Stand-ins for the parts of the board this test does not have. */
void ticket_header_text(char *left, char *right, bool with_location)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strcpy(left, with_location ? "TikPrint | Ann Arbor, MI" : "TikPrint");
    strftime(right, TICKET_HEADER_MAX, "%b %d, %Y | %I:%M %p %Z", &tm);
}
conn_result_t conn_http(const conn_request_t *request, char **reply)
{
    (void)request;
    *reply = NULL;
    return CONN_UNREACHABLE;   // this test never goes online
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: test_ticket <app> <reply.json> <out.pbm> [font] [size]\n");
        return 2;
    }
    FILE *f = fopen(argv[2], "rb");
    if (!f) {
        perror(argv[2]);
        return 1;
    }
    char *json = calloc(1, 1 << 20);
    fread(json, 1, (1 << 20) - 1, f);
    fclose(f);

    const connector_t *c = NULL;
    static conn_list_t list;
    conn_result_t res = CONN_BAD_ANSWER;
    if (strcmp(argv[1], "todoist") == 0) {
        c = &conn_todoist;
        res = conn_todoist_parse(json, (int64_t)time(NULL), &list);
    }
    if (!c) {
        fprintf(stderr, "no app called %s\n", argv[1]);
        return 2;
    }
    if (res != CONN_OK) {
        printf("could not read the answer: %s (%s)\n", conn_result_name(res), conn_result_message(res));
        return 1;
    }
    char title[CONN_TICKET_TITLE_MAX], body[CONN_TICKET_BODY_MAX];
    conn_ticket_title(title, sizeof(title), c, &list);
    conn_ticket_text(body, sizeof(body), &list);
    printf("%s\n%s\n%s", c->source, title, body);

    ticket_canvas_t canvas;
    int font = ticket_font_id(argc > 4 ? argv[4] : "Inter"), size = argc > 5 ? atoi(argv[5]) : 1;
    if (!ticket_canvas_init(&canvas, CONN_TICKET_MAX_ROWS) || !conn_ticket_render(&canvas, font, size, font, c->source, title, &list)) {
        fprintf(stderr, "could not draw the ticket\n");
        return 1;
    }
    f = fopen(argv[3], "wb");
    fprintf(f, "P4\n%d %d\n", PRINTER_WIDTH_DOTS, canvas.rows);
    fwrite(canvas.bits, PRINTER_WIDTH_BYTES, (size_t)canvas.rows, f);
    fclose(f);
    printf("\n(%d rows = %.1f cm of paper)\n", canvas.rows, canvas.rows / 80.0);
    return 0;
}

/* Names connectors.c would give; only the words are needed here. */
const char *conn_result_name(conn_result_t r) { return r == CONN_OK ? "ok" : "bad_answer"; }
const char *conn_result_message(conn_result_t r) { return r == CONN_OK ? "" : "The app answered with something the printer cannot read"; }
