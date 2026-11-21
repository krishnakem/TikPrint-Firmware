#!/usr/bin/env python3
"""
Build data/zip_table.bin: the ZIP code -> latitude/longitude table the firmware
uses to find the weather forecast for a ZIP code (main/zip_table.c).

Source: the US Census Bureau "ZCTA Gazetteer" file (public domain). Download and unzip
  https://www2.census.gov/geo/docs/maps-data/data/gazetteer/2024_Gazetteer/2024_Gaz_zcta_national.zip
then run:
  python3 tools/make_zip_table.py path/to/2024_Gaz_zcta_national.txt data/zip_table.bin

Output: one 8-byte record per ZIP code, sorted by ZIP code (so the board can binary-search it):
  4 bytes  ZIP code as a number        (unsigned, little-endian)
  2 bytes  latitude  in 1/100 degree   (signed,   little-endian)
  2 bytes  longitude in 1/100 degree   (signed,   little-endian)
1/100 degree is about 1 km, finer than the 2.5 km squares the weather forecast comes in.
"""
import struct
import sys


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    rows = []
    with open(sys.argv[1], encoding="utf-8") as f:
        header = [h.strip() for h in f.readline().split("\t")]
        zip_col, lat_col, lon_col = header.index("GEOID"), header.index("INTPTLAT"), header.index("INTPTLONG")
        for line in f:
            cols = [c.strip() for c in line.split("\t")]
            if len(cols) <= lon_col:
                continue
            rows.append((int(cols[zip_col]), round(float(cols[lat_col]) * 100), round(float(cols[lon_col]) * 100)))
    rows.sort()
    with open(sys.argv[2], "wb") as out:
        for row in rows:
            out.write(struct.pack("<Ihh", *row))
    print(f"{len(rows)} ZIP codes, {len(rows) * 8} bytes -> {sys.argv[2]}")


if __name__ == "__main__":
    main()
