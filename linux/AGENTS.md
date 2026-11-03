# Notes for agents and contributors

Engineering notes for the Linux port. The README is install-only.

## What is here

Only the window setup: `src/main.rs` builds the eframe `NativeOptions` (title,
size, the `rworldradio` app id that `packaging/rworldradio.desktop` names in
`StartupWMClass`) and hands over to `rworldradio::ui::RadioApp`. Everything else -
dataset, streaming, decoding, audio, the UI - is the shared crate in
[`../shared/core`](../shared/core/AGENTS.md).

```
Cargo.toml                  binary crate "rworldradio-linux", bin "rworldradio"
src/main.rs                 window setup
install.sh                  cargo build --release + XDG install / uninstall
packaging/rworldradio.desktop
```

The hicolor icon PNGs come from `../shared/core/packaging/`; `install.sh`
installs them from there.

## Features

`x11` (default) and `wayland` forward to the same-named features of the shared
crate, which forward to eframe. X11 is the default because winit links
`libwayland-client` at build time when the Wayland backend is on, which would
add `libwayland-dev` to the build requirements of an X11-only desktop.

## Using it

- Type in either search box to filter. With 229 countries and ~51,000 stations,
  search is how you get around.
- Click a country, then double-click a station to play it - or select one and
  press the ▶ button. The ■ button appears only while something is playing.
- The LED bar shows the level of the audio actually playing.
- Hover a station to see its codec, bitrate, language, location and stream URL;
  hover the status text on the right to see which station list is loaded and
  from where.

## When a station will not play

Stations come and go, and a fair number of entries in any public directory are
dead. Two tools in the shared crate tell you which side the problem is on:

```
cd ../shared/core
cargo run --release --example probe_stream -- "BBC Radio 4" 5   # no audio device used
cargo run --release --example play_stream  -- "BBC Radio 4" 8   # plays through ALSA
```

Both accept a station name (or part of one), or a URL directly.

- `probe_stream` fails: the station is dead, blocked in your region, or in a
  format that is not supported. The message says which.
- `probe_stream` works but `play_stream` does not: the problem is audio output,
  not the station. Check that PulseAudio or PipeWire is running.
- Both work but the app is silent: check the volume and which output device the
  desktop has selected.

## What is not supported

- **HE-AAC / AAC+ stations play, but sound duller than they should.** Only the
  core layer is decoded (symphonia has no SBR), so the high band is missing and
  the sample rate is typically half the nominal one.
- **Opus streams do not play.** Ogg/Vorbis does.
- **Encrypted HLS streams (`#EXT-X-KEY`) and fragmented MP4/CMAF segments do not
  play.**
- A few stations refuse connections from anything that does not look like a
  browser, or are restricted to their own country.

## Building on a small machine

On a machine with 2 cores and ~4GB of RAM the release profile's thin LTO makes
the link step the memory peak. Build without it - the binary is slightly larger
and that is the only difference:

```
CARGO_PROFILE_RELEASE_LTO=false CARGO_BUILD_JOBS=2 nice cargo build --release
```

## How this was verified

- **Debian 13 aarch64 under QEMU**: release build and all tests pass; the GUI
  runs under X11 with Mesa's software rasterizer; playback captured through the
  ALSA loopback device (`snd-aloop`) matched the level meter's reading.
- **Linux Mint 20.3 x86_64, real hardware**: build 10m24s with LTO disabled,
  tests pass, `install.sh` verified end to end including `desktop-file-validate`.
- The directory split (this crate over `../shared/core`) was verified by
  building the release binary; the per-port `install.sh` was not re-run after it.
