# Notes for agents and contributors

Engineering notes for the macOS port. The README is install-only.

## Why this is a thin directory

Almost none of a port like this is platform-specific. The dataset parser,
HTTP/TLS, the HLS pipeline, the MPEG-TS demuxer, the ADTS normalizer, symphonia
decoding, the cpal output path and the entire egui UI are the shared crate in
[`../shared/core`](../shared/core/AGENTS.md), which this crate depends on by
path. cpal selects CoreAudio instead of ALSA by itself, so audio needed no code
here at all.

What genuinely differs, and is therefore all that lives here:

| File | What's macOS-specific |
|---|---|
| `src/paths.rs` | Dataset search order: the `.app` bundle's `Contents/Resources`, then `~/Library/Application Support/RWorldRadio` - instead of XDG's `~/.local/share` and `/usr/share`. The shared crate's relative fallbacks (`../shared/data`) come last. |
| `src/fonts.rs` | Hiragino / Apple SD Gothic Neo / PingFang / Arial Unicode, layered by script. macOS has no fontconfig to ask and no single font covering all of CJK. |
| `src/mediakeys.rs` | The F8 play/pause key. It never reaches the window as a key event - macOS routes it through MediaPlayer's `MPRemoteCommandCenter` to whichever app last published Now Playing state. |
| `src/main.rs` | Cmd+Q and Cmd+W. winit creates no `NSMenu`, so without this the standard quit shortcut does nothing. |
| `packaging/`, `install.sh` | `.app` bundle, `Info.plist`, `.icns` - instead of a `.desktop` file and hicolor PNGs. |

`default-features = false` on the dependency drops the Linux windowing features
(`x11`, `wayland`), which are inert on macOS but should not be requested here.

## The dataset at runtime

`data/` is located at runtime, checked in this order: the enclosing `.app`
bundle's `Contents/Resources/data`, then next to the binary and up to three
levels above it (`data` and `shared/data` at each level, which covers
`cargo run` and `target/release/`), then
`~/Library/Application Support/RWorldRadio/data`,
`/Library/Application Support/RWorldRadio/data`, and finally `../shared/data`,
`../data` and `data`. Hover the status bar to see which one was used.

`install.sh` copies the dataset into `Contents/Resources/data`, so the installed
app does not depend on this checkout still existing, and dragging it to the Trash
removes everything. That is the macOS convention, and simpler than the Linux
port's XDG split.

## Signing and the icon

`install.sh` ad-hoc signs the bundle (`codesign --sign -`). That is required on
Apple silicon, where an unsigned binary will not execute at all, and `lipo` +
copying into a bundle invalidates the signature the linker applies.

The app icon lives in `packaging/rworldradio.icns`, and is also compiled into the
binary by the shared crate's `icon.rs` so the Dock shows it when running outside
a bundle (`cargo run`). Both come from `../shared/core/packaging/make-icons.py`,
which draws the icon in code:

```
cd ../shared/core/packaging && python3 make-icons.py --icns ../../../mac/packaging/rworldradio.icns
```

## Using it

- Type in either search box to filter; click a country, double-click a station to
  play (or select one and press ▶). The ■ button appears only while something is
  playing.
- The LED bar is the peak level of the audio actually being played.
- Hover a station for its codec, bitrate, language, coordinates and stream URL.
- F8 (the play/pause media key) toggles playback, as do the Control Centre and
  headphone transport controls. With nothing selected it does nothing - there is
  no sensible default station in a 51k-station list.
- Cmd+Q or Cmd+W quits.

## Stream support and limitations

Identical to the Linux port, since it is the same code - progressive
icecast/shoutcast (MP3, AAC, Ogg/Vorbis, FLAC) and HLS, with both MPEG-TS and
packed-audio segments, plus redirects and `.m3u`/`.pls` indirection followed to
whatever actually serves audio.

Not supported: **HE-AAC / AAC+** plays but only its core layer is decoded, so it
sounds duller and runs at half the nominal sample rate; **Opus** does not play
(Ogg/Vorbis does); and **encrypted HLS (`#EXT-X-KEY`) and fragmented MP4/CMAF
segments** do not play.

## Tests

```
cargo test                      # this crate: dataset paths and font discovery
cd ../shared/core && cargo test # the shared implementation
```

`src/paths.rs` tests assert the macOS search order and that the checkout's
dataset (`../shared/data`) is actually found. `src/fonts.rs` tests assert that at
least one of the hardcoded font paths exists on the machine and that it loads -
if a future macOS moves those files, that test is where it gets noticed rather
than the app quietly losing CJK rendering.

## Diagnosing a station that won't play

The two diagnostic tools live in the shared crate:

```
cd ../shared/core
cargo run --example probe_stream -- "BBC Radio 4" 5    # decode only, no audio device
cargo run --example play_stream  -- "BBC Radio 4" 8    # decode and play through CoreAudio
```

If `probe_stream` works and `play_stream` doesn't, the problem is audio output,
not the stream.

## Verification status

Built and run on macOS 15 (Apple silicon, arm64):

- `cargo build --release` clean, `cargo test` passes here and in the shared crate.
- The `.app` bundle assembles, `plutil -lint` passes on the Info.plist,
  `codesign -dv` reports a valid ad-hoc signature, and macOS resolves the radio
  icon for the bundle (queried through `NSWorkspace.iconForFile`).
- The app runs from the bundle, loads all 229 countries / 51,443 stations, and
  played a station end to end through CoreAudio (TBS eFM 101.3, AAC 414kbps) with
  the level meter tracking it. Japanese and Korean station names render.
- `play_stream` verified both stream paths through CoreAudio: a progressive MP3
  station (peak 0.228) and an HLS/packed-ADTS-AAC station (peak 0.439).
- The F8 media key starts and stops the selected station from the installed
  bundle.
- After the move to `../shared/core`: `cargo test` passes in both crates and
  `install.sh --build-only` assembles the bundle with the dataset inside.

Not verified: Intel (x86_64) macOS, and the `--universal` build - there is no
architecture-specific code, but neither has been run.
