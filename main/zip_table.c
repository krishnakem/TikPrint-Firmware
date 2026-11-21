/*
 * ZIP code -> latitude/longitude, looked up in a table that is part of the firmware.
 *
 * The table (data/zip_table.bin) is embedded in the firmware image the same way the web
 * page and the fonts are, so it is read straight from flash and uses no RAM. It is sorted
 * by ZIP code, so finding one takes about 15 steps of "is it in the upper or lower half?"
 * (a binary search).
 *
 * Each record is 8 bytes (see tools/make_zip_table.py):
 *   4 bytes ZIP code as a number, 2 bytes latitude, 2 bytes longitude (both in 1/100 degree).
 */
#include <stdint.h>
#include <string.h>

#include "zip_table.h"

extern const uint8_t table_start[] asm("_binary_zip_table_bin_start");
extern const uint8_t table_end[]   asm("_binary_zip_table_bin_end");

#define RECORD_BYTES 8

typedef struct {
    uint32_t zip;
    int16_t lat_e2;
    int16_t lon_e2;
} record_t;

_Static_assert(sizeof(record_t) == RECORD_BYTES, "a record must be exactly 8 bytes");

int zip_table_count(void)
{
    return (int)((table_end - table_start) / RECORD_BYTES);
}

bool zip_table_lookup(const char *zip, int *lat_e2, int *lon_e2)
{
    if (!zip || strlen(zip) != 5) {
        return false;
    }
    uint32_t want = 0;
    for (const char *p = zip; *p; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        want = want * 10 + (uint32_t)(*p - '0');
    }

    int low = 0, high = zip_table_count() - 1;
    while (low <= high) {
        int mid = low + (high - low) / 2;
        record_t r;
        memcpy(&r, table_start + (size_t)mid * RECORD_BYTES, sizeof(r));   // copy out: flash data may not be aligned
        if (r.zip == want) {
            *lat_e2 = r.lat_e2;
            *lon_e2 = r.lon_e2;
            return true;
        }
        if (r.zip < want) {
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
    return false;
}
