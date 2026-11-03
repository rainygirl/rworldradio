# Notes for agents and contributors

Engineering notes for changing this code. The README is install- and
operation-only; everything about *why the code looks like this* lives here.

Written in English to match the source comments.

## What this is

A rewrite in C of the Rust `rworldradio` crate, targeting Mac OS X 10.4 on a
PowerPC G4. The Rust ports (Linux, macOS 10.15+) share one crate and differ in a
handful of files. This port shares no code with them - see below - but it follows
the same module layout, and the protocol behaviour was ported rather than
redesigned.

Ported from the macOS Rust port (`../mac`, built on `../shared/core`), which
came from the Linux one, which came from the Haiku/BeAPI original (`../renkuos`).

### Why this one is a rewrite and not a port

The other three ports share a Rust crate. This one shares no code with them,
because none of what they are built on exists on this target:

- **rustc has no PowerPC macOS target.** `rustc --print target-list` has 19
  PowerPC entries; every one is Linux, AIX or BSD. There has never been a
  `powerpc-apple-darwin`.
- **eframe/egui, winit, cpal and rustls** all require macOS 10.12 or newer.
  Tiger has no Metal, no `AudioComponentFindNext`, and no `libSystem` symbols
  any of them link against.
- **Tiger's TLS cannot reach modern hosts.** Secure Transport on 10.4 speaks
  SSL 3.0 and TLS 1.0 with RSA key exchange. 29,763 of the dataset's 51,443
  stations are HTTPS, and essentially all of those hosts now require TLS 1.2
  with ECDHE. A TLS stack has to be bundled.

So this is C against Carbon, CoreAudio, libmad, faad2 and mbedTLS. It does follow
the Rust port module for module, because the parts that matter - HLS, the MPEG-TS
demuxer, the format detection, the session lifecycle - are protocol handling
rather than platform code, and every one of the odd-looking behaviours in there
exists because a real station broke without it.

The build targets `-mcpu=7450`, so it will not run on a G3, and there is no Intel
slice.

| Rust module | here | notes |
|---|---|---|
| `ui.rs` | `ui.c`, `listview.c`, `levelmeter.c` | Carbon instead of egui |
| `level_meter.rs` | `levelmeter.c` | same ten segments, same colours |
| `fonts.rs` | **gone** | ATSUI does script fallback; see below |
| `station.rs`, `dataset.rs`, `station_cache.rs` | `dataset.c` + `tools/mkdataset.py` | JSON parse moved to build time |
| `net.rs` | `net.c` | sockets + mbedTLS instead of ureq + rustls |
| `byte_queue.rs` | `bytequeue.c` | pthread condvar instead of `Condvar` |
| `hls.rs` | `hls.c` | same worker loop, same open semantics |
| `m3u8.rs` | `m3u8.c` | direct port, same tests |
| `segment.rs` | `segment.c` | direct port, same tests |
| `ts_demuxer.rs` | `tsdemux.c` | direct port, same tests |
| `adts.rs` | **gone** | faad2 does not need it; see below |
| `format.rs` | `format.c` | container path removed |
| `player.rs` | `player.c` | same generation counter |
| `audio.rs` (`AudioSink`) | `audio.c` | same ring, same level meter |
| `audio.rs` (`Converter`) | **gone** | the audio unit resamples; see below |
| `player.rs` (cpal) | `audio_ca.c` | AudioUnit DefaultOutput |
| `paths.rs` | `paths.c` | same search order |
| `icon.rs` | `icon.c` + `packaging/make-icns.py` | see "The icon" |
| `Arc<AtomicBool>` | `stopflag.c` | refcounted, because GCC 4.0 has no atomics |
| - | `der.c` | new: walks the concatenated-DER CA store |

## Module map

```
src/
  main.c        Carbon app setup, menu bar, event loop
  ui.c          the whole window: two search fields, both lists, status row
  listview.c    the virtualized list view (see "Why not DataBrowser")
  levelmeter.c  the LED bar
  paths.c       where stations.dat and cacert.der are looked for
  dataset.c     mmap reader for stations.dat
  player.c      session lifecycle, decode loop, generation counter
  decoder.c     libmad and faad2 behind one interface
  audio.c       ring buffer between decoder and render callback + peak level
  audio_ca.c    the CoreAudio output unit
  net.c         HTTP/HTTPS: one-shot GETs and one streaming GET
  der.c         ASN.1 length walk for the CA store
  hls.c         HLS worker: playlist polling, segment fetch, feeds the queue
  m3u8.c        HLS playlist parsing and URI resolution
  segment.c     one HLS segment -> elementary stream (TS or packed audio)
  tsdemux.c     MPEG-TS -> raw ADTS AAC / MPEG audio
  format.c      picks which decoder to use for a progressive stream
  bytequeue.c   bounded producer/consumer byte pipe
  stopflag.c    refcounted stop flag
  buf.c         growable byte buffer
  icon.c        the Dock icon, for running outside a bundle
tools/
  mkdataset.py  JSON dataset -> data/stations.dat
  mkcastore.py  PEM CA bundle -> data/cacert.der
  mkicondata.py PNG -> a C array (C has no include_bytes!)
  fetch-vendor.sh
packaging/
  make-icns.py  PNGs -> a 10.4-readable rworldradio.icns
tools/ppc-verify/
  verify.sh     cross-build + run the tests on an emulated PowerPC 7450
  mkcpio.py     hand-written cpio (needs a real /dev/console node)
  mkfixture.py  the WAV the decode comparison encodes from
  compare.py    diffs the host and PowerPC decode reports
tests/
  test_core.c   everything above that is not Carbon, CoreAudio or a decoder
  decode_probe.c  decodes a file through decoder.c; used by ppc-verify
  play_probe.c    plays one station end to end with no window (`make play-probe`)
```

## Invariants - do not "simplify" these away

These came over from the Rust port, where each exists because a real station
broke without it. They are covered by `make check`; if you change the behaviour,
the tests should fail.

1. **`segment.c` checks the packed-audio syncword before the TS heuristic.**
   Compressed audio can contain any byte pattern, including two `0x47` bytes 188
   apart. Checking the leading syncword first is what stops an AAC segment being
   mistaken for a transport stream. There is a test for exactly this.

2. **`segment.c` must not fall through from TS to packed audio.** A TS container
   we could not get audio out of is encrypted, video-only, or uses a stream type
   the PMT parser does not know. Handing the raw TS bytes to a decoder as if they
   were an elementary stream produces noise, not a diagnosis.

3. **`m3u8_resolve_url` checks protocol-relative (`//host/path`) before
   host-absolute (`/path`).** Reversed, `//host/path` gets pasted onto the base's
   own origin and produces a bogus double-slash URL. The Haiku original had this
   bug; qingting.fm's playlists use the form.

4. **`m3u8_resolve_url` strips the query string before taking the directory.**
   Otherwise a `?token=a/b` containing a slash is treated as part of the path.

5. **`format.c` checks the body's first bytes before the Content-Type.** Servers
   get the content type wrong constantly - a station serving ADTS AAC while
   claiming `audio/mpeg` is common. The body cannot lie.

6. **`player.c` decides "is this a playlist?" from the response, not the URL.**
   Stations very commonly use an extensionless URL that 302s to
   `...m3u8?Policy=...` (CDN signing). `looks_like_playlist` exists for that.

7. **`player.c`'s generation counter.** Starting a station while another is
   connecting bumps it; the old session sees it changed and exits without
   emitting anything. Without this a stale session's error overwrites the new
   station's status line seconds after the user moved on.

8. **`tsdemux.c` re-parses PAT/PMT for every segment.** Real encoders repeat them
   periodically precisely so a demuxer can start from any segment. Carrying PID
   state across segments would break on a stream that changes them.

## What was dropped, and why it was safe

**`adts.rs`, the ADTS normalizer.** It exists in the Rust port because
symphonia 0.5's `AdtsReader` syncs only on the exact pattern `0xFFF1` - MPEG-4
ADTS with no CRC - and on a stream that never ends, "never syncs" means blocking
forever rather than failing. faad2 syncs on all three variants (`0xFFF1`,
`0xFFF9`, and the CRC-protected forms) and skips the CRC itself, so rewriting
the headers would be work with no effect. `decoder.c` still walks to a syncword
before handing faad2 a buffer, but only to keep the "how much junk have we
skipped" accounting in one place.

**`audio.rs`'s `Converter`.** The Rust ports open the device's default config and
resample into it with linear interpolation. Here `audio_ca.c` opens the
DefaultOutput audio unit at the *stream's* own rate, and Apple's converter inside
that unit does the rate change and channel mapping. On a G4 that wins twice: the
converter is AltiVec-optimized where our C loop would not be, and it is a real
polyphase resampler rather than linear interpolation. The cost is that the unit
cannot be opened until the first frame has been decoded, which is when the Rust
port opens its cpal stream too.

**`fonts.rs`.** The dataset has station names in Latin, Cyrillic, Greek, Arabic,
Thai and all of CJK. On Linux the Rust port has to find a font per script with
fontconfig and stack them into egui. Here `HIThemeDrawTextBox` hands the string
to ATSUI, which does script-aware fallback against Tiger's own fonts (Lucida
Grande, Hiragino Kaku Gothic Pro, AppleGothic, STHeiti, Geeza Pro, Ayuthaya).
Zero lines of code.

**The container path in `format.rs`.** symphonia gives the Rust ports Ogg, FLAC,
MP4 and WAV. That is 505 of 51,443 stations. `format.c` still *recognizes* them,
so the status row can say "Ogg (unsupported by this build)" rather than failing
with something misleading.

**Media keys.** `mediakeys.rs` publishes Now Playing state to
`MPRemoteCommandCenter`, which is 10.12+. Tiger has nothing equivalent and an
iMac G4 keyboard has no play/pause key.

### The whole list, as it stands

| | Why |
|---|---|
| Ogg, Opus, FLAC, MP4/M4A, WAV | 505 of 51,443 stations, under 1%. The Rust ports get these from symphonia; adding two more decoders for a rounding error is not worth the binary size on this target. The status row names the format instead of failing silently. |
| TLS 1.3 | mbedTLS 2.28 is the newest branch that builds with GCC 4.0.1, and it is TLS 1.2 only. A small minority of hosts require 1.3 and will report a handshake failure. |
| G3 and Intel Macs | The build is `-mcpu=7450` PowerPC only. |
| Media keys | Tiger has no `MPRemoteCommandCenter`, and an iMac G4 keyboard has no play/pause key to route. |
| Now-playing metadata | Same as the other ports: the app never sends `Icy-MetaData`, so no title blocks are interleaved into the audio the decoder has to strip back out. |

## What only building on the target caught

Everything below compiled clean under clang for PowerPC and passed the QEMU
verification, and still did not work on Tiger. They are listed because each one
is a class of mistake, not a one-off.

| | |
|---|---|
| `HISearchFieldCopyText` / `HISearchFieldSetText` | Do not exist on 10.4. Its search field has only Create, {Get,Set}SearchMenu, {Copy,Set}DescriptiveText and {Get,Change}Attributes; the text goes through the generic `HIViewCopyText` / `HIViewSetText`. |
| `kHIScrollViewOptionsVerticalScroll` | Spelled `kHIScrollViewOptionsVertScroll` on 10.4 (and `...HorizScroll`). |
| `CFSTR("Ag\xe3\x83\xa9...")` | The 10.4 compiler reads non-ASCII bytes in a `CFSTR` literal as **MacRoman**, not UTF-8, and warns. `listview.c` measured its row height from a CJK sample, so it was measuring the wrong glyphs. Build the string with `CFStringCreateWithCString(..., kCFStringEncodingUTF8)`. |
| `make` built the icons and stopped | The default goal is the first target in the file, and the icon rules sit above `all`. GNU make 3.81 has `.DEFAULT_GOAL`; Tiger has 3.80, so `all:` is declared early instead and given its prerequisites later. |
| `CC ?= gcc-4.0` | Never took effect - make predefines `CC`, so its origin is `default`, not `undefined`, and `?=` kept `cc`. On Tiger `cc` happens to be the same compiler, which is exactly why it would have gone unnoticed. Now guarded with `ifeq ($(origin CC),default)`. |

The API-name ones share a cause worth remembering: a 10.4 SDK is the only
authority on what 10.4 has. Before using a Carbon call, grep
`/Developer/SDKs/MacOSX10.4u.sdk/System/Library/Frameworks/` for it - that is
seconds of work and catches what documentation written for later releases does
not.

## Nothing in the build path may need Python 3

Tiger has Python 2.3 and nothing newer, and no way to get anything newer without
a compiler - which is what is being built. So every generator here
(`mkdataset.py`, `mkcastore.py`, `mkicondata.py`, `make-icns.py`) runs on the
modern host, and its output is shipped across as a build input alongside the
source.

`make icons` produces `packaging/rworldradio.icns` and `packaging/icon_png.h`.
Both are gitignored, both are reproducible from what is checked in, and both are
prerequisites the Makefile explains rather than tries to build when python3 is
missing. `icon_png.h` used to be generated into `build/` during the compile,
which worked everywhere except on the only machine that matters.

The same applies to the `.tar.gz`/`.tar.bz2` files in `vendor/`: they are
downloaded on the host, and `tools/fetch-vendor.sh` on Tiger sees them already
present and only unpacks.

## The dataset file

```
data/stations.dat   229 countries, 51,443 stations, 5.0 MB
data/cacert.der     151 CA certificates, 157 kB
```

The JSON it is built from is the repository's `../shared/data` (`countries.json`
and 288 per-country files; only the 229 the index references are ever read).
`make dataset` reads it from there (`DATASET_SRC`) and writes `data/stations.dat`;
`data/` in this directory holds only the two generated files and is gitignored.

### Why it is a binary file here

The Rust ports parse `countries.json` plus 229 per-country files - 16.8 MB of
JSON - at every launch. On a G4 that takes somewhere between ten and twenty-five
seconds, which is not a startup, it is a hang.

`tools/mkdataset.py` does that parse once, at build time, and writes a flat
big-endian file that the app `mmap`s. Loading it is an `open` and an `mmap`:
no parsing, no allocation for the records, and only the pages actually touched
become resident. It also made the JSON parser unnecessary, since nothing else in
the app reads JSON.

### Where it is looked for

`paths.c`, in this order - the same order as `paths.rs`:

1. the enclosing `.app` bundle's `Contents/Resources`
2. `data/` beside the binary, and up to three directories above it
3. `~/Library/Application Support/RWorldRadio/data`
4. `/Library/Application Support/RWorldRadio/data`
5. `../data` and `data`

`make bundle` puts both files inside the `.app`, so an installed copy is
self-contained: drag it to the Trash and nothing is left behind. Cases 2 and 5
are what make `build/rworldradio` runnable straight out of the tree.

## The icon

The other macOS port's `rworldradio.icns` is built by `iconutil`, and `iconutil`
writes only the modern types: `ic07`/`ic08`/`ic09` (embedded PNG, 10.5+),
`ic04`/`ic05` (ARGB, 10.8+) and `ic10`-`ic14` (Retina, 10.7+). Tiger's Icon
Services reads none of them, so that file in a bundle produces the generic
application icon - there is nothing in it Tiger recognizes.

`packaging/make-icns.py` writes the classic types instead, from the same source
PNGs: `ics#`, `is32`/`s8mk`, `ICN#`, `il32`/`l8mk`, `ih32`/`h8mk` and
`it32`/`t8mk`. Three things about that format are easy to get wrong:

1. The 24-bit types are **RLE-compressed, one colour plane at a time** - all of
   R, then all of G, then all of B, each encoded independently and concatenated.
   Alpha is not in there; it is the separate `s8mk`/`l8mk`/`h8mk`/`t8mk` chunk,
   which is raw uncompressed 8-bit.
2. **`it32` carries four zero bytes before its RLE data** and no other type does.
   A decoder that is not expecting them renders noise.
3. Run lengths are bounded: a control byte of `0xFF` means 130 repeats and `0x7F`
   means 128 literal bytes. Emitting longer runs produces a file that most
   decoders accept and Tiger does not.

The script decodes its own output before writing, so a broken encoder fails the
build rather than shipping a corrupt icon. `packaging/rworldradio.icns` is
generated and gitignored - it is fully reproducible from the PNGs beside it.

The modern PNG types are appended as well. A reader takes the best type it knows
and ignores the rest, so the same file also looks right on a current Mac.

Separately, `src/icon.c` compiles the 128px PNG in and calls
`SetApplicationDockTileImage` at startup. That only matters for running
`build/rworldradio` directly: outside a bundle there is no Info.plist for
LaunchServices to read `CFBundleIconFile` from. `src/icon.rs` does the same in
the Rust ports, for the same reason. `CGImageSource` arrived in 10.4, which is
exactly this port's floor, so there is no older decoder to fall back to - a
failure there just leaves the generic tile, which is cosmetic.

## Why not DataBrowser

DataBrowser is the obvious Carbon control for a list and the wrong one here. It
owns its item list and has no filter API, so filtering as the user types means
`RemoveDataBrowserItems` + `AddDataBrowserItems` on every keystroke. The largest
country in the dataset is the United States with 6,557 stations; on a G4 that
makes typing visibly stutter.

`listview.c` holds no items at all. The delegate answers "how many rows" and
"what is row N", filtering is a rebuild of an `int` array in `ui.c`, and drawing
touches only the ~30 rows on screen. That is the same virtualization the Rust
port gets from egui's `ScrollArea::show_rows`, and it is *less* code than the
DataBrowser callbacks would be.

Two details worth keeping:

- Row text is a **borrowed** `CFStringRef`. `ui.c` creates one per row when a
  country is selected and holds it; the view never creates one while drawing.
  Creating them per draw would allocate ~30 times per scroll frame.
- The view implements `kEventClassScrollable`, which is what gets real Aqua
  scroll bars, live scrolling and the mouse wheel from `HIScrollView` without
  writing any of it.

## Where the time actually goes

Measured on the target: an iMac G4 (PowerMac4,2), PowerPC 7450 at 700 MHz, 1 GB
of RAM, 256 kB L2, Mac OS X 10.4.11, Xcode 2.5.

What was verified there, end to end:

| | |
|---|---|
| Build | clean, GCC 4.0.1, 22 app objects + 145 vendor objects, 791 kB binary |
| HTTPS progressive AAC | plays (Korea) |
| HTTP progressive MP3 | plays (UK) |
| HTTPS progressive MP3 | plays (Germany) |
| HTTP HLS AAC | plays (Al Jazeera) |
| HTTPS HLS AAC | plays (Poland) |
| CPU while playing MP3 192 kbps | 7.1%, 4.2 MB RSS |
| CPU while playing HTTPS HLS AAC | 13%, 4.6 MB RSS |
| TLS handshakes for 60 s of HTTPS HLS | 2, down from 22 before connection reuse |
| App idle | sleeping in the event loop, 13.6 MB RSS |

`make play-probe` builds the headless probe those rows come from: it drives the
whole path except Carbon, which is what makes it usable over SSH.

And where the time goes:

| | |
|---|---|
| Dataset load | `mmap`, imperceptible. This was 10-25 s before `mkdataset.py` |
| App idle, stopped | sleeping in `RunApplicationEventLoop`, 13.6 MB RSS |
| Playing MP3 at 192 kbps | **7.1% of the CPU, 4.2 MB RSS** (headless probe) |
| Connecting, HTTPS progressive | 1.5-7.5 s to first audio, dominated by the server |
| Connecting, HLS | 2-15 s to first audio - a playlist fetch plus a segment fetch |
| Binary | 791 kB, statically linked against mbedTLS, libmad and faad2 |

7.1% at 192 kbps on a 700 MHz part is the same work as roughly 4% at 128 kbps on
the 867 MHz machine the earlier estimates assumed, so the decoders are where they
were expected to be: not the bottleneck. The optimization effort went into the
dataset format, the list widget and the TLS cipher order instead. Three
consequences worth not undoing:

- **`-ffast-math` is not used.** faad2's SBR stage depends on IEEE semantics and
  misdecodes HE-AAC without them. There is nothing to win here anyway.
- **ChaCha20-Poly1305 is offered first** (`PREFERRED_CIPHERSUITES` in `net.c`).
  It is pure 32-bit add-rotate-xor: unlike table-driven AES it neither touches
  the FPU nor evicts anything from the 256 kB L2.
- **The CA store is concatenated DER, not PEM,** and is parsed lazily on the
  first HTTPS connection rather than at startup. Base64-decoding 151
  certificates costs a G4 a few hundred milliseconds.

**AltiVec is not in this binary.** `-faltivec` enables the language extensions,
but GCC 4.0.1 has no auto-vectorizer and `otool -tV build/rworldradio` finds zero
vector instructions. The AltiVec win this port gets is Apple's, inside the
CoreAudio converter that `audio_ca.c` hands the stream's native rate to. (The
clang cross-build in `tools/ppc-verify/` *does* emit AltiVec, which is why it is
run against a 7450 rather than the default G3.)

### Connection reuse, and what it was worth

A TLS handshake on this machine costs **1.2-2.7 seconds**. Not the 100-250 ms
estimated before there was hardware to measure on - an order of magnitude out.
That number is what makes HTTPS HLS the hard case: every playlist poll and every
segment fetch is a request, so a 6-second segment list means a request every
three seconds, forever.

`net.c` therefore does two things it did not originally do:

- **A connection pool** (`pool_take` / `pool_put`), keyed on host/port/TLS.
  Requests to a host already connected reuse the socket, so they cost no TCP
  setup and no handshake at all. A pooled connection can have been dropped by
  the server while idle, so `http_request` retries once on a fresh one; the
  `stale` counter in `NetStats` says how often that happens.
- **A client-side TLS session cache**, so the handshakes that *do* happen -
  a new host, or a connection the server closed - resume instead of running a
  full key exchange and certificate verification.

Measured on the target, same HTTPS HLS station, 60 seconds of playback each:

| | connections | handshakes | time in handshakes | first audio |
|---|---|---|---|---|
| Neither | 23 | 22 | **41.3 s** | 14.6 s |
| Session cache only | 30 | 30 | 14.5 s | 9.0 s |
| Both (current) | **2** | **2** | **3.3 s** | 7-8 s |

41.3 seconds of handshaking inside 60 seconds of playback is 69% of the one
core, on top of decoding. That is why the first version stuttered at startup.
It is now 5.6%, total CPU for the station is about 13%, and time to first audio
roughly halved.

`net_close_idle_connections()` is called from `player.c` when playback stops:
the next station is nearly always a different host and the sockets would be
dropped anyway.

Two things not to undo:

- **The body must be read to its end before a connection is pooled.** Only a
  `Content-Length` or a chunked body says where the response stops; a
  read-until-close body (what an icecast stream is) leaves no message boundary,
  so `net_open_stream`'s connection is never pooled. `response_done()` is the
  single place that decides.
- **Redirect and error bodies are drained** (`drain_body`) rather than
  abandoned, because the next hop is very often the same host.

## Security posture

The app parses attacker-controllable input on a machine that has not had a
security update since 2007, so a few things are deliberate rather than
incidental:

- **TLS verification is not optional.** `MBEDTLS_SSL_VERIFY_REQUIRED` plus a CA
  chain plus `mbedtls_ssl_set_hostname` for SNI *and* name checking. There is no
  "insecure" flag and no fallback path that skips it: if `ensure_ca_chain` fails
  the connection fails, and the station reports why. Do not add one - a
  --no-verify option in an app whose whole job is fetching URLs from a
  third-party dataset is a bad trade at any convenience.
- **Every bound is fixed.** Response bodies stop at 8 MB (text) or 32 MB
  (audio); redirects at 5 hops; playlist indirection at 2; a drained body at
  256 kB; the resync window in `decoder.c` at 512 kB. A hostile or broken server
  can waste time but cannot make this allocate without limit.
- **The dataset is treated as untrusted** even though it ships with the app.
  `dataset.c` validates the header, bounds-checks every offset against the
  mapping and returns `""` rather than reading past the end. `tests/test_core.c`
  covers the malformed cases.
- **No shell, ever.** Nothing here builds a command line from a URL or a station
  name.

What is *not* defended against: a malicious audio stream feeding libmad or
faad2. Those are twenty-year-old C decoders parsing hostile input in-process,
and that is the real attack surface. It is the same exposure any media player on
this platform has, and the mitigation is that the dataset only carries public
broadcast stations - but it is worth being honest that it exists.

## Threading

Four kinds of thread, all pthreads:

- **The main thread** runs `RunApplicationEventLoop`. It never blocks on I/O.
- **One session thread** per station (`player.c`), which owns the decode loop.
  512 kB stack.
- **Network workers** - one per progressive stream (`net.c`) and one per HLS
  stream (`hls.c`). Detached, 256 kB stacks.
- **CoreAudio's render thread**, which calls `audiosink_fill`.

Everything crossing a thread boundary is refcounted (`ByteQueue`, `AudioSink`,
`StopFlag`, `Player`, `WorkerState`) because the network workers are detached and
may still be unwinding after the session that started them is gone. GCC 4.0.1 has
no `__sync` builtins - those arrived in 4.1 - so refcounts are guarded by a mutex
rather than done atomically.

Status reaches the UI two ways: the player queues an event and calls a `wake`
callback, which posts a Carbon event to the main queue (`PostEventToQueue` is
thread-safe); and a 100 ms `EventLoopTimer` polls the level meter while playing.
The timer is installed only while playing or connecting.

`audiosink_fill` takes a mutex in the render callback. That is a real-time
no-no in principle. It is what the Rust port does too, the critical section is
two `memcpy`s with no allocation and no I/O, and the only other holder is the
decode thread doing the same. Do not add anything else under that lock.

## Verifying on a real big-endian PowerPC

`tools/ppc-verify/verify.sh` cross-compiles the portable half of this port for
32-bit big-endian PowerPC and runs it under `qemu-system-ppc` on an emulated
Power Mac with a 7450 - the iMac G4's own CPU family. See that directory's
README for how it is put together and what it does and does not prove.

It is worth running after touching anything in `dataset.c`, `tsdemux.c`,
`decoder.c` or the vendor flags in the Makefile, because those are where a
little-endian assumption hides best. It has already caught two bugs that would
have shipped:

1. faad2 was compiled without a `config.h`, so `libfaad/common.h` fell back to
   its own `typedef signed long int32_t` and collided with the system header -
   the library did not build at all.
2. `-DWORDS_BIGENDIAN=1` was in `CPPFLAGS_COMMON`, which only this port's own
   sources use, so it never reached faad2. `libfaad/bits.h` derives
   `ARCH_IS_BIG_ENDIAN` from it and byte-swaps the bitstream reader without it:
   AAC would have decoded to noise on the target, silently, while building
   cleanly.

Results as of the last run: all 162 core checks pass on PowerPC; MP3 decodes
bit-identically through libmad's `FPM_PPC` path (267 `mulhw`/`mulhwu`
instructions in the decoder objects); AAC matches the host on RMS, peak, the
coarse checksum and sampled output.

## Endianness

PowerPC is big-endian, which for this code is mostly a simplification - MPEG-TS
and ADTS headers are big-endian bitfields, so they read naturally.

The one place it matters is `stations.dat`. It is written big-endian by
`mkdataset.py` and read by `dataset.c` with explicit `be32`/`be16`/`be_float`
decoders rather than a cast-in-place. On the target that compiles to plain loads,
and it is what lets `tests/test_core.c` read the same file on a little-endian
host. Do not "optimize" those into a struct overlay.

faad2 gets its `WORDS_BIGENDIAN` from the `config.h` that
`tools/fetch-vendor.sh` writes, not from a `-D` on the command line. That is
deliberate: it sat in `CPPFLAGS_COMMON` at first, which faad2's own rule does
not use, and nothing failed - it just would have decoded noise on the target.
Keep it in the config.h where the rest of faad2's feature probes live.

## Adding a codec

If Ogg/Vorbis ever becomes worth the 502 stations:

1. Add `RWR_CODEC_VORBIS` to `tsdemux.h`.
2. Return it from `format.c` where `OGG` is currently returned.
3. Add a third backend to `decoder.c` - the interface is `decoder_next` filling a
   `DecodedFrame`, and nothing above it needs to change.
4. Vendor libvorbis (or Tremor) in the Makefile the way libmad is.

Nothing in `player.c`, `audio.c` or `audio_ca.c` needs to know.

## Re-syncing with the Rust ports

There is no automated path - this is a different language. When a protocol fix
lands upstream, the files worth diffing by hand are `m3u8.rs` vs `m3u8.c`,
`segment.rs` vs `segment.c`, `ts_demuxer.rs` vs `tsdemux.c`, `format.rs` vs
`format.c` and the playlist-following logic in `player.rs` vs `player.c`. Those
five are near-literal translations and their tests were translated with them, so
a new upstream test case can usually be pasted into `tests/test_core.c` with only
syntax changes.

## Checking the build without a G4

```sh
make check               # 162 checks, host compiler
make check-sanitize      # the same, under ASan, UBSan and TSan
```

builds the protocol and data half of the port with the *host* compiler and runs
its tests against the real `data/stations.dat` and `data/cacert.der` this build
ships (`make dataset` and `make castore` first). Carbon, CoreAudio and the two
decoders are not covered. On a Mac whose command line tools SDK is newer than
Xcode's, point `SDKROOT` at Xcode's SDK if the link step fails.

To run that same half on real big-endian PowerPC, under an emulated Power Mac
with a 7450:

```sh
brew install qemu llvm lld
tools/ppc-verify/verify.sh
```

See [`tools/ppc-verify/README.md`](tools/ppc-verify/README.md).

## Known limitations

- **Ogg, Opus, FLAC, MP4/M4A and WAV do not play.** That is 505 of the 51,443
  stations, under 1%; the status row names the format rather than failing
  silently.
- **No TLS 1.3.** A small minority of HTTPS hosts require it and will report a
  handshake failure.
- **No media keys and no now-playing metadata.**
- **PowerPC G4 only** - not G3, not Intel.
