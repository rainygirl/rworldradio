#!/usr/bin/env python3
"""Builds data/cacert.der - the trust store mbedTLS verifies HTTPS hosts against.

The input is a normal PEM CA bundle (Mozilla's, as shipped by certifi, curl or
any Linux distribution). The output is the same certificates concatenated as raw
DER, with no base64 and no PEM armour.

That conversion is worth doing at build time. A current bundle is ~150
certificates and ~230 kB of base64; decoding it costs a G4 a few hundred
milliseconds, and it would be paid on the first HTTPS station of every launch.
Concatenated DER is parsed by walking ASN.1 lengths (see der_object_len in
src/net.c) and costs almost nothing.

Usage:
    tools/mkcastore.py                          # find a bundle automatically
    tools/mkcastore.py /path/to/cacert.pem
    tools/mkcastore.py /path/to/cacert.pem data/cacert.der
"""

import base64
import os
import re
import sys

PEM_BLOCK = re.compile(
    rb"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", re.DOTALL
)

# Where a PEM bundle usually lives, most specific first. certifi is checked via
# an import rather than a path because its location depends on the interpreter.
SYSTEM_BUNDLES = [
    "/etc/ssl/cert.pem",                                   # macOS, FreeBSD
    "/etc/ssl/certs/ca-certificates.crt",                  # Debian, Ubuntu
    "/etc/pki/tls/certs/ca-bundle.crt",                    # Fedora, RHEL
    "/usr/local/etc/openssl/cert.pem",                     # Homebrew
    "/opt/homebrew/etc/openssl@3/cert.pem",
]


def find_bundle():
    try:
        import certifi

        return certifi.where()
    except ImportError:
        pass
    for path in SYSTEM_BUNDLES:
        if os.path.isfile(path):
            return path
    return None


def convert(pem_bytes):
    """PEM bundle -> concatenated DER. Returns (blob, certificate_count)."""
    out = bytearray()
    count = 0

    for match in PEM_BLOCK.finditer(pem_bytes):
        body = b"".join(match.group(1).split())
        try:
            der = base64.b64decode(body, validate=True)
        except Exception:
            sys.stderr.write("mkcastore: skipping a certificate that is not valid base64\n")
            continue
        # A certificate is an ASN.1 SEQUENCE; src/net.c walks the store by
        # reading these headers, so anything else would desynchronize it.
        if not der or der[0] != 0x30:
            sys.stderr.write("mkcastore: skipping a block that is not a DER SEQUENCE\n")
            continue
        out += der
        count += 1

    return bytes(out), count


def main(argv):
    source = argv[1] if len(argv) > 1 else find_bundle()
    out_path = argv[2] if len(argv) > 2 else "data/cacert.der"

    if not source:
        sys.stderr.write(
            "mkcastore: no CA bundle found. Pass one explicitly, or install certifi\n"
            "           (pip install certifi).\n"
        )
        return 1
    if not os.path.isfile(source):
        sys.stderr.write("mkcastore: %s does not exist\n" % source)
        return 1

    with open(source, "rb") as handle:
        blob, count = convert(handle.read())

    if count == 0:
        sys.stderr.write("mkcastore: %s contained no certificates\n" % source)
        return 1

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "wb") as handle:
        handle.write(blob)

    sys.stderr.write(
        "mkcastore: %s - %d certificates, %.0f kB (from %s)\n"
        % (out_path, count, len(blob) / 1024.0, source)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
