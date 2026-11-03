# R World Radio - Mac OS X 10.4 PowerPC
#
#   make            build/rworldradio
#   make bundle     build/RWorldRadio.app
#   make check      run the portable core's tests with the host compiler
#   make dataset    data/stations.dat from a JSON dataset (see DATASET_SRC)
#   make castore    data/cacert.der from a PEM CA bundle
#   make clean
#
# The default target needs the Tiger toolchain (Xcode 2.5, GCC 4.0.1) and the
# three libraries in vendor/ - run tools/fetch-vendor.sh first. `make check`
# needs none of that and runs anywhere with a C compiler, which is what makes the
# protocol code testable off the target.

# The default goal is the first target in the file, and the icon rules further
# down would otherwise claim it - `make` would build the icons and stop. Naming
# `all` here reserves it; its prerequisites are filled in under "rules" below.
# GNU make 3.81 has .DEFAULT_GOAL for this, but Tiger ships 3.80.
all:

# ---------------------------------------------------------------- toolchain --

# `CC ?= gcc-4.0` would NOT work here: make predefines CC, so its origin is
# `default` rather than `undefined` and ?= silently keeps `cc`. On Tiger with
# Xcode 2.5 that happens to be the same compiler, which is exactly why the bug
# would go unnoticed until someone builds with a newer Xcode installed.
ifeq ($(origin CC),default)
CC = gcc-4.0
endif
SDK       ?= /Developer/SDKs/MacOSX10.4u.sdk
DEPLOY    ?= 10.4

# Every iMac G4 is a PowerPC 7441/7445/7455, so -mcpu=7450 (GCC's name for the
# whole 745x family) is exact. It gives up G3 compatibility, which this build
# does not claim.
#
# -faltivec enables the AltiVec language extensions. It does NOT make this
# binary vectorized: GCC 4.0.1 has no auto-vectorizer, and `otool -tV` on the
# result finds zero AltiVec instructions. It is kept because it costs nothing
# and is what a hand-written vector routine would need. The AltiVec win this
# port actually gets is Apple's, inside the CoreAudio converter - see
# audio_ca.c.
#
# Not used on purpose: -ffast-math. faad2's SBR stage depends on IEEE semantics
# and misdecodes HE-AAC without them, and the decoders are nowhere near the
# bottleneck anyway: an MP3 at 192 kbps costs about 7% of the CPU on the target.
ARCHFLAGS ?= -arch ppc -mcpu=7450 -mtune=7450 -faltivec

OPT       ?= -O3 -fomit-frame-pointer -falign-loops=16
WARN       = -Wall -Wextra -Wno-unused-parameter
# -fpascal-strings is not implied by -std=gnu99, and without it a "\p..." literal
# is compiled as a plain C string whose first byte is 'p' (0x70). StandardAlert
# reads that as a 112-byte length and prints whatever literals happen to follow
# it in the binary, so the startup error alert came out as one run-on sentence.
CSTD       = -std=gnu99 -fpascal-strings

CPPFLAGS_COMMON = -Isrc
CFLAGS_TARGET   = $(CSTD) $(WARN) $(OPT) $(ARCHFLAGS) \
                  -isysroot $(SDK) -mmacosx-version-min=$(DEPLOY)
# No -prebind: ld ignores it whenever MACOSX_DEPLOYMENT_TARGET is 10.4 or newer
# and says so on every link. Prebinding stopped being used at 10.4.
LDFLAGS_TARGET  = $(ARCHFLAGS) -isysroot $(SDK) -mmacosx-version-min=$(DEPLOY) \
                  -Wl,-syslibroot,$(SDK) -Wl,-dead_strip

# ApplicationServices is where CGImageSource lives on 10.4 (ImageIO was a
# sub-framework of it until later releases); src/icon.c needs it.
FRAMEWORKS = -framework Carbon -framework AudioToolbox -framework AudioUnit \
             -framework ApplicationServices -framework CoreServices \
             -framework CoreFoundation

BUILD = build
OBJ   = $(BUILD)/obj

# ------------------------------------------------------------------ sources --

# Everything that is protocol or data handling rather than platform code. These
# build and are tested on any host - see `make check`.
PORTABLE_SRCS = \
	src/buf.c \
	src/bytequeue.c \
	src/dataset.c \
	src/der.c \
	src/format.c \
	src/m3u8.c \
	src/playlist.c \
	src/segment.c \
	src/stopflag.c \
	src/tsdemux.c \
	src/audio.c

# Needs the target's headers or libraries.
PLATFORM_SRCS = \
	src/audio_ca.c \
	src/decoder.c \
	src/hls.c \
	src/icon.c \
	src/levelmeter.c \
	src/listview.c \
	src/main.c \
	src/net.c \
	src/paths.c \
	src/player.c \
	src/ui.c

SRCS = $(PORTABLE_SRCS) $(PLATFORM_SRCS)
OBJS = $(patsubst src/%.c,$(OBJ)/%.o,$(SRCS))

# ------------------------------------------------------------------- vendor --

VENDOR       = vendor
MBEDTLS_DIR  = $(VENDOR)/mbedtls-2.28.10
LIBMAD_DIR   = $(VENDOR)/libmad-0.15.1b
FAAD2_DIR    = $(VENDOR)/faad2-2.10.0

MBEDTLS_LIB = $(BUILD)/libmbedtls.a
LIBMAD_LIB  = $(BUILD)/libmad.a
FAAD2_LIB   = $(BUILD)/libfaad.a
VENDOR_LIBS = $(MBEDTLS_LIB) $(LIBMAD_LIB) $(FAAD2_LIB)

MBEDTLS_CPPFLAGS = -I$(MBEDTLS_DIR)/include -I$(VENDOR) \
                   -DMBEDTLS_CONFIG_FILE='<mbedtls_config.h>'
MBEDTLS_SRCS     = $(wildcard $(MBEDTLS_DIR)/library/*.c)
MBEDTLS_OBJS     = $(patsubst $(MBEDTLS_DIR)/library/%.c,$(OBJ)/mbedtls/%.o,$(MBEDTLS_SRCS))

# FPM_PPC selects libmad's hand-written PowerPC fixed-point multiply (mulhw /
# mulhwu). Its configure script would find this too, but configure also passes
# -fforce-mem, which GCC 4.0 removed - so the sources are compiled directly.
LIBMAD_CPPFLAGS = -I$(LIBMAD_DIR) -DHAVE_CONFIG_H -DFPM_PPC
LIBMAD_SRCS = $(addprefix $(LIBMAD_DIR)/, \
	version.c fixed.c bit.c timer.c stream.c frame.c synth.c decoder.c \
	layer12.c layer3.c huffman.c)
LIBMAD_OBJS = $(patsubst $(LIBMAD_DIR)/%.c,$(OBJ)/libmad/%.o,$(LIBMAD_SRCS))

# Floating point, not FIXED_POINT: a G4 has a real FPU and fixed-point costs
# audible quality on the SBR stage HE-AAC streams depend on.
#
# -DHAVE_CONFIG_H and -I$(FAAD2_DIR) are not optional. Without them
# libfaad/common.h falls back to its own `typedef signed long int32_t`, which
# collides with the system header and fails to compile - and WORDS_BIGENDIAN,
# which decides whether the bitstream reader byte-swaps, never gets set. The
# config.h is written by tools/fetch-vendor.sh.
FAAD2_CPPFLAGS = -I$(FAAD2_DIR) -I$(FAAD2_DIR)/include -I$(FAAD2_DIR)/libfaad \
                 -DHAVE_CONFIG_H
FAAD2_SRCS     = $(wildcard $(FAAD2_DIR)/libfaad/*.c)
FAAD2_OBJS     = $(patsubst $(FAAD2_DIR)/libfaad/%.c,$(OBJ)/faad2/%.o,$(FAAD2_SRCS))

APP_CPPFLAGS = $(CPPFLAGS_COMMON) $(MBEDTLS_CPPFLAGS) \
               -I$(LIBMAD_DIR) -I$(FAAD2_DIR)/include -Ipackaging

# --------------------------------------------------------------------- icon --

ICON_PNGS  = $(wildcard packaging/rworldradio-*.png)
ICNS       = packaging/rworldradio.icns
ICON_DATA  = packaging/icon_png.h

# Both are generated by a Python 3 script and Tiger only has Python 2.3, so they
# are built on the same modern machine that runs mkdataset.py and mkcastore.py
# and shipped across with the source - see "Two-machine build" in README.md.
# Here they are prerequisites that explain themselves if missing.
icons: $(ICNS) $(ICON_DATA)

$(ICNS): $(ICON_PNGS) packaging/make-icns.py
	@command -v python3 >/dev/null 2>&1 || { \
		echo "$@ is missing and needs python3 - build it on the host machine:"; \
		echo "    make icons"; exit 1; }
	packaging/make-icns.py $@

$(ICON_DATA): packaging/rworldradio-128.png tools/mkicondata.py
	@command -v python3 >/dev/null 2>&1 || { \
		echo "$@ is missing and needs python3 - build it on the host machine:"; \
		echo "    make icons"; exit 1; }
	tools/mkicondata.py $< $@

$(OBJ)/icon.o: $(ICON_DATA)

# -------------------------------------------------------------------- rules --

BINARY = $(BUILD)/rworldradio

.PHONY: all bundle check check-syntax check-sanitize clean dataset castore icns icons play-probe vendor-check

all: $(BINARY)

$(BINARY): $(OBJS) $(VENDOR_LIBS)
	@mkdir -p $(BUILD)
	$(CC) $(LDFLAGS_TARGET) -o $@ $(OBJS) $(VENDOR_LIBS) $(FRAMEWORKS)
	@echo "built $@"

$(OBJ)/%.o: src/%.c | vendor-check
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_TARGET) $(APP_CPPFLAGS) -c -o $@ $<

$(OBJ)/mbedtls/%.o: $(MBEDTLS_DIR)/library/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_TARGET) $(MBEDTLS_CPPFLAGS) -c -o $@ $<

$(OBJ)/libmad/%.o: $(LIBMAD_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_TARGET) $(LIBMAD_CPPFLAGS) -c -o $@ $<

$(OBJ)/faad2/%.o: $(FAAD2_DIR)/libfaad/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_TARGET) $(FAAD2_CPPFLAGS) -w -c -o $@ $<

$(MBEDTLS_LIB): $(MBEDTLS_OBJS)
	@mkdir -p $(BUILD)
	$(AR) rcs $@ $^
	ranlib $@

$(LIBMAD_LIB): $(LIBMAD_OBJS)
	@mkdir -p $(BUILD)
	$(AR) rcs $@ $^
	ranlib $@

$(FAAD2_LIB): $(FAAD2_OBJS)
	@mkdir -p $(BUILD)
	$(AR) rcs $@ $^
	ranlib $@

vendor-check:
	@test -d $(MBEDTLS_DIR) -a -d $(LIBMAD_DIR) -a -d $(FAAD2_DIR) || { \
		echo "vendor/ is empty - run tools/fetch-vendor.sh first"; exit 1; }

# ------------------------------------------------------------------- bundle --

APP      = $(BUILD)/RWorldRadio.app
APP_EXEC = $(APP)/Contents/MacOS/rworldradio
APP_RES  = $(APP)/Contents/Resources

bundle: $(BINARY) $(ICNS) data/stations.dat data/cacert.der
	@rm -rf $(APP)
	@mkdir -p $(APP)/Contents/MacOS $(APP_RES)
	cp packaging/Info.plist $(APP)/Contents/Info.plist
	printf 'APPL????' > $(APP)/Contents/PkgInfo
	cp $(BINARY) $(APP_EXEC)
	cp data/stations.dat data/cacert.der $(APP_RES)/
	cp $(ICNS) $(APP_RES)/
	@echo "built $(APP)"

# --------------------------------------------------------------------- data --

# The JSON station dataset, which lives in this repository under ../shared/data.
# It is converted to the flat binary the app mmap's - see tools/mkdataset.py for
# why that happens at build time rather than at every launch. Point DATASET_SRC
# somewhere else to build from a different or newer dump.
DATASET_SRC ?= ../shared/data

dataset:
	@mkdir -p data
	tools/mkdataset.py $(DATASET_SRC) data/stations.dat

castore:
	@mkdir -p data
	tools/mkcastore.py

icns: $(ICNS)

data/stations.dat:
	@echo "data/stations.dat is missing - run: make dataset"; exit 1

data/cacert.der:
	@echo "data/cacert.der is missing - run: make castore"; exit 1

# -------------------------------------------------------------------- tests --

# Built with the *host* compiler, not the Tiger one. Everything in
# PORTABLE_SRCS is protocol and data handling with no platform dependency, which
# is the half of the port where the bugs that matter live - the HLS pipeline, the
# TS demuxer, the format detection, the dataset reader, the byte pipe. Running it
# here means a change can be checked without a G4 on the desk.
CHECK_CC     ?= cc
CHECK_CFLAGS ?= -std=gnu99 $(WARN) -g -O1 -Isrc
CHECK_BIN     = $(BUILD)/test_core

check: $(CHECK_BIN)
	$(CHECK_BIN) data/stations.dat data/cacert.der

$(CHECK_BIN): tests/test_core.c $(PORTABLE_SRCS)
	@mkdir -p $(BUILD)
	$(CHECK_CC) $(CHECK_CFLAGS) -o $@ $^

# hls.c, player.c and paths.c are listed as platform sources because they are
# only ever built for the target - but they depend on nothing beyond POSIX and
# <mach-o/dyld.h>, so they do compile here. Type-checking player.c in particular
# is worth having: it holds the session lifecycle and the playlist-following
# logic. They are compiled, not linked; net.c and decoder.c cannot be.
check-syntax:
	@mkdir -p $(BUILD)/syntax
	@for f in hls player paths; do \
		echo "$(CHECK_CC) -c src/$$f.c"; \
		$(CHECK_CC) $(CHECK_CFLAGS) -c -o $(BUILD)/syntax/$$f.o src/$$f.c || exit 1; \
	done
	@echo "hls.c, player.c and paths.c compile clean"

# The same tests with the sanitizers on. Worth running after touching anything
# in bytequeue.c or audio.c, which are the only lock-holding code here.
check-sanitize:
	@mkdir -p $(BUILD)
	$(CHECK_CC) $(CHECK_CFLAGS) -fsanitize=address,undefined \
		-o $(BUILD)/test_core_asan tests/test_core.c $(PORTABLE_SRCS)
	$(BUILD)/test_core_asan data/stations.dat data/cacert.der
	$(CHECK_CC) $(CHECK_CFLAGS) -fsanitize=thread \
		-o $(BUILD)/test_core_tsan tests/test_core.c $(PORTABLE_SRCS)
	$(BUILD)/test_core_tsan data/stations.dat data/cacert.der

# Plays one station end to end - network, TLS, decoder, CoreAudio - with no
# window, which is the only way to check those on a machine reached over SSH.
# See tests/play_probe.c.
PROBE_OBJS = $(filter-out $(OBJ)/main.o $(OBJ)/ui.o $(OBJ)/listview.o \
                          $(OBJ)/levelmeter.o $(OBJ)/icon.o $(OBJ)/paths.o, $(OBJS))

play-probe: $(BUILD)/play_probe

$(BUILD)/play_probe: tests/play_probe.c $(OBJS) $(VENDOR_LIBS)
	$(CC) $(CFLAGS_TARGET) $(APP_CPPFLAGS) $(LDFLAGS_TARGET) -o $@ \
		tests/play_probe.c $(PROBE_OBJS) $(VENDOR_LIBS) $(FRAMEWORKS)
	@echo "built $@ - try: $@ data/stations.dat data/cacert.der Germany"

clean:
	rm -rf $(BUILD)
