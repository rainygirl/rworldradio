# Vendored copy - do not edit here

This directory is an **unmodified copy** of the crate at the root of the Linux
port's repository (`rworldradio-linux`). It holds everything that is not
platform-specific: the dataset parser, HTTP/TLS, the HLS pipeline, the MPEG-TS
demuxer, the ADTS normalizer, symphonia decoding, the cpal output path and the
whole egui UI.

It is copied in rather than referenced with a path dependency so that this
repository builds on its own after a plain `git clone`. A
`path = "../rworldradio-linux/rust"` dependency would only work for someone who
happened to have both repositories checked out side by side.

## Keeping it in sync

Upstream keeps this crate at its repository root, so re-syncing means copying
everything except the dataset and the tooling:

```
rsync -a --delete \
    --exclude target --exclude data --exclude tools --exclude .git \
    --exclude VENDORED.md \
    /path/to/rworldradio-linux/ ./core/
```

Drift is visible with the same exclusions:

```
diff -r --exclude=target --exclude=VENDORED.md \
    /path/to/rworldradio-linux ./core
```

Fix bugs in the Linux repository and re-copy - never edit this directory
directly, or the next re-copy silently reverts the fix.

## What in here is unused by the macOS build

The copy is verbatim, so it carries a few files this repository does not use:

- `main.rs`, `install.sh`, `packaging/rworldradio.desktop`, the hicolor PNGs, the
  screenshot and both READMEs belong to the Linux binary and its packaging. Cargo
  only builds this package's **library** target as a dependency, so `main.rs` is
  never compiled here.
- The `x11` / `wayland` Cargo features are Linux windowing backends. This
  repository depends on the crate with `default-features = false`, so neither is
  enabled.

`packaging/rworldradio-256.png` *is* used - `src/icon.rs` compiles it into the
binary with `include_bytes!`, which is what puts the app icon in the Dock when
running outside an `.app` bundle.
