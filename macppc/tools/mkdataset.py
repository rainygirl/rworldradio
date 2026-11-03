#!/usr/bin/env python3
"""Builds data/stations.dat from the JSON dataset the other ports read directly.

The Rust ports parse data/countries.json plus 229 files under data/countries/ at
startup - 16.8 MB of JSON. On a G4 that is by far the most expensive thing the
app does, so this port moves the parse to build time and ships a flat,
mmap-able binary instead. Loading it at runtime is an open() and an mmap(): no
parsing, no heap allocation for the records, and only the pages actually touched
become resident.

Everything is big-endian, which is PPC's native order, so the reader on Tiger
does no byte swapping at all. (src/dataset.c still decodes explicitly, so the
same file also reads correctly on a little-endian host - that is what lets
tests/test_core.c exercise the reader here.)

Layout, all offsets from the start of the file:

    header      64 bytes, see HEADER_FORMAT below
    countries[] 12 bytes each: name_off, first_station, station_count
    stations[]  24 bytes each: see pack_station()
    smalltab[]  4 bytes each: string offsets for the interned codec/language
                values (there are ~110 distinct ones across 68k stations, so
                interning them saves ~1 MB over storing them inline)
    strings     NUL-terminated UTF-8, deduplicated

Usage:
    tools/mkdataset.py ../shared/data data/stations.dat
"""

import json
import os
import struct
import sys

MAGIC = 0x52575244  # 'RWRD'
VERSION = 1
HEADER_SIZE = 64
COUNTRY_SIZE = 12
STATION_SIZE = 24

# station flags
FLAG_HAS_LOCATION = 1 << 0
FLAG_NEEDS_RESOLVE = 1 << 1

NO_SMALL_STRING = 0xFFFF


class StringPool:
    """Deduplicating pool of NUL-terminated UTF-8 strings.

    Offset 0 is always the empty string, so a name_off of 0 reads back as ""
    without the reader needing a sentinel.
    """

    def __init__(self):
        self.buf = bytearray(b"\0")
        self.offsets = {"": 0}

    def add(self, text):
        if text in self.offsets:
            return self.offsets[text]
        offset = len(self.buf)
        self.buf += text.encode("utf-8") + b"\0"
        self.offsets[text] = offset
        return offset


def as_text(value):
    """serde_json's as_string equivalent: only a JSON string counts."""
    return value if isinstance(value, str) else ""


def as_int(value):
    if isinstance(value, bool):
        return 0
    if isinstance(value, (int, float)):
        return int(value)
    return 0


def as_float(value):
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return float(value)
    return None


def load_countries(data_dir):
    """Reads countries.json and every per-country file it points at.

    Mirrors dataset.rs: an entry with no name or no file is skipped, and so is a
    station with no name or no url.
    """
    index_path = os.path.join(data_dir, "countries.json")
    with open(index_path, encoding="utf-8") as handle:
        index = json.load(handle)
    if not isinstance(index, list):
        raise SystemExit("%s: expected a JSON array" % index_path)

    countries = []
    for entry in index:
        if not isinstance(entry, dict):
            continue
        name = as_text(entry.get("name"))
        filename = as_text(entry.get("file"))
        if not name or not filename:
            continue

        path = os.path.join(data_dir, "countries", filename)
        try:
            with open(path, encoding="utf-8") as handle:
                records = json.load(handle)
        except FileNotFoundError:
            sys.stderr.write("mkdataset: missing %s, skipping %s\n" % (path, name))
            continue
        if not isinstance(records, list):
            continue

        stations = []
        for record in records:
            if not isinstance(record, dict):
                continue
            station_name = as_text(record.get("name"))
            # playback_url(): the pre-resolved URL wins when the dataset has one.
            url = as_text(record.get("url_resolved")) or as_text(record.get("url"))
            if not station_name or not as_text(record.get("url")):
                continue

            lat = as_float(record.get("lat"))
            lon = as_float(record.get("lon"))
            stations.append(
                {
                    "name": station_name,
                    "url": url,
                    "codec": as_text(record.get("codec")),
                    "language": as_text(record.get("language")),
                    "bitrate": as_int(record.get("bitrate")),
                    "needs_resolve": record.get("needsResolve") is True,
                    "lat": lat,
                    "lon": lon,
                    "has_location": lat is not None and lon is not None,
                }
            )

        if stations:
            countries.append({"name": name, "stations": stations})

    # The Rust ports keep the dataset in a BTreeMap, so the country list is
    # sorted by name. Keep that here rather than trusting the index file's order.
    countries.sort(key=lambda country: country["name"])
    return countries


def build(data_dir):
    countries = load_countries(data_dir)

    strings = StringPool()
    smalls = {}          # text -> index into smalltab
    small_order = []

    def intern_small(text):
        if not text:
            return NO_SMALL_STRING
        if text not in smalls:
            smalls[text] = len(small_order)
            small_order.append(text)
        return smalls[text]

    country_rows = []
    station_rows = []

    for country in countries:
        first = len(station_rows)
        for station in country["stations"]:
            station_rows.append(
                {
                    "name_off": strings.add(station["name"]),
                    "url_off": strings.add(station["url"]),
                    "lang_id": intern_small(station["language"]),
                    "codec_id": intern_small(station["codec"]),
                    "bitrate": max(0, min(65535, station["bitrate"])),
                    "flags": (FLAG_HAS_LOCATION if station["has_location"] else 0)
                    | (FLAG_NEEDS_RESOLVE if station["needs_resolve"] else 0),
                    "lat": station["lat"] or 0.0,
                    "lon": station["lon"] or 0.0,
                }
            )
        country_rows.append(
            {
                "name_off": strings.add(country["name"]),
                "first": first,
                "count": len(station_rows) - first,
            }
        )

    # Small strings are interned into the pool last so that their offsets are
    # known before the table is written.
    small_offsets = [strings.add(text) for text in small_order]

    countries_off = HEADER_SIZE
    stations_off = countries_off + COUNTRY_SIZE * len(country_rows)
    smalltab_off = stations_off + STATION_SIZE * len(station_rows)
    strings_off = smalltab_off + 4 * len(small_offsets)
    total = strings_off + len(strings.buf)

    out = bytearray()
    out += struct.pack(
        ">11I5I",
        MAGIC,
        VERSION,
        len(country_rows),
        len(station_rows),
        len(small_offsets),
        countries_off,
        stations_off,
        smalltab_off,
        strings_off,
        len(strings.buf),
        total,
        0, 0, 0, 0, 0,
    )
    assert len(out) == HEADER_SIZE

    for row in country_rows:
        out += struct.pack(">III", row["name_off"], row["first"], row["count"])

    for row in station_rows:
        out += struct.pack(
            ">IIHHHHff",
            row["name_off"],
            row["url_off"],
            row["lang_id"],
            row["codec_id"],
            row["bitrate"],
            row["flags"],
            row["lat"],
            row["lon"],
        )

    for offset in small_offsets:
        out += struct.pack(">I", offset)

    out += strings.buf
    assert len(out) == total, (len(out), total)
    return bytes(out), len(country_rows), len(station_rows), len(small_offsets)


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: mkdataset.py <json-data-dir> <out.dat>\n")
        return 2

    blob, countries, stations, smalls = build(argv[1])
    out_path = argv[2]
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "wb") as handle:
        handle.write(blob)

    sys.stderr.write(
        "mkdataset: %s - %d countries, %d stations, %d interned codec/language "
        "values, %.1f MB\n"
        % (out_path, countries, stations, smalls, len(blob) / 1048576.0)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
