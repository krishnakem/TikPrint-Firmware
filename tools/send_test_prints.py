#!/usr/bin/env python3
"""
Send several test tickets to the board, one right after another, to try the print queue.

    python3 tools/send_test_prints.py            # 3 tickets
    python3 tools/send_test_prints.py 5          # 5 tickets
    python3 tools/send_test_prints.py 3 --dry    # build the tickets but send nothing

Each ticket is a striped image about 10 cm long. An image is used on purpose: it takes
the board a few seconds to send, so the later tickets really have to wait in line.
(A text-only ticket is sent in a few milliseconds, too fast to see the queue.)
"""
import json
import struct
import sys
import urllib.request

HOST = "http://tikprint.local"   # or http://192.168.7.1 over USB
WIDTH_DOTS = 576                 # must match PRINTER_WIDTH_DOTS in main/printer.h
ROWS = 800                       # 8 rows = 1 mm, so 800 rows = 10 cm
STRIPE_EVERY = 80                # one black stripe per cm
STRIPE_ROWS = 4


def ticket_body(index):
    """The body /api/print expects: 4-byte length + JSON + raw image bits (see print_post in api.c)."""
    meta = {"title": f"Queue test {index}", "body": "", "source": "TEST",
            "image": {"width": WIDTH_DOTS, "height": ROWS}}
    text = json.dumps(meta).encode()
    row_bytes = WIDTH_DOTS // 8
    image = bytearray()
    for y in range(ROWS):
        black = y % STRIPE_EVERY < STRIPE_ROWS * index   # ticket 1 has thin stripes, ticket 2 thicker, ...
        image += (b"\xff" if black else b"\x00") * row_bytes
    return struct.pack("<I", len(text)) + text + bytes(image)


def main():
    args = [a for a in sys.argv[1:] if a != "--dry"]
    dry = "--dry" in sys.argv
    count = int(args[0]) if args else 3
    for i in range(1, count + 1):
        body = ticket_body(i)
        if dry:
            print(f"ticket {i}: {len(body)} bytes (not sent)")
            continue
        req = urllib.request.Request(HOST + "/api/print", data=body,
                                     headers={"Content-Type": "application/octet-stream"})
        with urllib.request.urlopen(req, timeout=20) as reply:
            print(f"ticket {i}: {reply.read().decode()}")


if __name__ == "__main__":
    main()
