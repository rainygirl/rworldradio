#!/usr/bin/env python3
"""Diffs two decode_probe reports and says whether the PowerPC run agrees.

`checksum` is expected to differ for AAC and only for AAC: faad2 is floating
point, ARM64 contracts multiply-add where PowerPC does not, and one last-bit
difference propagates through the rotate-XOR. Everything else must match -
including `coarse`, which is the same checksum quantized to 9 bits and so is
order-sensitive across the whole stream while tolerating that last bit.
"""

import sys

FIELDS = ("result", "frames", "rate", "channels", "samples",
          "checksum", "coarse", "rms", "peak", "mid8")

# Only this one may differ, and only for a floating-point decoder.
TOLERATED = {("aac", "checksum")}


def parse(path):
    out, current = {}, None
    for line in open(path):
        line = line.strip()
        if line.startswith("codec"):
            current = line.split()[1]
            out[current] = {}
        elif current and line and not line.startswith(("#", "---")):
            key, _, value = line.partition(" ")
            out[current][key] = value.strip()
    return out


def main(argv):
    native, ppc = parse(argv[1]), parse(argv[2])
    failures = 0

    print("%-6s %-9s %-30s %-30s" % ("codec", "field", "arm64 LE (host)", "ppc 7450 BE (qemu)"))
    print("-" * 82)
    for codec in sorted(set(native) & set(ppc)):
        for field in FIELDS:
            a, b = native[codec].get(field, ""), ppc[codec].get(field, "")
            same = a == b
            note = "ok" if same else ("expected" if (codec, field) in TOLERATED else "MISMATCH")
            if not same and note == "MISMATCH":
                failures += 1
            show_a, show_b = a[:30], b[:30]
            print("%-6s %-9s %-30s %-30s %s" % (codec, field, show_a, show_b, note))
        print()

    if failures:
        print("%d field(s) differ that should not" % failures)
        return 1
    print("PowerPC decode matches the host")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
