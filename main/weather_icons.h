#pragma once

#include <stdint.h>

#include "printer.h"
#include "weather_ticket.h"

/* The weather symbols (sun, cloud, rain, ...) printed at the top of weather tickets.
 * They are small pictures built into the firmware (data/weather_icons.bin, drawn by
 * tools/make_weather_icons.py). Which symbol a forecast gets is decided in weather_ticket.c. */

#define WEATHER_ICON_SIZE        96                                         // dots per side (12 mm on paper)
#define WEATHER_ICON_STRIP_ROWS  WEATHER_ICON_SIZE                          // rows of the strip below
#define WEATHER_ICON_STRIP_BYTES (WEATHER_ICON_STRIP_ROWS * PRINTER_WIDTH_BYTES)

/**
 * Draw a symbol into a strip of paper: an image as wide as the paper and WEATHER_ICON_STRIP_ROWS
 * tall, white except for the symbol at its left edge. strip must have room for
 * WEATHER_ICON_STRIP_BYTES bytes. The strip can be printed as a printer_job_t image.
 */
void weather_icon_strip(weather_icon_t icon, uint8_t *strip);
