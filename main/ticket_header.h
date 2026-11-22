#pragma once

#include <stdbool.h>
#include <stddef.h>

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
