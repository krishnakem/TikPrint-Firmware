#!/usr/bin/env python3
"""
Build data/city_table.bin: the town name -> latitude/longitude table the firmware uses when
a place is added by name ("Austin, TX") instead of by ZIP code (main/city_table.c).

Source: the US Census Bureau "Places Gazetteer" file (public domain). Download and unzip
  https://www2.census.gov/geo/docs/maps-data/data/gazetteer/2024_Gazetteer/2024_Gaz_place_national.zip
then run:
  python3 tools/make_city_table.py path/to/2024_Gaz_place_national.txt data/city_table.bin

Output (all numbers little-endian):
  4 bytes            number of towns, N
  N times 4 bytes    where each town's record starts, counted from the start of the file
  N records, sorted by name (ignoring capitals), so the board can binary-search a typed beginning:
    2 bytes  latitude  in 1/100 degree (signed)
    2 bytes  longitude in 1/100 degree (signed)
    2 bytes  land area in square km, at most 65535 (bigger towns are suggested first)
    2 bytes  state, e.g. "TX"
    1 byte   length of the name
    n bytes  the name in plain ASCII, e.g. "Austin"
"""
import re
import struct
import sys
import unicodedata

# The Census adds what kind of place it is to the name ("Austin city"): leave that out.
KINDS = re.compile(r" (city|town|village|borough|CDP|comunidad|zona urbana|municipality|"
                   r"city and borough|consolidated government|metropolitan government|unified government|"
                   r"urban county|metro government|corporation|plantation)( \(balance\))?$")


def plain(text):
    """Accented letters -> plain ones, so the name can be typed on any keyboard and printed in any font."""
    text = unicodedata.normalize("NFKD", text)
    return "".join(c for c in text if ord(c) < 128)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    towns = {}
    with open(sys.argv[1], encoding="utf-8") as f:
        header = [h.strip() for h in f.readline().split("\t")]
        col = {name: header.index(name) for name in ("USPS", "NAME", "ALAND_SQMI", "INTPTLAT", "INTPTLONG")}
        for line in f:
            cols = [c.strip() for c in line.split("\t")]
            if len(cols) <= col["INTPTLONG"]:
                continue
            name = plain(cols[col["NAME"]])
            while KINDS.search(name):
                name = KINDS.sub("", name)
            name = name.replace(" (balance)", "").strip()
            state = cols[col["USPS"]]
            if not name or len(name) > 40 or len(state) != 2:
                continue
            area = min(65535, round(float(cols[col["ALAND_SQMI"]]) * 2.59))
            town = (name, state, round(float(cols[col["INTPTLAT"]]) * 100), round(float(cols[col["INTPTLONG"]]) * 100), area)
            key = (name.lower(), state)
            if key not in towns or towns[key][4] < area:   # two places with one name in a state: keep the bigger
                towns[key] = town
    rows = sorted(towns.values(), key=lambda t: (t[0].lower(), t[1]))
    records, starts, at = [], [], 4 + 4 * len(rows)
    for name, state, lat, lon, area in rows:
        record = struct.pack("<hhH2sB", lat, lon, area, state.encode(), len(name)) + name.encode()
        starts.append(at)
        records.append(record)
        at += len(record)
    with open(sys.argv[2], "wb") as out:
        out.write(struct.pack("<I", len(rows)))
        out.write(struct.pack(f"<{len(rows)}I", *starts))
        out.write(b"".join(records))
    print(f"{len(rows)} towns, {at} bytes -> {sys.argv[2]}")


if __name__ == "__main__":
    main()
