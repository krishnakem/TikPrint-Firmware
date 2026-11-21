#pragma once

#include <stdbool.h>

/**
 * Where a US ZIP code is, from the table built into the firmware (data/zip_table.bin,
 * made by tools/make_zip_table.py from the US Census ZCTA gazetteer). No internet needed.
 *
 *   zip       five digits, e.g. "48104"
 *   lat_e2    latitude  in 1/100 degree, e.g.  4228 =  42.28
 *   lon_e2    longitude in 1/100 degree, e.g. -8374 = -83.74
 *
 * Returns false if zip is not five digits or is not in the table. (The table has every ZIP
 * code that covers an area. A few that are only PO boxes or one big building are missing.)
 */
bool zip_table_lookup(const char *zip, int *lat_e2, int *lon_e2);

/** How many ZIP codes the table holds. */
int zip_table_count(void);
