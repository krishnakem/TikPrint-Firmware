#pragma once

/*
 * REFERENCE ONLY: this file is not part of the firmware any more (connectors run in TikPrint Cloud
 * since firmware 0.4.1). It is kept, unchanged in what it draws, because the cloud's drift test
 * compares its own tickets with this code, dot for dot (tools/host_test/run.sh ticket ...).
 */

#include <stdbool.h>
#include <stddef.h>

#include "connectors.h"
#include "ticket_draw.h"

/*
 * Turning a connector's list into a ticket:
 *
 *   TikPrint | Ann Arbor, MI            NOV 21, 2026 | 8:00 AM EST     header line
 *   TODOIST · TODAY                                                     source line
 *   5 tasks                                                             title, big
 *   [ ] Call the dentist                                                one line per item, with a box to tick
 *       OVERDUE                                                         ... and its note, small
 *   [ ] Send the invoice to Sam
 *   + 2 more
 *
 * Nothing in this file touches the network or the printer.
 */

#define CONN_TICKET_MAX_ROWS 3000   // tallest ticket drawn: more is cut off
#define CONN_TICKET_BODY_MAX 2048   // room for the ticket as plain text
#define CONN_TICKET_TITLE_MAX 48

/**
 * Make text printable: the board's letters are plain English ones only, so accents are taken off
 * (é -> e), emoji and other characters it has no picture of are left out, Markdown marks are removed
 * ("[the site](https://...)" -> "the site", "**now**" -> "now") and runs of spaces become one.
 */
void conn_clean_text(char *out, size_t size, const char *in);

/** The title: "5 tasks", "1 task", or the connector's words for an empty list. */
void conn_ticket_title(char *out, size_t size, const connector_t *connector, const conn_list_t *list);

/** The list as plain text, for the printer's built-in font (lines of at most PRINTER_TEXT_COLS). */
void conn_ticket_text(char *out, size_t size, const conn_list_t *list);

/**
 * Draw the whole ticket as dots, in a chosen font. Same arguments as weather_render().
 * canvas must be blank with room for CONN_TICKET_MAX_ROWS rows. False if the board has no
 * letters for that font (print in the built-in font instead).
 */
bool conn_ticket_render(ticket_canvas_t *canvas, int font, int size, int header_font, const char *source,
                        const char *title, const conn_list_t *list);
