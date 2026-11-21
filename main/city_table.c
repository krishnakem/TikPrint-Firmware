/*
 * Town name -> latitude/longitude, looked up in a table that is part of the firmware
 * (data/city_table.bin, made by tools/make_city_table.py from US Census data). Like the
 * ZIP code table it is read straight from flash and uses no RAM.
 *
 * The table is sorted by name, so the first town starting with what was typed is found
 * with a binary search; the ones after it are then read in order.
 *
 * File layout: 4 bytes count, then one 4-byte position per town, then the records:
 *   2 bytes latitude, 2 bytes longitude (1/100 degree), 2 bytes land area in square km,
 *   2 bytes state, 1 byte name length, the name.
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "city_table.h"

extern const uint8_t table_start[] asm("_binary_city_table_bin_start");
extern const uint8_t table_end[]   asm("_binary_city_table_bin_end");

#define RECORD_HEAD 9      // bytes of a record before the name
#define SCAN_MAX    4000   // towns looked at for one search ("s" alone matches thousands)
#define KEEP_MAX    8      // most matches one search can return

typedef struct {
    int16_t lat_e2, lon_e2;
    uint16_t area;
    char state[2];
    uint8_t name_len;
    const char *name;   // not 0-terminated
} record_t;

int city_table_count(void)
{
    uint32_t n;
    memcpy(&n, table_start, sizeof(n));   // copy out: flash data may not be aligned
    return (int)n;
}

static record_t record_at(int index)
{
    uint32_t at;
    memcpy(&at, table_start + 4 + 4 * (size_t)index, sizeof(at));
    const uint8_t *p = table_start + at;
    record_t r;
    memcpy(&r.lat_e2, p, 2);
    memcpy(&r.lon_e2, p + 2, 2);
    memcpy(&r.area, p + 4, 2);
    memcpy(r.state, p + 6, 2);
    r.name_len = p[8];
    r.name = (const char *)p + RECORD_HEAD;
    return r;
}

/* Compare the first n letters of a (length a_len) with b (length n), ignoring capitals: <0, 0 or >0. */
static int compare_start(const char *a, size_t a_len, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (i >= a_len) {
            return -1;   // a is shorter: it sorts first
        }
        int d = tolower((unsigned char)a[i]) - tolower((unsigned char)b[i]);
        if (d) {
            return d;
        }
    }
    return 0;
}

/* Cut spaces off both ends. Returns the new start and writes the new length. */
static const char *trimmed(const char *s, size_t *len)
{
    while (*len && s[0] == ' ') {
        s++;
        (*len)--;
    }
    while (*len && s[*len - 1] == ' ') {
        (*len)--;
    }
    return s;
}

int city_table_search(const char *typed, city_t *out, int max)
{
    // "austin, t" -> name "austin" (must be the whole name), state starts with "t"
    const char *comma = strchr(typed, ',');
    size_t name_len = comma ? (size_t)(comma - typed) : strlen(typed);
    size_t state_len = comma ? strlen(comma + 1) : 0;
    const char *name = trimmed(typed, &name_len);
    const char *state = comma ? trimmed(comma + 1, &state_len) : "";
    if (name_len == 0 || state_len > 2 || max <= 0) {
        return 0;
    }
    if (max > KEEP_MAX) {
        max = KEEP_MAX;
    }

    // The first town that does not sort before what was typed
    int low = 0, high = city_table_count();
    while (low < high) {
        int mid = low + (high - low) / 2;
        record_t r = record_at(mid);
        if (compare_start(r.name, r.name_len, name, name_len) < 0) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }

    // Read on from there while the names still start with it, keeping the best few.
    // Best = the name is exactly what was typed, then the biggest town.
    record_t best[KEEP_MAX];
    uint32_t score[KEEP_MAX];
    int found = 0, count = city_table_count();
    for (int i = low; i < count && i < low + SCAN_MAX; i++) {
        record_t r = record_at(i);
        if (compare_start(r.name, r.name_len, name, name_len) != 0) {
            break;
        }
        bool whole = r.name_len == name_len;
        if (comma && (!whole || compare_start(r.state, 2, state, state_len) != 0)) {
            continue;
        }
        uint32_t s = (whole ? 1u << 20 : 0) + r.area;
        int at = found;
        while (at > 0 && score[at - 1] < s) {
            at--;
        }
        if (at >= max) {
            continue;
        }
        if (found < max) {
            found++;
        }
        for (int j = found - 1; j > at; j--) {
            best[j] = best[j - 1];
            score[j] = score[j - 1];
        }
        best[at] = r;
        score[at] = s;
    }
    for (int i = 0; i < found; i++) {
        snprintf(out[i].name, sizeof(out[i].name), "%.*s, %.2s", (int)best[i].name_len, best[i].name, best[i].state);
        out[i].lat_e2 = best[i].lat_e2;
        out[i].lon_e2 = best[i].lon_e2;
    }
    return found;
}

bool city_table_find(const char *name, city_t *out)
{
    city_t some[KEEP_MAX];
    int n = city_table_search(name, some, KEEP_MAX);
    for (int i = 0; i < n; i++) {
        if (strcasecmp(some[i].name, name) == 0) {
            *out = some[i];
            return true;
        }
    }
    return false;
}
