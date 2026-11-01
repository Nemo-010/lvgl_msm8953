#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Check the native_sim render of the UI.

The point of running the app on the host against a simulated MDP5 pipe is that
the *same* driver code, blit path and LVGL glue that go into the boot image can
be exercised without a phone. This script asserts that what came out is really
the UI and not, say, an untouched bootloader splash or a blank surface.
"""

import struct
import sys
from collections import Counter

BG = "101828"      # screen background
TITLE = "ffffff"   # title text
ACCENT = "f0a020"  # arc indicator
SUB = "b0c4d8"     # subtitle text
ACCENT2 = "20c0f0" # footer text

failures = []
checks = 0


def check(cond, msg, detail=""):
    global checks
    checks += 1
    print(("  ok   " if cond else "  FAIL ") + msg + ((": " + detail) if not cond and detail else ""))
    if not cond:
        failures.append(msg)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "mido_fb.ppm"
    data = open(path, "rb").read()

    if not data.startswith(b"P6"):
        sys.exit("not a PPM")
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    px = parts[3][: w * h * 3]

    check((w, h) == (1080, 1920), "framebuffer is 1080x1920", "%dx%d" % (w, h))

    counts = Counter(px[i:i + 3].hex() for i in range(0, len(px), 3))
    total = w * h

    check(counts[BG] > total * 0.90, "background covers the screen",
          "%s px of %d" % (counts[BG], total))
    check(counts[TITLE] > 500, "title text drawn in white", str(counts[TITLE]))
    check(counts[ACCENT] > 1000, "arc indicator drawn in accent orange",
          str(counts[ACCENT]))
    check(counts[SUB] > 200, "subtitle text drawn", str(counts[SUB]))
    check(counts[ACCENT2] > 50, "footer text drawn", str(counts[ACCENT2]))

    # The bootloader's splash colour must be gone, otherwise we never drew.
    check(counts["200020"] < 100, "bootloader splash fully painted over",
          str(counts["200020"]))

    # Title should be horizontally centred and sit above the middle.
    xs, ys = [], []
    for y in range(0, h, 2):
        row = px[y * w * 3:(y + 1) * w * 3]
        for x in range(0, w, 2):
            if row[x * 3:x * 3 + 3].hex() == TITLE:
                xs.append(x)
                ys.append(y)
    check(bool(xs), "title pixels found")
    if xs:
        cx = (min(xs) + max(xs)) / 2
        cy = (min(ys) + max(ys)) / 2
        check(abs(cx - w / 2) < w * 0.1, "title is horizontally centred", "cx=%.0f" % cx)
        check(cy < h / 2, "title sits above the middle", "cy=%.0f" % cy)

    print()
    if failures:
        print("%d/%d checks FAILED" % (len(failures), checks))
        return 1
    print("all %d checks passed" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
