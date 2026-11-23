#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "ticket_draw.h"

#define TICKET_HEADER_MAX 96   // room for either half of the header line, including the ending 0

/**
 * The two halves of the line at the top of every ticket, as the settings say:
 *   left   "TikPrint | Ann Arbor, MI"
 *   right  "Nov 20, 2026 | 10:05 PM EST"   ("" if date and time are both switched off or the clock is not set)
 * The location is the one typed in Settings, else the weather place, else the timezone's city.
 * Both need room for TICKET_HEADER_MAX bytes.
 * with_location = false leaves the location out whatever the settings say (left is then just "TikPrint"):
 * weather tickets do that, as they name their place themselves.
 */
void ticket_header_text(char *left, char *right, bool with_location);

/**
 * Draw that line as dots across the top of a ticket the board draws itself, with its top at y:
 * the left half at the left edge, the right half in capitals at the right edge, in the smaller of
 * the two header sizes if it would not fit otherwise. `font` is a ticket_font_id().
 * Returns the y below the line, or 0 if the board has no letters for that font.
 */
int ticket_header_draw(ticket_canvas_t *canvas, int font, int y);
