# Notes for agents and contributors

Engineering notes for the RenkuOS (HaikuOS) port - the original from which every
other port in this repository was made. The README is install-only.

## What this is

A native internet radio player for RenkuOS / Haiku (BeAPI). It uses
Haiku-specific APIs (Media Kit, the classic Network Kit, app_info/BEntry,
BAdapterIO) directly and will not build or run anywhere else.

## Layout

```
src/
  JsonValue.h/.cpp           minimal JSON parser (no third-party dependency)
  Station.h                  station record
  DataSetRepository.h/.cpp   parses countries.json + countries/*.json
  StationCache.h/.cpp        locates the data directory and loads it
  NetworkFetch.h/.cpp        blocking HTTP GET via BUrlProtocolRoster - used only to resolve a TuneIn station's Tune.ashx link at play time
  HttpClient.h/.cpp          HTTP/HTTPS client (TLS through OpenSSL when the build host has it - HAIKU_HAS_OPENSSL)
  HttpAudioIO.h/.cpp         progressive stream as a BPositionIO
  M3u8Parser.h/.cpp          HLS playlist parser (master + media playlists)
  TsDemuxer.h/.cpp           MPEG-TS demuxer -> raw ADTS AAC / MPEG-audio elementary stream
  HlsAdapterIO.h/.cpp        BAdapterIO subclass: fetches/demuxes a live HLS stream and feeds it to BMediaFile like a plain progressive stream
  RadioPlayer.h/.cpp         BMediaFile/BMediaTrack/BSoundPlayer playback - BUrl directly for plain streams, HlsAdapterIO for .m3u8 ones
  StreamSniffer.h/.cpp, Mp3StreamDecoder, AacStreamDecoder
                             the arm64 in-process decoders (see THIRD-PARTY-NOTICES.md)
  LevelMeterView.h/.cpp      peak-level bar next to the now-playing text
  MainWindow.h/.cpp          BWindow: country list, station list, now-playing/status
  App.h/.cpp, main.cpp       BApplication entry point
  app.rdef                   signature, catalog name, icon
  thirdparty/                minimp3 and the FDK-AAC decode subset (arm64 build only)
test/
  test_dataset_repository.cpp   standalone test against ../../shared/data
  test_m3u8_parser.cpp          standalone test, real m3u8 master/media playlists
tools/
  cross-build.sh            cross-compiles dist/<arch>/rworldradio for x86_64 or arm64 (Docker) - used by the pkgman recipe
Makefile                    Haiku makefile-engine build file (x86_gcc2 hybrid and x86_64 native)
build-arm64.sh              native build on arm64 RENKU, which has no makefile-engine
install.sh                  builds and links the app into Deskbar's Applications menu
```

## Where the station list is looked for

`StationCache` checks, in order: `<dir of the executable>/data`, one level up
from it (`<port>/data`), two levels up plus `shared/data` (this repository's
`shared/data`), `~/config/non-packaged/data/RWorldRadio`, `../shared/data`
relative to the current directory, and a plain `data`. The pkgman package
copies the dataset next to the binary; `install.sh` links
`~/config/non-packaged/data/RWorldRadio` to `../shared/data`.

## Building on Haiku

```
make
```

Produces the `rworldradio` binary under `objects.<arch>-<compiler>-release/`.
On an x86_gcc2 hybrid the engine is C++11, so it is built with the x86 secondary
toolchain (`setarch x86 make`), which is what the pkgman recipe does.

If the linker complains about undefined references, check the `LIBS` /
`SYSTEM_INCLUDE_PATHS` comments in `Makefile` first - this Haiku build put the
classic Url Kit (`BUrlRequest`/`BHttpRequest`) under `private/netservices` inside
a `BPrivate::Network` namespace, and its implementation is a static archive
(`libnetservices.a` + `libshared.a`) that isn't linked automatically the way
`libbe.so` is. These specifics may differ across Haiku releases.

TLS: the makefile compiles HTTPS support in only when the build host has
OpenSSL headers and `libssl.so`; the cross builds always do, so their packages
require `lib:libssl` and `lib:libcrypto`.

## Deskbar's Applications menu (what `install.sh` does)

Deskbar's Applications menu is a plain directory of symlinks at
`/boot/system/data/deskbar/menu/Applications/`, but that path is read-only
(packagefs) - a non-packaged addition goes in the mirrored
`/boot/system/non-packaged/data/deskbar/menu/Applications/` directory instead,
which packagefs merges into the read-only view. Confirmed on a real system: that
merge is not picked up live, even after restarting Deskbar
(`quit application/x-vnd.Be-TSKB`) - a reboot is needed the first time this
directory is created. Re-running `ln -sf` on an existing link recreates the inode
and un-merges it again until the next reboot, so `install.sh` only touches the
link when it does not already point at the right target.
`~/config/non-packaged/apps/` does *not* drive this menu, despite what you might
read elsewhere.

## Cross builds and the arm64 decoders

`tools/cross-build.sh x86_64|arm64` builds `dist/<arch>/rworldradio` in Docker
(the `haiku/cross-compiler:x86_64-r1beta4` image, or the `haiku-builder`
container that holds a Haiku arm64 build tree). The arm64 image ships an empty
`/boot/system/add-ons/media/plugins`, so the Media Kit cannot decode anything
there; that build decodes MP3 (minimp3) and AAC (FDK-AAC) in-process -
see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). The x86 builds use the
system's media add-ons and include neither decoder.

## Publishing

The `pkgman-repo` recipe (`recipes/rworldradio.recipe`) builds the x86_gcc2
package natively on a 32-bit RenkuOS machine and packages the cross-built
binaries for x86_64 and arm64; its `INSTALL` copies `../shared/data` next to the
binary and installs the READMEs. Package names: `rworldradio` (x86_64, arm64)
and `rworldradio_x86` (x86_gcc2 hybrid). See the root `AGENTS.md`.

## Known risk areas

- `libnetservices.a`/`libshared.a` paths in `Makefile` are hardcoded to this
  Haiku build's SDK layout; adjust if yours differs.
- The dataset must be reachable through one of the paths above - a copy of the
  binary alone fails with "data/countries.json not found next to the app".
- The HLS pipeline only handles ADTS AAC and MPEG audio inside plain MPEG-TS
  segments - fragmented MP4/CMAF segments and anything encrypted (`#EXT-X-KEY`)
  aren't handled.
- `pkgman add-repo` with `https://` fails with `Operation not supported` on an
  image whose network kit has no TLS; `http://` works there.

## Verification

Built and verified on a real Haiku x86_64 nightly (gcc13) in QEMU and on a
32-bit RenkuOS (x86_gcc2) machine; the arm64 package on a RENKU arm64 image.
