#!/usr/bin/env python3
"""Writes the WAV that verify.sh encodes to MP3 and AAC.

Content matters here: the two channels carry different tones and an
anti-symmetric dither, so a channel swap or a byte-order bug cannot slip through
on symmetry. The fade at each end keeps the encoders from clipping the first
frame.
"""

import math
import struct
import sys
import wave

RATE = 44100
SECONDS = 2.0


def main(argv):
    out_path = argv[1] if len(argv) > 1 else "test.wav"
    frames = int(RATE * SECONDS)

    handle = wave.open(out_path, "wb")
    handle.setnchannels(2)
    handle.setsampwidth(2)
    handle.setframerate(RATE)

    data = bytearray()
    state = 12345
    for index in range(frames):
        t = index / RATE
        state = (1103515245 * state + 12345) & 0x7FFFFFFF
        noise = (state / 0x7FFFFFFF - 0.5) * 0.02
        envelope = min(1.0, t * 4) * min(1.0, (SECONDS - t) * 4)
        left = 0.55 * math.sin(2 * math.pi * 440 * t) \
            + 0.15 * math.sin(2 * math.pi * 1320 * t) + noise
        right = 0.40 * math.sin(2 * math.pi * 660 * t) \
            + 0.20 * math.sin(2 * math.pi * 220 * t) - noise
        data += struct.pack(
            "<hh",
            int(max(-1.0, min(1.0, left * envelope)) * 32000),
            int(max(-1.0, min(1.0, right * envelope)) * 32000),
        )

    handle.writeframes(bytes(data))
    handle.close()
    sys.stderr.write("mkfixture: %s - %d frames\n" % (out_path, frames))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
