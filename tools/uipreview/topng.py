#!/usr/bin/env python3
"""Convert the preview's PPM output to PNG, with no third-party library.

The renderer writes PPM because that needs no image library in C at all, but
nobody wants to open a PPM. PNG is written here by hand: zlib is in the Python
standard library and a PNG is four chunks around a zlib stream, so this has no
dependencies and cannot stop working because something was not installed.
"""

import struct
import sys
import zlib


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()

    # Header: "P6\n<w> <h>\n255\n". Parsed by taking the first three
    # whitespace-separated tokens after the magic, which also tolerates the
    # comments a PPM is allowed to carry.
    if not data.startswith(b"P6"):
        raise ValueError("%s is not a binary PPM" % path)

    pos = 2
    fields = []

    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1

        if data[pos:pos + 1] == b"#":
            while data[pos:pos + 1] not in (b"\n", b""):
                pos += 1
            continue

        start = pos

        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1

        fields.append(int(data[start:pos]))

    pos += 1  # the single whitespace byte before the pixels
    width, height, _maxval = fields

    return width, height, data[pos:pos + width * height * 3]


def write_png(path, width, height, rgb):
    raw = bytearray()

    for y in range(height):
        raw.append(0)  # filter type 0, no prediction
        raw += rgb[y * width * 3:(y + 1) * width * 3]

    def chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload
                + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))

    header = struct.pack(">2I5B", width, height, 8, 2, 0, 0, 0)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", header))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))


def main():
    if len(sys.argv) < 2:
        print("usage: topng.py <file.ppm> [...]", file=sys.stderr)

        return 1

    for src in sys.argv[1:]:
        width, height, rgb = read_ppm(src)

        if len(rgb) != width * height * 3:
            print("%s is truncated" % src, file=sys.stderr)

            return 1

        dst = src.rsplit(".", 1)[0] + ".png"
        write_png(dst, width, height, rgb)

    return 0


if __name__ == "__main__":
    sys.exit(main())
