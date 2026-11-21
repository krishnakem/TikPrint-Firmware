/*
 * The weather symbols, as dots. See weather_icons.h.
 *
 * data/weather_icons.bin holds the symbols one after another, each WEATHER_ICON_SIZE x
 * WEATHER_ICON_SIZE dots, 1 bit per dot, in the order of weather_icon_t. It is embedded in
 * the firmware like the fonts are, so it is read straight from flash.
 */
#include <string.h>

#include "weather_icons.h"

extern const uint8_t icons_start[] asm("_binary_weather_icons_bin_start");
extern const uint8_t icons_end[]   asm("_binary_weather_icons_bin_end");

#define ICON_ROW_BYTES (WEATHER_ICON_SIZE / 8)
#define ICON_BYTES     (WEATHER_ICON_SIZE * ICON_ROW_BYTES)

void weather_icon_strip(weather_icon_t icon, uint8_t *strip)
{
    memset(strip, 0, WEATHER_ICON_STRIP_BYTES);   // all white
    if (icon < 0 || (size_t)(icon + 1) * ICON_BYTES > (size_t)(icons_end - icons_start)) {
        return;
    }
    const uint8_t *bits = icons_start + (size_t)icon * ICON_BYTES;
    for (int y = 0; y < WEATHER_ICON_SIZE; y++) {
        // The symbol starts at the paper's left edge, which is also the start of a byte: copy row by row.
        memcpy(strip + (size_t)y * PRINTER_WIDTH_BYTES, bits + (size_t)y * ICON_ROW_BYTES, ICON_ROW_BYTES);
    }
}
