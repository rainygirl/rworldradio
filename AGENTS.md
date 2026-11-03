# Notes for agents and contributors

Engineering notes for the repository as a whole. The READMEs are install-only;
each port's `AGENTS.md` holds the notes for that port, and this file holds what
is common to all of them.

Written in English to match the source comments.

## What this is

Six ports of one internet radio player, merged from six repositories
(`rworldradio-linux`, `-mac`, `-macppc`, `-os9`, `-winphone` and
`renkuos-rworldradio`) with their history. The original is the RenkuOS (Haiku,
BeAPI) application in `renkuos/`; every other port was made from it or from
another port.

| Directory | Language / toolkit | Shares code with |
|---|---|---|
| `renkuos/` | C++, Be API (Media Kit, Network Kit) | nothing - the original |
| `shared/core/` | Rust: dataset, HTTP/TLS, HLS, MPEG-TS, ADTS, symphonia, cpal, egui | used by `linux/` and `mac/` |
| `linux/` | Rust, a window-setup `main.rs` over `shared/core` | `shared/core` |
| `mac/` | Rust: dataset paths, fonts, media keys, `.app` packaging over `shared/core` | `shared/core` |
| `macppc/` | C, Carbon, CoreAudio, mbedTLS, libmad, faad2 | nothing - a module-for-module rewrite of `shared/core` |
| `os9/` | C, classic Toolbox (Retro68) + a Python relay | nothing - the relay does the protocol work |
| `winphone/` | C#, UWP | nothing - WinRT decodes, only the resolver was translated |

What all six share is the station list in `shared/data/`.

## shared/data

`countries.json` is the index (`[{name, file, count}, ...]`); each
`countries/<slug>.json` is a flat array of station records. 229 countries are
referenced by the index; the remaining files are left in as collected. All six
ports read the same files:

| Port | How it gets to the data |
|---|---|
| `renkuos/` | `StationCache` searches `<exe dir>/data`, `<port>/data`, `<repo>/shared/data`, `~/config/non-packaged/data/RWorldRadio`, `../shared/data`, `data`. The pkgman package copies it next to the binary. |
| `linux/`, `mac/` | `station_cache::exe_relative_candidates()` and `relative_candidates()` in `shared/core` include `shared/data`; `install.sh` copies it to the XDG dir / into the `.app`. |
| `macppc/` | `make dataset` converts it to `data/stations.dat` (see `macppc/AGENTS.md`); `DATASET_SRC` defaults to `../shared/data`. |
| `os9/` | `relay/radio_relay.py --data` defaults to `../../shared/data` relative to the script. |
| `winphone/` | The `.csproj` lists every file as `<Content Include="..\shared\data\...">` with a `<Link>` under `Assets\data`, so the package layout is unchanged. |

To refresh it:

```
python3 shared/tools/update_stations_db.py
```

It fetches radio-browser's catalogue and rewrites `shared/data`. Afterwards:
re-run the Rust ports' `install.sh`, `make dataset` for the PowerPC port, restart
the OS 9 relay, and regenerate the `<Content>` list in `winphone/RWorldRadio.csproj`
if country files were added or removed (`winphone/AGENTS.md`).

## shared/core

The Rust crate used to be the Linux repository itself, with the macOS repository
carrying a verbatim copy in `core/`. It is now a library-only crate
(`[lib] rworldradio`) that both `linux/` and `mac/` depend on by path, so a fix
lands in both ports at once and there is nothing to re-sync. Its own notes are in
[`shared/core/AGENTS.md`](shared/core/AGENTS.md), including the invariants the
protocol code must keep and the platform seams `mac/` depends on.

`shared/core/packaging/` holds the icon PNGs and `make-icons.py` that draws them;
`icon.rs` compiles the 256 px one in, and `linux/install.sh` installs the hicolor
sizes from there. `mac/packaging/rworldradio.icns` is generated from the same script.

There is no Cargo workspace: `shared/core`, `linux` and `mac` each have their own
`Cargo.lock` and `target/`, because the Linux crate's windowing features and the
macOS crate's MediaPlayer bindings cannot both be built on one machine.

## Building each port

| Port | Where it builds | Check |
|---|---|---|
| `shared/core` | any Linux or macOS with Rust 1.88+ | `cargo test` (56 unit tests) |
| `linux/` | Linux; also compiles on macOS | `cargo build --release`, `./install.sh` |
| `mac/` | macOS 10.15+ | `cargo test`, `./install.sh --build-only` |
| `macppc/` | generators on a modern Mac, compile on Tiger | `make check` on the host (162 checks), `tools/ppc-verify/verify.sh` under QEMU |
| `os9/` | Retro68 in Docker (`make`), relay anywhere with Python 3 + ffmpeg | run on the Mac |
| `winphone/` | Windows with VS2017 Build Tools | on the phone only |
| `renkuos/` | Haiku (`make`); x86_64/arm64 cross-built by `tools/cross-build.sh` | `test/` programs, run on Haiku |

## Publishing the RenkuOS package

`pkgman.rainygirl.com` is built from the `pkgman-repo` project
(`~/Workspace/renkuos-apps/pkgman-repo`), whose `recipes/rworldradio.recipe` has
`SOURCE_DIR` pointing at this repository and `cd`s into `renkuos/`. The recipe's
`INSTALL` copies `../shared/data` next to the binary. x86_gcc2 is built natively
on a 32-bit RenkuOS machine with `scripts/build.sh rworldradio`; x86_64 and arm64
take the binaries from `renkuos/dist/<arch>/` that `renkuos/tools/cross-build.sh`
produces. See the pkgman-repo README for indexing and deploying.

## Previous repositories

The six original repositories now carry only a note pointing here. Their history
is in this repository: `git log -- linux/` and so on.
