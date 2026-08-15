#!/usr/bin/env python3
"""Writes a newc-format cpio initramfs by hand.

Two reasons not to use the system cpio: /dev/console has to be a character
device node or the kernel gives PID 1 no stdout at all (and then the run is
silent), and creating a device node on the host needs root. Emitting the archive
directly sets the mode and rdev fields without touching the host filesystem.
"""

import os
import sys

TRAILER = "TRAILER!!!"


def entry(name, mode, data=b"", ino=0, nlink=1, rmajor=0, rminor=0):
    header = "070701"
    fields = [
        ino, mode, 0, 0, nlink, 0, len(data),
        0, 0,          # device the file resides on
        rmajor, rminor,  # the node's own device numbers
        len(name) + 1, 0,
    ]
    out = bytearray(header.encode())
    for value in fields:
        out += b"%08X" % value
    out += name.encode() + b"\0"
    while len(out) % 4:
        out += b"\0"
    out += data
    while len(out) % 4:
        out += b"\0"
    return bytes(out)


def main(argv):
    if len(argv) < 3:
        sys.stderr.write("usage: mkcpio.py <out.cpio> <name=path> ...\n")
        return 2

    out_path = argv[1]
    blob = bytearray()
    ino = 100

    # S_IFDIR 0o040000, S_IFCHR 0o020000, S_IFREG 0o100000
    blob += entry(".", 0o040755, ino=ino); ino += 1
    blob += entry("dev", 0o040755, ino=ino); ino += 1
    # /dev/console is character device 5:1 - what the kernel opens for init.
    blob += entry("dev/console", 0o020600, ino=ino, rmajor=5, rminor=1); ino += 1

    for spec in argv[2:]:
        name, _, path = spec.partition("=")
        with open(path, "rb") as handle:
            data = handle.read()
        mode = 0o100755 if name == "init" else 0o100644
        blob += entry(name, mode, data, ino=ino)
        ino += 1
        sys.stderr.write("  %-16s %9d bytes\n" % (name, len(data)))

    blob += entry(TRAILER, 0)
    while len(blob) % 512:
        blob += b"\0"

    with open(out_path, "wb") as handle:
        handle.write(blob)
    sys.stderr.write("mkcpio: %s - %.1f MB\n" % (out_path, len(blob) / 1048576.0))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
