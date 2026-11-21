#!/usr/bin/env python3
"""
Draw the weather symbols printed at the top of weather tickets and write them to
data/weather_icons.bin (used by main/weather_icons.c).

  python3 tools/make_weather_icons.py data/weather_icons.bin [preview.png]

Each symbol is SIZE x SIZE dots, 1 bit per dot (1 = black, leftmost dot = top bit), rows top
to bottom. The file is the symbols one after another, in the order of ICONS below, which must
match weather_icon_t in main/weather_icons.h.

The symbols are drawn with thick lines so they print cleanly on thermal paper. They are made
from simple shapes by asking, for every dot, "how far is this dot from the shape's edge?".
No drawing library is needed. The optional preview is a PNG of all symbols side by side.
"""
import math
import struct
import sys
import zlib

SIZE = 96      # dots per side (12 mm on paper)
STROKE = 6     # line thickness in dots


class Canvas:
    def __init__(self):
        self.px = [[0] * SIZE for _ in range(SIZE)]

    def paint(self, inside, value=1):
        """Set every dot for which inside(x, y) is true."""
        for y in range(SIZE):
            for x in range(SIZE):
                if inside(x + 0.5, y + 0.5):
                    self.px[y][x] = value

    def line(self, x0, y0, x1, y1, width=STROKE):
        """A thick line with round ends."""
        def inside(x, y):
            dx, dy = x1 - x0, y1 - y0
            t = max(0.0, min(1.0, ((x - x0) * dx + (y - y0) * dy) / (dx * dx + dy * dy)))
            return math.hypot(x - x0 - t * dx, y - y0 - t * dy) <= width / 2
        self.paint(inside)

    def disc(self, cx, cy, r, value=1):
        self.paint(lambda x, y: math.hypot(x - cx, y - cy) <= r, value)

    def ring(self, cx, cy, r):
        self.paint(lambda x, y: r - STROKE <= math.hypot(x - cx, y - cy) <= r)

    def polygon(self, points):
        def inside(x, y):
            hit = False
            for (ax, ay), (bx, by) in zip(points, points[1:] + points[:1]):
                if (ay > y) != (by > y) and x < ax + (y - ay) * (bx - ax) / (by - ay):
                    hit = not hit
            return hit
        self.paint(inside)

    def bytes(self):
        out = bytearray()
        for row in self.px:
            for i in range(0, SIZE, 8):
                out.append(sum(bit << (7 - k) for k, bit in enumerate(row[i:i + 8])))
        return bytes(out)


def cloud_distance(x, y, dx, dy, s):
    """Roughly how far (x, y) is from the edge of a cloud (negative = inside). dx, dy move it, s scales it."""
    x, y = (x - dx) / s, (y - dy) / s
    bumps = [(30, 50, 15), (50, 40, 21), (70, 52, 14)]
    d = min(math.hypot(x - cx, y - cy) - r for cx, cy, r in bumps)
    # the flat bottom: a bar with round ends from (26, 58) to (72, 58)
    t = max(26.0, min(72.0, x))
    d = min(d, math.hypot(x - t, y - 58) - 12)
    return d * s


def cloud(c, dx=0, dy=0, s=1.0):
    """A cloud outline. Whatever was drawn behind it is wiped out first, so it sits in front."""
    def inside(x, y):
        return cloud_distance(x, y, dx, dy, s) <= 0

    def on_outline(x, y):
        # inside the cloud, but with a spot outside it less than one line thickness away
        ring = ((x + STROKE * math.cos(k * math.pi / 12), y + STROKE * math.sin(k * math.pi / 12)) for k in range(24))
        return inside(x, y) and any(not inside(px, py) for px, py in ring)

    c.paint(inside, 0)
    c.paint(on_outline)


def sun(c, cx, cy, r, ray_from, ray_to):
    c.ring(cx, cy, r)
    for i in range(8):
        a = i * math.pi / 4
        c.line(cx + ray_from * math.cos(a), cy + ray_from * math.sin(a),
               cx + ray_to * math.cos(a), cy + ray_to * math.sin(a))


def moon(c, cx, cy, r):
    """A crescent: a disc with a bite taken out of its upper right."""
    c.disc(cx, cy, r)
    c.disc(cx + r * 0.45, cy - r * 0.3, r * 0.85, 0)


def draw_clear_day(c):
    sun(c, 48, 48, 19, 27, 40)


def draw_clear_night(c):
    moon(c, 44, 50, 30)


def draw_partly_day(c):
    sun(c, 32, 32, 14, 20, 29)
    cloud(c, 14, 22, 0.8)


def draw_partly_night(c):
    moon(c, 30, 34, 22)
    cloud(c, 14, 22, 0.8)


def draw_cloudy(c):
    cloud(c, -2, -4)


def draw_rain(c):
    cloud(c, -2, -18)
    for x in (30, 50, 70):
        c.line(x, 64, x - 8, 84)


def draw_snow(c):
    cloud(c, -2, -18)
    for x, y in ((26, 68), (48, 68), (70, 68), (37, 84), (59, 84)):
        c.disc(x, y, 5)


def draw_thunder(c):
    cloud(c, -2, -18)
    c.polygon([(52, 56), (34, 76), (46, 76), (40, 94), (62, 70), (50, 70), (58, 56)])


def draw_fog(c):
    cloud(c, -2, -22)
    for y in (64, 78, 90):
        c.line(16, y, 80, y)


# Order must match weather_icon_t in main/weather_icons.h
ICONS = [draw_clear_day, draw_clear_night, draw_partly_day, draw_partly_night, draw_cloudy,
         draw_rain, draw_snow, draw_thunder, draw_fog]


def write_png(path, canvases):
    """All symbols side by side, black on white, with a grey line between them."""
    gap, width = 8, len(canvases) * (SIZE + 8) - 8
    raw = bytearray()
    for y in range(SIZE):
        raw.append(0)
        for i, c in enumerate(canvases):
            raw.extend(0 if bit else 255 for bit in c.px[y])
            if i < len(canvases) - 1:
                raw.extend([200] * gap)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, SIZE, 8, 0, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b""))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    canvases = []
    for draw in ICONS:
        c = Canvas()
        draw(c)
        canvases.append(c)
    with open(sys.argv[1], "wb") as out:
        for c in canvases:
            out.write(c.bytes())
    print(f"{len(canvases)} symbols of {SIZE}x{SIZE} dots, {len(canvases) * SIZE * SIZE // 8} bytes -> {sys.argv[1]}")
    if len(sys.argv) > 2:
        write_png(sys.argv[2], canvases)


if __name__ == "__main__":
    main()
