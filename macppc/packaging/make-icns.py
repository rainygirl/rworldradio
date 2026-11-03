#!/usr/bin/env python3
"""Builds packaging/rworldradio.icns from the PNGs beside it.

`iconutil`, which the other macOS port uses, only writes the modern icon types:
ic07/ic08/ic09 (embedded PNG, 10.5+), ic04/ic05 (ARGB, 10.8+) and ic10-ic14
(Retina, 10.7+). Mac OS X 10.4's Icon Services reads none of those. Handed that
file, Tiger finds nothing it understands in the bundle and falls back to the
generic application icon - which is exactly what happened here.

Tiger reads the classic types, so this writes those:

    ics#  16x16 1-bit icon + 1-bit mask   (small list views)
    is32  16x16 24-bit RGB, RLE
    s8mk  16x16 8-bit alpha mask
    ICN#  32x32 1-bit icon + 1-bit mask
    il32  32x32 24-bit RGB, RLE
    l8mk  32x32 8-bit alpha mask
    ih32  48x48 24-bit RGB, RLE
    h8mk  48x48 8-bit alpha mask
    it32  128x128 24-bit RGB, RLE (with its four-byte zero prefix)
    t8mk  128x128 8-bit alpha mask

The modern PNG types are appended as well, so the same file also looks right if
the bundle is ever opened on a current Mac. A reader takes the best type it
knows and ignores the rest, so carrying both costs nothing but bytes.

No dependencies: the PNGs are decoded with zlib, and the RLE encoder verifies
itself by decoding its own output before writing.

    packaging/make-icns.py                       # -> packaging/rworldradio.icns
    packaging/make-icns.py path/to/out.icns
"""

import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))

# 24-bit RLE run lengths: a control byte of 0xFF means 130 repeats, and 0x7F
# means 128 literal bytes.
MAX_RUN = 130
MAX_LITERAL = 128


# ------------------------------------------------------------------- PNG ---

def read_png(path):
    """Minimal PNG reader. Returns (width, height, rgba_bytes).

    Only what these files are: 8-bit, non-interlaced, colour type 2 (RGB) or
    6 (RGBA). Anything else is an error rather than a silent wrong icon.
    """
    with open(path, "rb") as handle:
        data = handle.read()

    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("%s: not a PNG" % path)

    width = height = None
    depth = colour_type = interlace = None
    idat = bytearray()
    offset = 8

    while offset + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[offset:offset + 8])
        body = data[offset + 8:offset + 8 + length]
        offset += 12 + length  # 8-byte header + body + 4-byte CRC

        if kind == b"IHDR":
            width, height, depth, colour_type, _, _, interlace = struct.unpack(
                ">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break

    if width is None:
        raise SystemExit("%s: no IHDR" % path)
    if depth != 8 or interlace != 0 or colour_type not in (2, 6):
        raise SystemExit(
            "%s: need an 8-bit non-interlaced RGB/RGBA PNG (got depth %d, "
            "colour type %d, interlace %d)" % (path, depth, colour_type, interlace))

    channels = 4 if colour_type == 6 else 3
    raw = zlib.decompress(bytes(idat))
    stride = width * channels

    # Undo the per-row filters. Each row is prefixed with its filter type.
    out = bytearray()
    previous = bytearray(stride)
    position = 0
    for _ in range(height):
        filter_type = raw[position]
        position += 1
        row = bytearray(raw[position:position + stride])
        position += stride

        if filter_type == 1:      # Sub
            for i in range(channels, stride):
                row[i] = (row[i] + row[i - channels]) & 0xFF
        elif filter_type == 2:    # Up
            for i in range(stride):
                row[i] = (row[i] + previous[i]) & 0xFF
        elif filter_type == 3:    # Average
            for i in range(stride):
                left = row[i - channels] if i >= channels else 0
                row[i] = (row[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif filter_type == 4:    # Paeth
            for i in range(stride):
                left = row[i - channels] if i >= channels else 0
                up = previous[i]
                upper_left = previous[i - channels] if i >= channels else 0
                estimate = left + up - upper_left
                da, db, dc = (abs(estimate - left), abs(estimate - up),
                              abs(estimate - upper_left))
                if da <= db and da <= dc:
                    predictor = left
                elif db <= dc:
                    predictor = up
                else:
                    predictor = upper_left
                row[i] = (row[i] + predictor) & 0xFF
        elif filter_type != 0:
            raise SystemExit("%s: unknown row filter %d" % (path, filter_type))

        out += row
        previous = row

    if channels == 3:
        rgba = bytearray()
        for i in range(0, len(out), 3):
            rgba += out[i:i + 3] + b"\xff"
        out = rgba

    return width, height, bytes(out)


# ------------------------------------------------------------------- RLE ---

def rle24_encode(plane):
    """Apple's icns 24-bit RLE, one colour plane at a time.

    Control byte < 0x80: copy the next (n + 1) literal bytes.
    Control byte >= 0x80: repeat the next single byte (n - 0x80 + 3) times.
    """
    out = bytearray()
    index = 0
    count = len(plane)

    while index < count:
        run = 1
        while (index + run < count and plane[index + run] == plane[index]
               and run < MAX_RUN):
            run += 1

        if run >= 3:
            out.append(0x80 + run - 3)
            out.append(plane[index])
            index += run
            continue

        # A literal run, ending as soon as a compressible run begins.
        start = index
        index += 1
        while index < count and (index - start) < MAX_LITERAL:
            if (index + 2 < count
                    and plane[index] == plane[index + 1] == plane[index + 2]):
                break
            index += 1
        out.append(index - start - 1)
        out += plane[start:index]

    return bytes(out)


def rle24_decode(data, expected):
    """Inverse of rle24_encode, used only to check the encoder's own output."""
    out = bytearray()
    index = 0
    while index < len(data) and len(out) < expected:
        control = data[index]
        index += 1
        if control >= 0x80:
            out += bytes([data[index]]) * (control - 0x80 + 3)
            index += 1
        else:
            length = control + 1
            out += data[index:index + length]
            index += length
    return bytes(out)


def rgb_rle(rgba, width, height):
    """RGBA -> the three RLE'd colour planes an is32/il32/ih32/it32 holds."""
    pixels = width * height
    out = bytearray()
    for channel in range(3):
        plane = bytes(rgba[channel::4][:pixels])
        encoded = rle24_encode(plane)
        if rle24_decode(encoded, pixels) != plane:
            raise SystemExit("internal error: RLE round-trip failed")
        out += encoded
    return bytes(out)


def alpha_mask(rgba, width, height):
    return bytes(rgba[3::4][:width * height])


def one_bit(rgba, width, height, threshold=128):
    """The 1-bit icon and its 1-bit mask, packed MSB first.

    Both planes are the same shape here: a pixel is "on" where it is opaque
    enough to draw, and dark enough to be ink. Tiger only uses these in places
    that cannot show colour, so a threshold is enough.
    """
    pixels = width * height
    bitmap = bytearray((pixels + 7) // 8)
    mask = bytearray((pixels + 7) // 8)

    for index in range(pixels):
        red, green, blue, alpha = rgba[index * 4:index * 4 + 4]
        byte, bit = divmod(index, 8)
        shift = 7 - bit
        if alpha >= threshold:
            mask[byte] |= 1 << shift
            if (red * 299 + green * 587 + blue * 114) // 1000 < 190:
                bitmap[byte] |= 1 << shift

    return bytes(bitmap) + bytes(mask)


# ------------------------------------------------------------------ icns ---

def chunk(kind, payload):
    return kind + struct.pack(">I", len(payload) + 8) + payload


def build(source_dir):
    def png(size):
        path = os.path.join(source_dir, "rworldradio-%d.png" % size)
        if not os.path.isfile(path):
            raise SystemExit("missing %s" % path)
        width, height, rgba = read_png(path)
        if (width, height) != (size, size):
            raise SystemExit("%s is %dx%d, expected %dx%d"
                             % (path, width, height, size, size))
        return rgba

    icon16, icon32, icon48, icon128 = (png(16), png(32), png(48), png(128))
    chunks = []

    # Classic types - the only ones Tiger reads.
    chunks.append(chunk(b"ics#", one_bit(icon16, 16, 16)))
    chunks.append(chunk(b"is32", rgb_rle(icon16, 16, 16)))
    chunks.append(chunk(b"s8mk", alpha_mask(icon16, 16, 16)))

    chunks.append(chunk(b"ICN#", one_bit(icon32, 32, 32)))
    chunks.append(chunk(b"il32", rgb_rle(icon32, 32, 32)))
    chunks.append(chunk(b"l8mk", alpha_mask(icon32, 32, 32)))

    chunks.append(chunk(b"ih32", rgb_rle(icon48, 48, 48)))
    chunks.append(chunk(b"h8mk", alpha_mask(icon48, 48, 48)))

    # it32 alone carries four zero bytes before its RLE data. Every other
    # 24-bit type starts straight in, and a decoder that is not expecting the
    # prefix renders noise.
    chunks.append(chunk(b"it32", b"\x00\x00\x00\x00" + rgb_rle(icon128, 128, 128)))
    chunks.append(chunk(b"t8mk", alpha_mask(icon128, 128, 128)))

    # Modern types, so the bundle also looks right on a current Mac. These are
    # the PNG files verbatim.
    for kind, size in ((b"ic07", 128), (b"ic08", 256)):
        path = os.path.join(source_dir, "rworldradio-%d.png" % size)
        if os.path.isfile(path):
            with open(path, "rb") as handle:
                chunks.append(chunk(kind, handle.read()))

    body = b"".join(chunks)
    return b"icns" + struct.pack(">I", len(body) + 8) + body


def main(argv):
    out_path = argv[1] if len(argv) > 1 else os.path.join(HERE, "rworldradio.icns")
    blob = build(HERE)

    with open(out_path, "wb") as handle:
        handle.write(blob)

    # Report what a Tiger reader will actually find, since that is the whole
    # point of this script.
    classic = 0
    offset = 8
    while offset + 8 <= len(blob):
        kind, length = struct.unpack(">4sI", blob[offset:offset + 8])
        if kind in (b"ics#", b"is32", b"s8mk", b"ICN#", b"il32", b"l8mk",
                    b"ih32", b"h8mk", b"it32", b"t8mk"):
            classic += 1
        if length < 8:
            break
        offset += length

    sys.stderr.write("make-icns: %s - %.1f kB, %d classic (10.4-readable) entries\n"
                     % (out_path, len(blob) / 1024.0, classic))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
