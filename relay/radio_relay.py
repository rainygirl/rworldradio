#!/usr/bin/env python3
"""R WorldRadio relay for Mac OS 9 (runs on the Pi).

Mac OS 9 has no modern TLS, no AAC/HLS stream support and QuickTime 6 only
plays endless MP3 when the server answers like Shoutcast ("ICY 200 OK").
This relay does the hard parts:

  GET /countries                 idx|enc|name|count          (one per line)
  GET /stations?c=IDX            id|enc|name|codec|bitrate
  GET /search?q=TEXT             id|enc|name|codec|bitrate|country
  GET /play?id=ID                ICY MP3 stream (ffmpeg -> 128k MP3)
  GET /kexp                      ICY passthrough of the KEXP relay on :8082
  GET /health                    JSON

Text lines are encoded for the Mac: enc K = Mac Korean (EUC-KR) bytes,
enc R = Mac Roman bytes ('?' for anything else). Only one /play stream runs at
a time; a new request stops the previous ffmpeg.
"""
import argparse
import glob
import json
import os
import signal
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

KEXP_SOURCE = "http://127.0.0.1:8082/kexp.mp3"
COUNTRIES = []          # (name, [station dict])
STATIONS = []           # flat list, index = id
_play_lock = threading.Lock()
_current = {"proc": None, "id": None, "started": 0}


def mac_text(s):
    s = " ".join(str(s).split()).replace("|", "/")
    if any("가" <= ch <= "힣" for ch in s):
        try:
            return "K", s.encode("cp949")
        except UnicodeEncodeError:
            pass
    return "R", s.encode("mac_roman", "replace")


def load(data_dir):
    index = json.load(open(os.path.join(data_dir, "countries.json"), encoding="utf-8"))
    for c in index:
        path = os.path.join(data_dir, "countries", c["file"])
        try:
            items = json.load(open(path, encoding="utf-8"))
        except (OSError, ValueError):
            items = []
        rows = []
        for s in items:
            if not s.get("url"):
                continue
            s = dict(s, id=len(STATIONS), country=c["name"])
            STATIONS.append(s)
            rows.append(s)
        COUNTRIES.append((c["name"], rows))


def station_line(s, with_country=False):
    enc, name = mac_text(s.get("name") or "?")
    fields = [str(s["id"]).encode(), enc.encode(), name, str(s.get("codec") or "").encode(),
              str(s.get("bitrate") or 0).encode()]
    if with_country:
        fields.append(mac_text(s["country"])[1])
    return b"|".join(fields)


def stop_current():
    p = _current["proc"]
    if p and p.poll() is None:
        try:
            os.killpg(p.pid, signal.SIGTERM)
        except OSError:
            pass
    _current["proc"] = None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))

    def text(self, lines):
        body = b"\r".join(lines) + b"\r"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=x-mac")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def icy(self, name):
        # QuickTime 6 only treats an endless MP3 as live radio with a Shoutcast reply.
        head = b"ICY 200 OK\r\nicy-name:" + mac_text(name)[1] + b"\r\nicy-br:128\r\nicy-pub:0\r\ncontent-type:audio/mpeg\r\n\r\n"
        self.wfile.write(head)
        self.wfile.flush()

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(url.query)
        path = url.path
        if path == "/health":
            body = json.dumps({"stations": len(STATIONS), "countries": len(COUNTRIES),
                               "playing": _current["id"], "since": _current["started"]}).encode()
            self.send_response(200); self.send_header("Content-Type", "application/json")
            self.end_headers(); self.wfile.write(body); return
        if path == "/countries":
            self.text([b"%d|%s|%s|%d" % (i, mac_text(n)[0].encode(), mac_text(n)[1], len(rows))
                       for i, (n, rows) in enumerate(COUNTRIES)])
            return
        if path == "/stations":
            try:
                rows = COUNTRIES[int(q.get("c", ["-1"])[0])][1]
            except (ValueError, IndexError):
                self.send_error(404); return
            self.text([station_line(s) for s in rows])
            return
        if path == "/search":
            term = (q.get("q", [""])[0]).strip().lower()
            if not term:
                self.text([]); return
            hits = [s for s in STATIONS if term in s["name"].lower()][:200]
            self.text([station_line(s, True) for s in hits])
            return
        if path == "/kexp":
            self.icy("KEXP 90.3 FM")
            try:
                with urllib.request.urlopen(KEXP_SOURCE, timeout=15) as r:
                    while True:
                        b = r.read(8192)
                        if not b:
                            break
                        self.wfile.write(b)
            except Exception as exc:
                sys.stderr.write("kexp client ended: %s\n" % exc)
            return
        if path == "/play":
            try:
                s = STATIONS[int(q.get("id", ["-1"])[0])]
            except (ValueError, IndexError):
                self.send_error(404); return
            with _play_lock:
                stop_current()
                proc = subprocess.Popen(
                    ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
                     "-reconnect", "1", "-reconnect_streamed", "1", "-reconnect_delay_max", "5",
                     "-user_agent", "Mozilla/5.0", "-i", s["url"],
                     "-vn", "-ac", "2", "-ar", "44100", "-c:a", "libmp3lame", "-b:a", "128k", "-f", "mp3", "-"],
                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, preexec_fn=os.setsid)
                _current.update(proc=proc, id=s["id"], started=int(time.time()))
            # Wait for real audio before answering, so a dead station fails fast
            # instead of looking like a silent live stream to QuickTime.
            first = b""
            deadline = time.time() + 20
            import select
            while time.time() < deadline and proc.poll() is None:
                r, _, _ = select.select([proc.stdout], [], [], 1)
                if r:
                    first = os.read(proc.stdout.fileno(), 4096)
                    break
            if not first:
                with _play_lock:
                    if _current["proc"] is proc:
                        stop_current(); _current["id"] = None
                self.send_error(502, "station unavailable")
                return
            self.icy(s["name"])
            self.wfile.write(first)
            try:
                while True:
                    b = proc.stdout.read(4096)
                    if not b:
                        break
                    self.wfile.write(b)
            except Exception as exc:
                sys.stderr.write("play client ended: %s\n" % exc)
            finally:
                with _play_lock:
                    if _current["proc"] is proc:
                        stop_current()
                        _current["id"] = None
                if proc.poll() is None:
                    try:
                        os.killpg(proc.pid, signal.SIGTERM)
                    except OSError:
                        pass
            return
        self.send_error(404)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "data"))
    ap.add_argument("--port", type=int, default=8083)
    args = ap.parse_args()
    load(args.data)
    sys.stderr.write("loaded %d stations in %d countries\n" % (len(STATIONS), len(COUNTRIES)))
    ThreadingHTTPServer(("0.0.0.0", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
