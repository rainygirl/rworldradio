# Verifying the port on a real big-endian PowerPC

The Tiger build can only be checked on Tiger: Carbon, CoreAudio and the
Darwin/PowerPC linker have no substitute anywhere else. What *can* be checked
elsewhere is the other half of the port - and it is the half most likely to be
silently wrong when code written on a little-endian machine is compiled for a
big-endian one.

`verify.sh` cross-compiles that half for 32-bit big-endian PowerPC and runs it
under `qemu-system-ppc` on an emulated Power Mac with a **7450** CPU - the same
processor family as the iMac G4 this port targets, AltiVec included.

```sh
brew install qemu llvm lld
tools/fetch-vendor.sh
make dataset && make castore
tools/ppc-verify/verify.sh            # core + decode
tools/ppc-verify/verify.sh core       # just tests/test_core.c
```

The guest is Debian's PowerPC kernel and glibc, not Tiger. That is the point
where the analogy stops: this proves nothing about Carbon or CoreAudio.

## What it checks

| | |
|---|---|
| `tests/test_core.c` | all of it, on big-endian: the mmap dataset reader, the MPEG-TS demuxer, the HLS playlist parser, the packed-audio/ID3 handling, format detection, the DER CA store walk, the producer/consumer byte pipe and the audio ring buffer |
| `vendor/mbedtls_config.h` | that mbedTLS builds with it for PowerPC - `check_config.h` fails the compile on an inconsistent feature set |
| libmad `FPM_PPC` | that the PowerPC `mulhw`/`mulhwu` fixed-point path is actually selected, and that it decodes MP3 **bit-identically** to the portable path |
| faad2 `WORDS_BIGENDIAN` | that AAC decodes to the same audio. This is the one that matters most: `WORDS_BIGENDIAN` is what gives `libfaad/bits.h` its `ARCH_IS_BIG_ENDIAN`, and getting it wrong makes the bitstream reader byte-swap when it should not - which yields noise rather than an error |
| `-mcpu=7450` codegen | the binaries contain AltiVec instructions and run without a `SIGILL` on the emulated 7450 |

## How the pieces fit

There is no PowerPC Darwin toolchain available, so:

- **Compiler**: Homebrew's `llvm`, whose clang has the PowerPC backend that Apple
  clang does not. `lld` links it.
- **Sysroot**: glibc, kernel headers and the gcc runtime, unpacked straight from
  Debian 8 `.deb` files - jessie is the last release with `powerpc` as an
  official architecture, so those URLs are stable on `archive.debian.org`. No
  Debian install is performed.
- **Guest**: Debian's own `vmlinux` plus an initramfs holding one statically
  linked binary as PID 1 and the data files. Booting takes seconds rather than
  the hour a netinst under emulation would.
- **Console**: `/dev/console` has to be a real character device node in the
  initramfs or the kernel gives PID 1 no stdout and the run is silent. Creating
  one needs root, so `mkcpio.py` writes the cpio archive by hand and sets the
  mode and rdev fields directly.
- **Machine**: `-M mac99 -cpu 7450`. The default `g3beige` is a G3, which has no
  AltiVec - a `-mcpu=7450` binary dies there with a program exception.

## Reading the decode comparison

`decode_probe` decodes the same MP3 and AAC on the host and in the guest and
prints frame counts, sample rate, channels, an RMS, a peak, eight samples from
the middle of the stream and two checksums. `compare.py` diffs them.

One field is expected to differ, and only one: the 16-bit `checksum` for **AAC**.
faad2 is floating point, ARM64 contracts multiply-add where PowerPC does not, and
a single last-bit difference propagates through the rotate-XOR. `coarse` is the
same checksum quantized to 9 bits - still order-sensitive across all 180,224
samples, but tolerant of that last bit - and it must match. So must the RMS, the
peak and the eight middle samples.

MP3 has no such allowance: libmad is fixed-point, so `FPM_PPC` and the portable
path must agree bit for bit.

## What this found

Both of these would have failed on Tiger and neither was reachable by any other
check here:

1. **faad2 did not compile at all.** Without a `config.h`, `libfaad/common.h`
   falls back to its own `typedef signed long int32_t`, which collides with the
   system header. The Makefile was compiling it that way.
2. **`WORDS_BIGENDIAN` never reached faad2.** It was set in the Makefile's
   `CPPFLAGS_COMMON`, which only the app's own sources use. AAC would have
   decoded to noise on the target while building cleanly.

Both are fixed - `tools/fetch-vendor.sh` now writes a `config.h` for faad2 the
way it already did for libmad, and `FAAD2_CPPFLAGS` picks it up. A third,
smaller one turned up on the way: faad2 2.10.0 ships a stray `#error` right after
`#define sin sinf` in `libfaad/common.h`, so `HAVE_SINF` must be left undefined.
