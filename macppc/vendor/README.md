# Vendored libraries

Three C libraries are built into the binary. None is checked in - run
`tools/fetch-vendor.sh` to download and unpack them here. The Makefile builds
each from source with this port's own flags rather than using a system copy,
because Tiger has no package manager and a statically linked binary is what makes
the `.app` self-contained.

| Directory | Version | Why |
|---|---|---|
| `mbedtls-2.28.10/` | 2.28.10 (LTS) | TLS 1.2. See below. |
| `libmad-0.15.1b/` | 0.15.1b | MP3 - 35,066 of the dataset's stations. |
| `faad2-2.10.0/` | 2.10.0 | AAC and HE-AAC - 14,209 stations. |

## Why mbedTLS and not Tiger's own TLS

Mac OS X 10.4's Secure Transport speaks SSL 3.0 and TLS 1.0 with RSA key
exchange. Essentially every host serving audio today requires TLS 1.2 with an
ECDHE key exchange and an AEAD cipher, so a Secure Transport client gets
`handshake_failure` from them - and 29,763 of the dataset's 51,443 stations are
HTTPS. Using the system TLS would mean losing well over half the app.

mbedTLS 2.28 is the last branch that still builds cleanly with GCC 4.0.1 and has
no C11, no `stdatomic`, and no assumptions about a little-endian host. It is
compiled with `mbedtls_config.h` in this directory, which turns off everything
this app does not use: DTLS, the server side, session tickets, PSK, all key
exchanges except ECDHE/RSA, and every curve except secp256r1 and Curve25519.
That takes the static library from roughly 900 kB to under 400 kB.

The cipher suite *order* the client offers is set in `src/net.c`, not here, and
puts ChaCha20-Poly1305 first: it is pure 32-bit add-rotate-xor, so unlike
table-driven AES it neither touches the FPU nor evicts anything from a G4's
256 kB L2 cache.

`MBEDTLS_SSL_SESSION_TICKETS` is on for the client side. Bulk encryption is not
where the time goes on this hardware - the handshake is, at 1.2-2.7 s measured -
so resumption matters more than cipher choice.

TLS 1.3 is not supported by the 2.28 branch. Hosts that require it - a small
minority today - will fail to connect and say so in the status row.

## Why libmad and faad2 rather than QuickTime

Tiger's QuickTime 7 can decode both MP3 and AAC, and its decoders are
AltiVec-optimized. It is still the wrong choice here: QuickTime wants a movie or
a data reference it can seek, and an endless ADTS bitstream arriving over a
socket is neither. Feeding it one means reimplementing most of what libmad and
faad2 already do, and the failure modes are much harder to see.

libmad matters for a second reason: its `fixed.h` has a hand-written PowerPC
implementation of the fixed-point multiply (`FPM_PPC`, using `mulhw`/`mulhwu`).
The Makefile defines `FPM_PPC` explicitly rather than trusting the configure
script to detect it.

faad2 is built in its floating-point mode. Its fixed-point mode exists for
machines with no FPU; a G4 has a good one plus AltiVec, and fixed-point costs
audible quality on the SBR stage that HE-AAC streams depend on.

## Licences

| | |
|---|---|
| libmad 0.15.1b | GPL-2.0-**or-later** |
| faad2 2.10.0 | GPL-2.0-**or-later**, plus the note below |
| mbedTLS 2.28.10 | dual **Apache-2.0 OR GPL-2.0-or-later** - taken here under the GPL arm |

The "or later" matters. Apache-2.0 and GPL-2.0-only are not compatible, so if
mbedTLS were Apache-only this combination could not be distributed at all. It is
dual-licensed, so this build takes it under GPL-2.0-or-later and everything lines
up: **the linked binary is GPL-2.0-or-later.**

`LICENSE` at the repository root is MIT and covers the source in `src/`,
`tests/`, `tools/` and `packaging/` - that code stays MIT and can be reused on
its own. It is only the *binary*, which contains all four, that is GPL.

If you distribute that binary, GPL-2.0 section 3 applies: you have to offer the
corresponding source for the three libraries as well. They are unmodified
upstream releases and `tools/fetch-vendor.sh` records the exact versions and
URLs, but "someone else's download page" is not by itself a valid offer - keep a
copy of the three tarballs alongside whatever you ship.

## Patents

faad2's own headers carry this:

> Commercial non-GPL licensing of this software is possible.
> For more info contact Nero AG through Mpeg4AAClicense@nero.com.

Decoding AAC may be covered by patents in some jurisdictions, and shipping a
decoder is not the same as having a licence to use it commercially. Distributing
this under the GPL, as source or as a hobbyist binary for a twenty-year-old Mac,
is what every Linux distribution has done with faad2 for years - but it is worth
knowing the line is there. MP3's patents expired in 2017, so libmad carries no
equivalent question.
