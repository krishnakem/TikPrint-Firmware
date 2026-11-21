#!/usr/bin/env python3
"""
Build data/ticket_fonts.bin: the letters the board draws tickets with when no browser is
there to draw them (scheduled weather tickets). See main/ticket_font.c.

A browser turns a font file into letters of any size. The board cannot, so this tool does
it ahead of time: for each font in web/fonts and each size a ticket uses, it draws every
letter as black and white dots and stores them.

Needs:  pip install pillow fonttools brotli
Run:    python3 tools/make_ticket_fonts.py web/fonts data/ticket_fonts.bin

Output (all numbers little-endian):
  4 bytes   "TKF1"
  2 bytes   number of faces, N   (a face = one font, in one weight, at one size)
  N times 16 bytes, one per face:
    1 font (0 = Inter, 1 = Source Serif, 2 = Nunito, 3 = Caveat, 4 = JetBrains Mono: FONTS below)
    1 bold (0 or 1)
    1 size in dots (height of the letters' box)
    1 number of letters (always 97)
    1 ascent: dots from the top of a line to the line the letters stand on
    1 line height in dots
    2 unused
    4 where this face's letter list starts, from the start of the file
    4 where this face's dots start
  Letter list: 97 times 8 bytes, for the characters space..~ then the degree sign and the middle dot:
    1 advance: dots to move right after the letter
    1 width, 1 height of its dots
    1 x offset (signed): dots right of the pen position where its dots start
    1 top (signed): dots below the top of the line where its dots start
    1 unused
    2 where its dots start, counted from the face's dots
  Dots: one letter after another, row by row, each row padded to whole bytes, leftmost dot = top bit, 1 = black.
"""
import io
import os
import struct
import sys

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

# Order matters: the number of a font is its place here (and in FONT_NAMES in main/ticket_font.c).
FONTS = ["inter", "source-serif", "nunito", "caveat", "jetbrains-mono"]
REGULAR_SIZES = [22, 25, 29]                    # text: small, medium, large
BOLD_SIZES = [17, 20, 22, 25, 29, 44, 52, 60]   # 17-20 = header and source line; 22-29 = labels; 44-60 = the title
REGULAR_WEIGHT, BOLD_WEIGHT = 500, 700          # a little heavier than on screen: thin strokes fade on thermal paper
CHARS = [chr(c) for c in range(32, 127)] + ["°", "·"]
BLACK_FROM = 110                             # a dot this dark or darker (0-255) prints black


def load(path, size, weight):
    """Open a .woff2 font at one size and weight."""
    ttf = io.BytesIO()
    font = TTFont(path)
    font.flavor = None          # woff2 -> plain TrueType, which Pillow can read
    font.save(ttf)
    ttf.seek(0)
    face = ImageFont.truetype(ttf, size)
    try:
        axes = face.get_variation_axes()
    except OSError:
        return face             # not a variable font: it has the one weight it has
    values = []
    for axis in axes:
        tag = axis["name"] if isinstance(axis["name"], str) else axis["name"].decode()
        if tag.lower().startswith("weight") or tag == "wght":
            values.append(max(axis["minimum"], min(axis["maximum"], weight)))
        else:
            values.append(axis["default"])
    face.set_variation_by_axes(values)
    return face


def draw_face(face):
    """-> (ascent, line height, [(advance, width, height, x offset, top, dots), ...])"""
    ascent, descent = face.getmetrics()
    box = (ascent + descent) * 3
    glyphs = []
    for ch in CHARS:
        img = Image.new("L", (box, box), 0)
        ImageDraw.Draw(img).text((box // 3, box // 3), ch, font=face, fill=255, anchor="la")
        bw = img.point(lambda v: 255 if v >= 255 - BLACK_FROM else 0)
        bbox = bw.getbbox()
        advance = min(255, round(face.getlength(ch)))
        if not bbox:
            glyphs.append((advance, 0, 0, 0, 0, b""))
            continue
        left, top, right, bottom = bbox
        w, h = right - left, bottom - top
        px = bw.load()
        rows = bytearray()
        for y in range(top, bottom):
            row = bytearray((w + 7) // 8)
            for x in range(w):
                if px[left + x, y]:
                    row[x >> 3] |= 0x80 >> (x & 7)
            rows += row
        glyphs.append((advance, w, h, left - box // 3, top - box // 3, bytes(rows)))
    return ascent, ascent + descent, glyphs


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    faces = []
    for font_id, name in enumerate(FONTS):
        path = os.path.join(sys.argv[1], name + ".woff2")
        for bold, sizes in ((0, REGULAR_SIZES), (1, BOLD_SIZES)):
            for size in sizes:
                ascent, line, glyphs = draw_face(load(path, size, BOLD_WEIGHT if bold else REGULAR_WEIGHT))
                faces.append((font_id, bold, size, ascent, line, glyphs))
    at = 6 + 16 * len(faces)
    directory, blobs = b"", b""
    for font_id, bold, size, ascent, line, glyphs in faces:
        table, dots = b"", b""
        for advance, w, h, xoff, top, rows in glyphs:
            table += struct.pack("<BBBbbBH", advance, w, h, max(-128, min(127, xoff)), max(-128, min(127, top)), 0, len(dots))
            dots += rows
        assert len(dots) < 65536, "a face's dots must fit 16-bit offsets"
        directory += struct.pack("<BBBBBBHII", font_id, bold, size, len(glyphs), ascent, min(255, line), 0, at, at + len(table))
        blobs += table + dots
        at += len(table) + len(dots)
    with open(sys.argv[2], "wb") as out:
        out.write(b"TKF1" + struct.pack("<H", len(faces)) + directory + blobs)
    print(f"{len(faces)} faces, {at} bytes -> {sys.argv[2]}")


if __name__ == "__main__":
    main()
