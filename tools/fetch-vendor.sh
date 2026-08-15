#!/bin/sh
# Downloads and unpacks the three vendored libraries into vendor/.
#
# Run this on a machine with a working network and modern TLS, then copy the
# whole checkout to the Tiger machine - curl on 10.4 cannot reach most of these
# hosts any more, for exactly the reason this port bundles mbedTLS in the first
# place.
#
# Nothing here is checked in: the three tarballs are about 4 MB and are
# unmodified upstream releases. See vendor/README.md for why each is used.

set -e

here=$(cd "$(dirname "$0")/.." && pwd)
vendor="$here/vendor"
cd "$vendor"

MBEDTLS_VERSION=2.28.10
LIBMAD_VERSION=0.15.1b
FAAD2_VERSION=2.10.0

MBEDTLS_URL="https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS_VERSION/mbedtls-$MBEDTLS_VERSION.tar.bz2"
LIBMAD_URL="https://downloads.sourceforge.net/project/mad/libmad/$LIBMAD_VERSION/libmad-$LIBMAD_VERSION.tar.gz"
# faad2 left SourceForge; upstream is knik0/faad2 and its tags use underscores,
# so the tarball unpacks as faad2-2_10_0/ and is renamed below.
FAAD2_TAG=$(echo "$FAAD2_VERSION" | tr . _)
FAAD2_URL="https://github.com/knik0/faad2/archive/refs/tags/$FAAD2_TAG.tar.gz"

fetch() {
    url=$1
    file=$2
    if [ -f "$file" ]; then
        echo "have $file"
        return
    fi
    echo "fetching $file"
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 3 -o "$file.part" "$url"
    elif command -v wget >/dev/null 2>&1; then
        wget -O "$file.part" "$url"
    else
        echo "need curl or wget" >&2
        exit 1
    fi
    mv "$file.part" "$file"
}

unpack() {
    archive=$1
    directory=$2
    if [ -d "$directory" ]; then
        echo "have $directory/"
        return
    fi
    echo "unpacking $archive"
    case "$archive" in
        *.tar.bz2) tar xjf "$archive" ;;
        *.tar.gz)  tar xzf "$archive" ;;
    esac
}

fetch "$MBEDTLS_URL" "mbedtls-$MBEDTLS_VERSION.tar.bz2"
fetch "$LIBMAD_URL"  "libmad-$LIBMAD_VERSION.tar.gz"
fetch "$FAAD2_URL"   "faad2-$FAAD2_VERSION.tar.gz"

unpack "mbedtls-$MBEDTLS_VERSION.tar.bz2" "mbedtls-$MBEDTLS_VERSION"
unpack "libmad-$LIBMAD_VERSION.tar.gz"    "libmad-$LIBMAD_VERSION"
unpack "faad2-$FAAD2_VERSION.tar.gz"      "faad2-$FAAD2_VERSION"
if [ -d "faad2-$FAAD2_TAG" ] && [ ! -d "faad2-$FAAD2_VERSION" ]; then
    mv "faad2-$FAAD2_TAG" "faad2-$FAAD2_VERSION"
fi

# libmad 0.15.1b predates GCC 4 and its configure script passes -fforce-mem,
# which GCC 4.0 removed. The Makefile compiles the sources directly and never
# runs configure, so nothing here has to be patched - but libmad still expects a
# config.h to exist.
if [ ! -f "libmad-$LIBMAD_VERSION/config.h" ]; then
    cat > "libmad-$LIBMAD_VERSION/config.h" <<'EOF'
/* Written by tools/fetch-vendor.sh. libmad's own configure is not used - see
 * the Makefile, which sets FPM_PPC and the optimization level directly. */
#define HAVE_ASSERT_H 1
#define HAVE_LIMITS_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define SIZEOF_INT 4
#define SIZEOF_LONG 4
#define SIZEOF_LONG_LONG 8
EOF
    echo "wrote libmad-$LIBMAD_VERSION/config.h"
fi

# faad2 has the same problem as libmad and a worse consequence. Without a
# config.h it falls back to hardcoded typedefs in libfaad/common.h - including
# `typedef signed long int32_t`, which collides with the system's own and stops
# the build outright. And WORDS_BIGENDIAN is what sets ARCH_IS_BIG_ENDIAN, which
# libfaad/bits.h uses to decide whether its bitstream reader byte-swaps: get it
# wrong on PowerPC and AAC decodes to noise rather than failing loudly.
#
# It is hardcoded rather than detected because the target is always big-endian
# PowerPC, whatever machine this script runs on.
if [ ! -f "faad2-$FAAD2_VERSION/config.h" ]; then
    cat > "faad2-$FAAD2_VERSION/config.h" <<'EOF'
/* Written by tools/fetch-vendor.sh. faad2's own configure is not used - the
 * Makefile compiles libfaad/*.c directly - so this supplies what
 * libfaad/common.h probes for. */
#define STDC_HEADERS 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_MEMORY_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_MEMCPY 1
#define HAVE_STRCHR 1

#define PACKAGE_VERSION "2.10.0"

/* C99 float math, all present in Tiger's libm. Two are left out on purpose:
 *
 *   HAVE_SINF - faad2 2.10.0 ships a stray `#error` immediately after
 *               `#define sin sinf` in libfaad/common.h (line 377). Defining it
 *               fails the build outright. Without it `sin` stays double, which
 *               only affects SBR table setup, not the decode loop.
 *   HAVE_LRINTF - faad2 substitutes a plain cast, and that is one fewer thing
 *               to be wrong about on a 2005 toolchain.
 */
#define HAVE_COSF 1
#define HAVE_SQRTF 1
#define HAVE_FLOORF 1
#define HAVE_CEILF 1
#define HAVE_EXPF 1
#define HAVE_LOGF 1

/* The target is a PowerPC G4. This is what gives libfaad/bits.h
 * ARCH_IS_BIG_ENDIAN. */
#define WORDS_BIGENDIAN 1
EOF
    echo "wrote faad2-$FAAD2_VERSION/config.h"
fi

echo
echo "vendor/ is ready. Next:"
echo "  tools/mkcastore.py             # build data/cacert.der"
echo "  tools/mkdataset.py <json-dir> data/stations.dat"
echo "  make"
