#pragma once

#include <stdbool.h>

#define CITY_NAME_MAX 48   // room for a town with its state ("Louisville/Jefferson County, KY"), including the ending 0

typedef struct {
    char name[CITY_NAME_MAX];   // "Austin, TX"
    int lat_e2, lon_e2;         // where it is, in 1/100 degree
} city_t;

/**
 * Towns whose name starts with what was typed ("aust" -> Austin, TX; Austin, MN; Austintown, OH),
 * the biggest first. With a comma, the name must be whole and the state may be partly typed
 * ("austin, t" -> Austin, TX). Capitals do not matter. Returns how many were written to out (at most max).
 */
int city_table_search(const char *typed, city_t *out, int max);

/** The town with exactly this name and state ("Austin, TX"; capitals do not matter). False if there is none. */
bool city_table_find(const char *name, city_t *out);

/** How many towns the table holds. */
int city_table_count(void);
