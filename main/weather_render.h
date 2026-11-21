#pragma once

#include <stdbool.h>

#include "ticket_draw.h"
#include "weather_ticket.h"

#define WEATHER_RENDER_MAX_ROWS 3000   // tallest weather ticket drawn (37 cm of paper): more is cut off

/**
 * Draw a whole weather ticket (header line, source line, symbol, title, forecast) as dots,
 * in the font and size chosen on the page.
 *
 *   font         number of the font (ticket_font_id)
 *   size         0 = small, 1 = medium, 2 = large
 *   header_font  number of the font for the header line at the very top
 *   source       "WEATHER · ANN ARBOR, MI"
 *   title, rich_body   from weather_ticket_build() with rich = true
 *
 * canvas must be blank with room for WEATHER_RENDER_MAX_ROWS rows; canvas->rows is set to the
 * ticket's height. False if the board has no letters for that font (print in the built-in font instead).
 */
bool weather_render(ticket_canvas_t *canvas, int font, int size, int header_font, const char *source,
                    weather_icon_t icon, const char *title, const char *rich_body);
