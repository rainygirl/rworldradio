#!/bin/bash
# Runs this port's tests on a real big-endian PowerPC, under emulation.
#
# The Tiger build itself cannot be checked anywhere but on Tiger - Carbon,
# CoreAudio and the Darwin/PPC linker have no substitute. What this does check is
# the other half, and the half most likely to be silently wrong when a port moves
# from a little-endian host to a big-endian target:
#
#   * the whole portable core (tests/test_core.c - dataset mmap reader, MPEG-TS
#     demuxer, HLS playlist parser, format detection, DER CA store walk, the
#     producer/consumer byte pipe and the audio ring buffer)
#   * that mbedTLS builds with vendor/mbedtls_config.h on a PowerPC target
#   * that libmad's FPM_PPC fixed-point path is selected and decodes MP3
#     bit-identically to the portable path
#   * that faad2 with WORDS_BIGENDIAN decodes AAC to the same audio
#
# The guest is Linux, not Tiger, so this proves nothing about Carbon. It does
# prove the byte-order and PowerPC-codegen assumptions, which is what nothing
# else here can reach.
#
# Requirements on the host (a modern Mac):
#   brew install qemu llvm lld
#   tools/fetch-vendor.sh      (for the decoder check)
#   make dataset && make castore
#
# Everything downloaded is cached under the work directory; re-running is cheap.
#
#   tools/ppc-verify/verify.sh [core|decode|all]

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
WORK="${RWR_PPC_WORK:-$REPO/build/ppc-verify}"
WHAT="${1:-all}"

LLVM_BIN=/opt/homebrew/opt/llvm/bin
LLD_BIN=/opt/homebrew/opt/lld/bin
export PATH="$LLVM_BIN:$LLD_BIN:$PATH"

SR="$WORK/sysroot"
KERNEL="$SR/boot/vmlinux-3.16.0-6-powerpc"

# Debian 8 (jessie) is the last release with powerpc as an official
# architecture, so these URLs are stable on archive.debian.org. The gcc runtime
# is needed because clang emits calls into libgcc and expects crtbegin/crtend.
DEB_BASE=https://archive.debian.org/debian/pool/main
DEBS=(
  "$DEB_BASE/g/glibc/libc6_2.19-18+deb8u10_powerpc.deb"
  "$DEB_BASE/g/glibc/libc6-dev_2.19-18+deb8u10_powerpc.deb"
  "$DEB_BASE/l/linux/linux-libc-dev_4.9.88-1+deb9u1~bpo8+1_powerpc.deb"
  "$DEB_BASE/l/linux/linux-image-3.16.0-6-powerpc_3.16.56-1+deb8u1_powerpc.deb"
  "$DEB_BASE/g/gcc-4.9/gcc-4.9-base_4.9.2-10+deb8u1_powerpc.deb"
  "$DEB_BASE/g/gcc-4.9/libgcc-4.9-dev_4.9.2-10+deb8u1_powerpc.deb"
)

need() {
    command -v "$1" >/dev/null 2>&1 || { echo "missing $1 - $2" >&2; exit 1; }
}

need qemu-system-ppc "brew install qemu"
need "$LLVM_BIN/clang" "brew install llvm"
need "$LLD_BIN/ld.lld" "brew install lld"

# ------------------------------------------------------------- sysroot ---

if [ ! -f "$KERNEL" ]; then
    echo "== building the PowerPC sysroot =="
    mkdir -p "$WORK/debs" "$SR"
    for url in "${DEBS[@]}"; do
        f="$WORK/debs/$(basename "$url")"
        [ -f "$f" ] || curl -sS --fail --max-time 600 -o "$f" "$url"
        ar p "$f" data.tar.xz 2>/dev/null | tar xJf - -C "$SR" 2>/dev/null ||
        ar p "$f" data.tar.gz 2>/dev/null | tar xzf - -C "$SR" 2>/dev/null
        echo "  unpacked $(basename "$f")"
    done
fi

# -mcpu=7450 is what the Tiger Makefile targets, so this exercises the same
# instruction selection - AltiVec included, which is why the guest CPU below is
# a 7450 and not the default G3.
CC=(clang --target=powerpc-unknown-linux-gnu -mcpu=7450
    --sysroot="$SR" -static -std=gnu99 -O2
    -isystem "$SR/usr/include/powerpc-linux-gnu" -isystem "$SR/usr/include")
LINK=(-fuse-ld=lld
      -B"$SR/usr/lib/gcc/powerpc-linux-gnu/4.9"
      -L"$SR/usr/lib/gcc/powerpc-linux-gnu/4.9"
      -L"$SR/usr/lib/powerpc-linux-gnu" -L"$SR/lib/powerpc-linux-gnu")

CORE_SRCS=(
  src/buf.c src/bytequeue.c src/dataset.c src/der.c src/format.c
  src/m3u8.c src/playlist.c src/segment.c src/stopflag.c src/tsdemux.c
  src/audio.c
)

# Boots the guest with `image` as its initramfs and prints everything between
# the two markers the init prints.
boot() {
    local image=$1 begin=$2 end=$3 log="$WORK/qemu.log"
    rm -f "$log"
    qemu-system-ppc -M mac99 -cpu 7450 -m 512 \
        -kernel "$KERNEL" -initrd "$image" \
        -append "console=ttyPZ0 rdinit=/init panic=1" \
        -nographic -no-reboot > "$log" 2>&1 &
    local qemu=$!
    # stdout is redirected away from the watchdog on purpose: it outlives the
    # kill below (killing the subshell leaves its sleep running) and would
    # otherwise hold this script's stdout open long after the run is done.
    ( sleep 900; kill -9 "$qemu" 2>/dev/null ) >/dev/null 2>&1 &
    local watchdog=$!
    wait "$qemu" 2>/dev/null || true
    pkill -P "$watchdog" 2>/dev/null || true
    kill "$watchdog" 2>/dev/null || true

    if ! grep -q "$end" "$log"; then
        echo "the guest did not finish - last lines of $log:" >&2
        tail -20 "$log" >&2
        return 1
    fi
    sed -n "/$begin/,/$end/p" "$log" | tr -d '\r'
}

cd "$REPO"

# ---------------------------------------------------------------- core ---

if [ "$WHAT" = core ] || [ "$WHAT" = all ]; then
    echo "== cross-building the portable core for powerpc =="
    O="$WORK/obj-core"; rm -rf "$O"; mkdir -p "$O"
    "${CC[@]}" -Wall -Wextra -Wno-unused-parameter -Isrc \
        -Dmain=rwr_test_main -c tests/test_core.c -o "$O/test_core.o"
    for f in "${CORE_SRCS[@]}"; do
        "${CC[@]}" -Wall -Wextra -Wno-unused-parameter -Isrc \
            -c "$f" -o "$O/$(basename "${f%.c}").o"
    done
    "${CC[@]}" -c "$HERE/ppc_init.c" -o "$O/ppc_init.o"
    "${CC[@]}" "${LINK[@]}" "$O"/*.o -o "$WORK/init-core" -lpthread

    python3 "$HERE/mkcpio.py" "$WORK/core.cpio" \
        "init=$WORK/init-core" \
        "stations.dat=$REPO/data/stations.dat" \
        "cacert.der=$REPO/data/cacert.der" >/dev/null
    gzip -9 -f -c "$WORK/core.cpio" > "$WORK/core.cpio.gz"

    echo "== running on emulated PowerPC 7450 =="
    boot "$WORK/core.cpio.gz" "RWR-BEGIN" "RWR-EXIT"
fi

# -------------------------------------------------------------- decode ---

if [ "$WHAT" = decode ] || [ "$WHAT" = all ]; then
    MBEDTLS="$REPO/vendor/mbedtls-2.28.10"
    LIBMAD="$REPO/vendor/libmad-0.15.1b"
    FAAD2="$REPO/vendor/faad2-2.10.0"
    for d in "$MBEDTLS" "$LIBMAD" "$FAAD2"; do
        [ -d "$d" ] || { echo "missing $d - run tools/fetch-vendor.sh" >&2; exit 1; }
    done

    echo
    echo "== cross-building the vendored libraries for powerpc =="
    O="$WORK/obj-vendor"; rm -rf "$O"; mkdir -p "$O"

    # mbedTLS compiling at all is vendor/mbedtls_config.h being validated:
    # check_config.h at the bottom of it errors on an inconsistent feature set.
    n=0
    for f in "$MBEDTLS"/library/*.c; do
        "${CC[@]}" -w -I"$MBEDTLS/include" -I"$REPO/vendor" \
            -DMBEDTLS_CONFIG_FILE='<mbedtls_config.h>' \
            -c "$f" -o "$O/mbed_$(basename "${f%.c}").o"
        n=$((n + 1))
    done
    echo "  mbedTLS: $n objects"

    n=0
    for m in version fixed bit timer stream frame synth decoder layer12 layer3 huffman; do
        "${CC[@]}" -w -I"$LIBMAD" -DHAVE_CONFIG_H -DFPM_PPC \
            -c "$LIBMAD/$m.c" -o "$O/mad_$m.o"
        n=$((n + 1))
    done
    echo -n "  libmad: $n objects, PowerPC mulhw/mulhwu instructions: "
    llvm-objdump -d "$O/mad_layer3.o" "$O/mad_synth.o" | grep -cE "\bmulhwu?\b"

    n=0
    for f in "$FAAD2"/libfaad/*.c; do
        "${CC[@]}" -w -I"$FAAD2" -I"$FAAD2/include" -I"$FAAD2/libfaad" \
            -DHAVE_CONFIG_H -c "$f" -o "$O/faad_$(basename "${f%.c}").o"
        n=$((n + 1))
    done
    echo "  faad2: $n objects"

    # The fixtures have to be made on the host; lame and afconvert both ship or
    # install easily there, and neither exists in the guest.
    A="$WORK/audio"; mkdir -p "$A"
    if [ ! -f "$A/test.mp3" ] || [ ! -f "$A/test.aac" ]; then
        echo "  generating fixtures"
        python3 "$HERE/mkfixture.py" "$A/test.wav"
        need lame "brew install lame"
        lame --quiet -b 128 -h "$A/test.wav" "$A/test.mp3"
        afconvert -f adts -d aac -b 96000 "$A/test.wav" "$A/test.aac"
    fi

    echo "== building decode_probe for both architectures =="
    # Native reference. libmad's portable path (FPM_64BIT) and faad2 without
    # WORDS_BIGENDIAN - i.e. the same source configured for a little-endian host.
    N="$WORK/obj-native"; rm -rf "$N"; mkdir -p "$N"
    sed 's/^#define WORDS_BIGENDIAN 1/\/* little-endian host *\//' \
        "$FAAD2/config.h" > "$N/faad_config.h"
    for m in version fixed bit timer stream frame synth decoder layer12 layer3 huffman; do
        cc -std=gnu99 -O2 -w -I"$LIBMAD" -DHAVE_CONFIG_H -DFPM_64BIT \
            -c "$LIBMAD/$m.c" -o "$N/mad_$m.o"
    done
    for f in "$FAAD2"/libfaad/*.c; do
        cc -std=gnu99 -O2 -w -I"$FAAD2" -I"$FAAD2/include" -I"$FAAD2/libfaad" \
            -include "$N/faad_config.h" -c "$f" -o "$N/faad_$(basename "${f%.c}").o"
    done
    cc -std=gnu99 -O2 -Wall -Isrc -I"$LIBMAD" -I"$FAAD2/include" \
        tests/decode_probe.c src/buf.c src/bytequeue.c src/decoder.c "$N"/*.o \
        -o "$WORK/decode_probe_native" -lm -lpthread

    for f in src/buf.c src/bytequeue.c src/decoder.c; do
        "${CC[@]}" -Wall -Isrc -I"$LIBMAD" -I"$FAAD2/include" \
            -c "$f" -o "$O/$(basename "${f%.c}").o"
    done
    "${CC[@]}" -Wall -Isrc -I"$LIBMAD" -I"$FAAD2/include" \
        -Dmain=decode_probe_main -c tests/decode_probe.c -o "$O/decode_probe.o"
    "${CC[@]}" -Isrc -c "$HERE/ppc_decode_init.c" -o "$O/ppc_decode_init.o"
    "${CC[@]}" "${LINK[@]}" "$O"/*.o -o "$WORK/init-decode" -lm -lpthread

    python3 "$HERE/mkcpio.py" "$WORK/decode.cpio" \
        "init=$WORK/init-decode" \
        "test.mp3=$A/test.mp3" "test.aac=$A/test.aac" >/dev/null
    gzip -9 -f -c "$WORK/decode.cpio" > "$WORK/decode.cpio.gz"

    "$WORK/decode_probe_native" "$A/test.mp3" mp3 > "$WORK/native.txt"
    echo "---" >> "$WORK/native.txt"
    "$WORK/decode_probe_native" "$A/test.aac" aac >> "$WORK/native.txt"

    echo "== decoding on emulated PowerPC 7450 =="
    boot "$WORK/decode.cpio.gz" "RWR-DECODE-BEGIN" "RWR-DECODE-END" > "$WORK/ppc.txt"

    echo
    python3 "$HERE/compare.py" "$WORK/native.txt" "$WORK/ppc.txt"
fi
