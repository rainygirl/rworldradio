# R World Radio - Mac OS 9 port

Internet radio for Mac OS 9 (PowerPC). The [Tiger PPC port](https://github.com/rainygirl/rworldradio-macppc)
depends on pthreads, BSD sockets, CoreAudio, HIView and a bundled TLS stack, none of which exist on Mac OS 9.
So this port splits the work:

- **`mac/`** - the Mac OS 9 application (C, classic Toolbox, Retro68). Search box, country list, station list,
  KEXP / Stop / Play buttons. Playback is QuickTime 6; lists are fetched with URL Access.
  It starts the last station at launch (KEXP by default) and reconnects after an error or a 30-second stall;
  after three failures in a row it stops. Korean names are drawn in AppleGothic.
- **`relay/radio_relay.py`** - a small relay on a Linux box on the same LAN (a Raspberry Pi here).
  It serves the R World Radio dataset (`data/`, 229 countries, 51,443 stations with a URL) and turns any station -
  HTTPS, AAC, HLS - into a 128 kbps MP3 stream with ffmpeg.

## Why a relay

- Mac OS 9 has no TLS that modern hosts accept, and no AAC or HLS decoder.
- QuickTime 6 treats an endless HTTP MP3 as a file and tries to download all of it. It plays it live only from a
  Shoutcast-style server: an `icy://` URL answered with `ICY 200 OK`. The relay answers exactly that way.
- The relay waits for the first audio bytes before answering, so a dead station (for example an expired token URL)
  returns 502 within 20 seconds instead of looking like a silent live stream.

## Relay endpoints

| Path | Answer |
|---|---|
| `/countries` | `idx\|enc\|name\|count` per line |
| `/stations?c=IDX` | `id\|enc\|name\|codec\|bitrate` |
| `/search?q=TEXT` | `id\|enc\|name\|codec\|bitrate\|country` (up to 200) |
| `/play?id=ID` | ICY MP3 stream (one transcode at a time) |
| `/kexp` | ICY pass-through of an existing KEXP MP3 relay on `:8082` |
| `/now` | `V 1` / `PLAYING 0\|1` / `STATION name` / `TITLE artist - song` (UTF-8 lines) for the Home View screen saver: the stream the Mac has open, with the KEXP API or the station's ICY StreamTitle. `?src=kexp` answers the KEXP track even with no stream open (Tiger, which plays KEXP in iTunes) |
| `/health` | JSON |

Lines end with CR. `enc` is `K` (Mac Korean / EUC-KR bytes) or `R` (Mac Roman).

```sh
python3 relay/radio_relay.py --data data --port 8083
sudo cp relay/radio-relay.service /etc/systemd/system/ && sudo systemctl enable --now radio-relay
```

The service file assumes `/home/rainygirl/Workspace/robot/tiger/radio`; adjust the paths. Needs Python 3 and ffmpeg
with libmp3lame.

## Build and install the Mac app

```sh
make            # Retro68 in docker -> build/mac/RWorldRadio.bin (MacBinary)
```

Copy `RWorldRadio.bin` to the Mac and expand it with StuffIt Expander (or any MacBinary tool that keeps the
resource fork). Next to the app, `R WorldRadio Settings` (text, CR lines) holds:

```
relay=10.0.0.129:8083
last=kexp            (or a station id; empty = play nothing at launch)
lastname=KEXP 90.3 FM / Seattle
```

Requirements: Mac OS 9 with QuickTime 6, URL Access and Open Transport; 24 MB partition (12 MB minimum).

## Tested

On an iMac G4 (700 MHz) running Mac OS 9.2.2, 2026-11-03: KEXP plays at launch, country and station lists,
Korean search results, an AAC station (4MBS Classic FM) transcoded to MP3, the 502 path for an expired URL,
switching back to KEXP, and playing together with a full-screen screen saver.

## License

MIT, see `LICENSE`. The station dataset in `data/` comes from the other R World Radio ports.
