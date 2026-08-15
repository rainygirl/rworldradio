/* Unit tests for the platform-independent half of the port.
 *
 * These are the Rust port's own tests, translated: the m3u8, ts_demuxer,
 * segment and format cases exist because a real station broke without the
 * behaviour they pin down, so they are the part of the port most worth checking.
 * Nothing here touches Carbon, CoreAudio or the network, which is what lets this
 * build and run on a modern Mac as well as on the Tiger target - see
 * `make check`.
 *
 * The dataset tests read data/stations.dat if it is present and are skipped if
 * it is not. */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio.h"
#include "buf.h"
#include "bytequeue.h"
#include "dataset.h"
#include "der.h"
#include "format.h"
#include "m3u8.h"
#include "playlist.h"
#include "segment.h"
#include "tsdemux.h"

static int failures;
static int checks;
static const char *current_test;

static void check(int condition, const char *what, const char *file, int line)
{
    checks++;
    if (!condition) {
        failures++;
        fprintf(stderr, "FAIL %s: %s (%s:%d)\n", current_test, what, file, line);
    }
}

#define CHECK(condition) check((condition), #condition, __FILE__, __LINE__)

static void check_str(const char *got, const char *want, const char *file, int line)
{
    checks++;
    if (!got || strcmp(got, want) != 0) {
        failures++;
        fprintf(stderr, "FAIL %s: got \"%s\", want \"%s\" (%s:%d)\n",
                current_test, got ? got : "(null)", want, file, line);
    }
}

#define CHECK_STR(got, want) check_str((got), (want), __FILE__, __LINE__)

#define TEST(name) do { current_test = name; } while (0)

/* ------------------------------------------------------------------ buf --- */

static void test_buf(void)
{
    Buf b;

    TEST("buf grows, keeps a NUL, and drains from the front");
    buf_init(&b);
    CHECK(buf_append_str(&b, "hello ") == 0);
    CHECK(buf_append_str(&b, "world") == 0);
    CHECK(b.len == 11);
    CHECK_STR(b.data, "hello world");

    buf_drain_front(&b, 6);
    CHECK(b.len == 5);
    CHECK_STR(b.data, "world");

    buf_drain_front(&b, 99);
    CHECK(b.len == 0);
    CHECK_STR(b.data, "");

    TEST("buf_appendf handles a value longer than the stack probe");
    buf_clear(&b);
    {
        char big[400];
        memset(big, 'x', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        CHECK(buf_appendf(&b, "%s", big) == 0);
        CHECK(b.len == sizeof(big) - 1);
        CHECK(b.data[b.len] == '\0');
    }
    buf_free(&b);
}

/* ----------------------------------------------------------------- m3u8 --- */

static const char MASTER_URL[] =
    "http://as-hls-ww-live.akamaized.net/pool_01505109/live/ww/"
    "bbc_radio_one/bbc_radio_one.isml/bbc_radio_one.m3u8";

static const char MASTER_PLAYLIST[] =
    "#EXTM3U\n"
    "#EXT-X-VERSION:3\n"
    "## Created with Unified Streaming Platform  (version=1.13.5-30103)\n"
    "\n"
    "# variants\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=56000,AVERAGE-BANDWIDTH=51000,CODECS=\"mp4a.40.5\"\n"
    "bbc_radio_one-audio=48000.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=112000,AVERAGE-BANDWIDTH=102000,CODECS=\"mp4a.40.5\"\n"
    "bbc_radio_one-audio=96000.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=150000,AVERAGE-BANDWIDTH=136000,CODECS=\"mp4a.40.2\"\n"
    "bbc_radio_one-audio=128000.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=374000,AVERAGE-BANDWIDTH=340000,CODECS=\"mp4a.40.2\"\n"
    "bbc_radio_one-audio=320000.m3u8\n";

static const char MEDIA_URL[] =
    "http://as-hls-ww-live.akamaized.net/pool_01505109/live/ww/"
    "bbc_radio_one/bbc_radio_one.isml/bbc_radio_one-audio=320000.m3u8";

static const char MEDIA_PLAYLIST[] =
    "#EXTM3U\n"
    "#EXT-X-VERSION:3\n"
    "#EXT-X-MEDIA-SEQUENCE:278793294\n"
    "#EXT-X-INDEPENDENT-SEGMENTS\n"
    "#EXT-X-TARGETDURATION:6\n"
    "#USP-X-TIMESTAMP-MAP:MPEGTS=4700405152,LOCAL=2026-07-17T08:31:15.200000Z\n"
    "#EXT-X-PROGRAM-DATE-TIME:2026-07-17T08:31:15.200000Z\n"
    "#EXTINF:6.4, no desc\n"
    "bbc_radio_one-audio=320000-278793294.ts\n"
    "#EXTINF:6.4, no desc\n"
    "bbc_radio_one-audio=320000-278793295.ts\n"
    "#EXTINF:6.4, no desc\n"
    "bbc_radio_one-audio=320000-278793296.ts\n";

static void expect_resolved(const char *base, const char *uri, const char *want)
{
    char *got = m3u8_resolve_url(base, uri);
    CHECK_STR(got, want);
    free(got);
}

static void test_m3u8(void)
{
    M3u8Variant *variants = NULL;
    M3u8MediaPlaylist media;
    size_t count = 0;

    TEST("master and media playlists are told apart");
    CHECK(m3u8_is_master_playlist(MASTER_PLAYLIST));
    CHECK(!m3u8_is_master_playlist(MEDIA_PLAYLIST));

    TEST("master variants parse with relative URIs resolved");
    CHECK(m3u8_parse_master(MASTER_PLAYLIST, MASTER_URL, &variants, &count) == 0);
    CHECK(count == 4);
    if (count == 4) {
        CHECK(variants[3].bandwidth == 374000);
        CHECK_STR(variants[3].uri,
                  "http://as-hls-ww-live.akamaized.net/pool_01505109/live/ww/"
                  "bbc_radio_one/bbc_radio_one.isml/bbc_radio_one-audio=320000.m3u8");
    }
    m3u8_free_variants(variants, count);

    TEST("live media playlist parses");
    CHECK(m3u8_parse_media(MEDIA_PLAYLIST, MEDIA_URL, &media) == 0);
    CHECK(media.segment_count == 3);
    CHECK(media.media_sequence == 278793294L);
    CHECK(media.target_duration == 6.0);
    CHECK(!media.is_end_list);
    if (media.segment_count == 3) {
        CHECK(media.segments[0].duration == 6.4);
        CHECK_STR(media.segments[0].uri,
                  "http://as-hls-ww-live.akamaized.net/pool_01505109/live/ww/"
                  "bbc_radio_one/bbc_radio_one.isml/"
                  "bbc_radio_one-audio=320000-278793294.ts");
    }
    m3u8_free_media(&media);

    TEST("all four URI shapes resolve");
    expect_resolved("https://host.example/a/b/playlist.m3u8",
                    "https://other.example/x.ts", "https://other.example/x.ts");
    expect_resolved("https://host.example/a/b/playlist.m3u8", "/root.ts",
                    "https://host.example/root.ts");
    /* Protocol-relative: keep the scheme, replace the host. */
    expect_resolved("http://ls.qingting.fm/live/4025.m3u8", "//cdn.example/a/b.aac",
                    "http://cdn.example/a/b.aac");
    expect_resolved("https://host.example/a/b/playlist.m3u8", "//cdn.example/a/b.aac",
                    "https://cdn.example/a/b.aac");
    expect_resolved("https://host.example/a/b/playlist.m3u8", "seg.ts",
                    "https://host.example/a/b/seg.ts");
    expect_resolved("https://host.example", "seg.ts", "https://host.example/seg.ts");
    /* A query string on the playlist URL must not be mistaken for a path. */
    expect_resolved("https://host.example/a/list.m3u8?t=1/2", "seg.ts",
                    "https://host.example/a/seg.ts");

    TEST("#EXT-X-ENDLIST is detected");
    CHECK(m3u8_parse_media("#EXTM3U\n#EXTINF:4.0,\na.ts\n#EXT-X-ENDLIST\n",
                           "https://host.example/v/list.m3u8", &media) == 0);
    CHECK(media.is_end_list);
    CHECK(media.segment_count == 1);
    m3u8_free_media(&media);

    TEST("CRLF line endings are handled");
    CHECK(m3u8_parse_media("#EXTM3U\r\n#EXTINF:4.0,\r\na.ts\r\n",
                           "https://host.example/v/list.m3u8", &media) == 0);
    CHECK(media.segment_count == 1);
    if (media.segment_count == 1)
        CHECK_STR(media.segments[0].uri, "https://host.example/v/a.ts");
    m3u8_free_media(&media);
}

/* -------------------------------------------------------------- tsdemux --- */

#define TS_PACKET_SIZE 188

static void ts_packet(Buf *out, unsigned pid, int payload_start,
                      const unsigned char *payload, size_t payload_len)
{
    unsigned char packet[TS_PACKET_SIZE];

    memset(packet, 0xFF, sizeof(packet));
    packet[0] = 0x47;
    packet[1] = (unsigned char)((payload_start ? 0x40 : 0x00) | ((pid >> 8) & 0x1F));
    packet[2] = (unsigned char)(pid & 0xFF);
    packet[3] = 0x10; /* payload only, continuity counter 0 */
    memcpy(packet + 4, payload, payload_len);
    buf_append(out, packet, sizeof(packet));
}

/* PAT with a single program pointing at PMT PID 0x1000. */
static void pat_payload(Buf *out)
{
    static const unsigned char section[] = {
        0x00,                                /* table_id (PAT) */
        0xB0, 0x0D,                          /* syntax indicator + section_length 13 */
        0x00, 0x01, 0xC1, 0x00, 0x00,        /* transport_stream_id ... last_section */
        0x00, 0x01,                          /* program_number 1 */
        0xF0, 0x00,                          /* reserved + PMT PID 0x1000 */
        0xDE, 0xAD, 0xBE, 0xEF               /* CRC32 */
    };
    unsigned char pointer_field = 0x00;

    buf_append(out, &pointer_field, 1);
    buf_append(out, section, sizeof(section));
}

/* PMT declaring one audio stream of `stream_type` on PID 0x0101. */
static void pmt_payload(Buf *out, unsigned char stream_type)
{
    unsigned char section[] = {
        0x02,                                /* table_id (PMT) */
        0xB0, 0x12,                          /* section_length = 18 */
        0x00, 0x01, 0xC1, 0x00, 0x00,        /* program_number ... last_section */
        0xE1, 0x00,                          /* PCR PID */
        0xF0, 0x00,                          /* program_info_length = 0 */
        0x00,                                /* stream_type, filled in below */
        0xE1, 0x01,                          /* elementary PID 0x0101 */
        0xF0, 0x00,                          /* ES_info_length = 0 */
        0xDE, 0xAD, 0xBE, 0xEF               /* CRC32 */
    };
    unsigned char pointer_field = 0x00;

    section[12] = stream_type;
    buf_append(out, &pointer_field, 1);
    buf_append(out, section, sizeof(section));
}

/* PES packet start whose payload is `audio`, with no optional header bytes. */
static void pes_payload(Buf *out, const unsigned char *audio, size_t len)
{
    static const unsigned char header[] = {
        0x00, 0x00, 0x01, /* packet_start_code_prefix */
        0xC0,             /* stream_id (audio) */
        0x00, 0x00,       /* PES_packet_length */
        0x80, 0x00,       /* flags */
        0x00              /* PES_header_data_length = 0 */
    };
    buf_append(out, header, sizeof(header));
    buf_append(out, audio, len);
}

static int contains(const Buf *haystack, const unsigned char *needle, size_t len)
{
    size_t index;
    if (haystack->len < len)
        return 0;
    for (index = 0; index + len <= haystack->len; index++) {
        if (memcmp(haystack->data + index, needle, len) == 0)
            return 1;
    }
    return 0;
}

static void test_tsdemux(void)
{
    Buf stream, payload, out;
    static const unsigned char audio[] = { 0x11, 0x22, 0x33 };
    static const unsigned char more[]  = { 0x44, 0x55 };

    TEST("AAC elementary stream is extracted across PES and continuation packets");
    buf_init(&stream);
    buf_init(&payload);
    buf_init(&out);

    pat_payload(&payload);
    ts_packet(&stream, 0x0000, 1, (unsigned char *)payload.data, payload.len);
    buf_clear(&payload);
    pmt_payload(&payload, 0x0F);
    ts_packet(&stream, 0x1000, 1, (unsigned char *)payload.data, payload.len);
    buf_clear(&payload);
    pes_payload(&payload, audio, sizeof(audio));
    ts_packet(&stream, 0x0101, 1, (unsigned char *)payload.data, payload.len);
    ts_packet(&stream, 0x0101, 0, more, sizeof(more));

    CHECK(tsdemux_extract((unsigned char *)stream.data, stream.len, &out) ==
          RWR_CODEC_ADTS_AAC);
    CHECK(out.len >= 3);
    if (out.len >= 3)
        CHECK(memcmp(out.data, audio, 3) == 0);
    CHECK(contains(&out, more, sizeof(more)));

    TEST("MPEG audio stream_type is recognized");
    buf_clear(&stream);
    buf_clear(&payload);
    buf_clear(&out);
    pat_payload(&payload);
    ts_packet(&stream, 0x0000, 1, (unsigned char *)payload.data, payload.len);
    buf_clear(&payload);
    pmt_payload(&payload, 0x03);
    ts_packet(&stream, 0x1000, 1, (unsigned char *)payload.data, payload.len);
    buf_clear(&payload);
    pes_payload(&payload, audio, 1);
    ts_packet(&stream, 0x0101, 1, (unsigned char *)payload.data, payload.len);
    CHECK(tsdemux_extract((unsigned char *)stream.data, stream.len, &out) ==
          RWR_CODEC_MPEG_AUDIO);

    TEST("a misaligned stream resyncs");
    buf_clear(&stream);
    buf_clear(&out);
    {
        static const unsigned char junk[] = { 0x00, 0x11 };
        buf_append(&stream, junk, sizeof(junk));
    }
    buf_clear(&payload);
    pat_payload(&payload);
    ts_packet(&stream, 0x0000, 1, (unsigned char *)payload.data, payload.len);
    buf_clear(&payload);
    pmt_payload(&payload, 0x0F);
    ts_packet(&stream, 0x1000, 1, (unsigned char *)payload.data, payload.len);
    buf_clear(&payload);
    pes_payload(&payload, audio, 1);
    ts_packet(&stream, 0x0101, 1, (unsigned char *)payload.data, payload.len);
    CHECK(tsdemux_extract((unsigned char *)stream.data, stream.len, &out) ==
          RWR_CODEC_ADTS_AAC);

    TEST("garbage yields no stream rather than reading out of bounds");
    buf_clear(&out);
    {
        unsigned char garbage[400];
        memset(garbage, 0x47, sizeof(garbage));
        CHECK(tsdemux_extract(garbage, sizeof(garbage), &out) == RWR_CODEC_UNKNOWN);
        CHECK(out.len == 0);
        CHECK(tsdemux_extract(garbage, 0, &out) == RWR_CODEC_UNKNOWN);
    }

    buf_free(&stream);
    buf_free(&payload);
    buf_free(&out);
}

/* -------------------------------------------------------------- segment --- */

static void adts_frame(Buf *out)
{
    /* syncword 0xFFF, MPEG-4, no CRC, AAC-LC, 44.1kHz, stereo */
    static const unsigned char frame[] = {
        0xFF, 0xF1, 0x50, 0x80, 0x01, 0xFF, 0xFC, 0x00, 0x11, 0x22
    };
    buf_append(out, frame, sizeof(frame));
}

static void id3v2_tag(Buf *out, size_t payload_len)
{
    unsigned char header[10];
    size_t index;

    header[0] = 'I'; header[1] = 'D'; header[2] = '3';
    header[3] = 0x04; header[4] = 0x00; header[5] = 0x00;
    header[6] = (unsigned char)((payload_len >> 21) & 0x7F);
    header[7] = (unsigned char)((payload_len >> 14) & 0x7F);
    header[8] = (unsigned char)((payload_len >> 7) & 0x7F);
    header[9] = (unsigned char)(payload_len & 0x7F);
    buf_append(out, header, sizeof(header));
    for (index = 0; index < payload_len; index++) {
        unsigned char zero = 0;
        buf_append(out, &zero, 1);
    }
}

static void test_segment(void)
{
    Buf segment, out, expected;

    buf_init(&segment);
    buf_init(&out);
    buf_init(&expected);
    adts_frame(&expected);

    TEST("a packed ADTS segment passes through untouched");
    adts_frame(&segment);
    CHECK(segment_elementary_stream((unsigned char *)segment.data, segment.len,
                                    "http://h/a/b.aac", &out) == RWR_CODEC_ADTS_AAC);
    CHECK(out.len == expected.len && memcmp(out.data, expected.data, out.len) == 0);

    TEST("a leading ID3 tag is stripped");
    buf_clear(&segment);
    buf_clear(&out);
    id3v2_tag(&segment, 64);
    adts_frame(&segment);
    CHECK(segment_elementary_stream((unsigned char *)segment.data, segment.len,
                                    "http://h/a/b.aac", &out) == RWR_CODEC_ADTS_AAC);
    CHECK(out.len == expected.len && memcmp(out.data, expected.data, out.len) == 0);

    TEST("stacked ID3 tags are all stripped");
    buf_clear(&segment);
    buf_clear(&out);
    id3v2_tag(&segment, 16);
    id3v2_tag(&segment, 8);
    adts_frame(&segment);
    CHECK(segment_elementary_stream((unsigned char *)segment.data, segment.len,
                                    "http://h/a/b.aac", &out) == RWR_CODEC_ADTS_AAC);
    CHECK(out.len == expected.len && memcmp(out.data, expected.data, out.len) == 0);

    TEST("an MPEG audio syncword is recognized");
    buf_clear(&out);
    {
        /* 0xFFFB: MPEG-1 Layer III */
        static const unsigned char mpeg[] = { 0xFF, 0xFB, 0x90, 0x00, 0x01, 0x02 };
        CHECK(segment_elementary_stream(mpeg, sizeof(mpeg), "http://h/a/b.ts", &out) ==
              RWR_CODEC_MPEG_AUDIO);
    }

    TEST("the extension is the fallback when we joined mid-frame");
    buf_clear(&out);
    {
        static const unsigned char junk[] = { 0x12, 0x34, 0x56, 0x78 };
        CHECK(segment_elementary_stream(junk, sizeof(junk), "http://h/a/b.aac?token=1",
                                        &out) == RWR_CODEC_ADTS_AAC);
        buf_clear(&out);
        CHECK(segment_elementary_stream(junk, sizeof(junk), "http://h/a/b.bin", &out) ==
              RWR_CODEC_UNKNOWN);
    }

    TEST("an ADTS frame full of 0x47 bytes is still read as AAC");
    /* Compressed audio can contain any byte pattern, including one that
     * satisfies the 188-byte TS stride heuristic. The leading syncword is
     * checked first precisely so that cannot hijack the segment. */
    buf_clear(&segment);
    buf_clear(&out);
    adts_frame(&segment);
    while (segment.len < 400) {
        unsigned char filler = 0x47;
        buf_append(&segment, &filler, 1);
    }
    CHECK(segment_looks_like_mpeg_ts((unsigned char *)segment.data, segment.len));
    CHECK(segment_elementary_stream((unsigned char *)segment.data, segment.len,
                                    "http://h/a/b.aac", &out) == RWR_CODEC_ADTS_AAC);

    TEST("TS bytes never fall through to the packed-audio path");
    buf_clear(&segment);
    buf_clear(&out);
    {
        unsigned char ts[400];
        memset(ts, 0x00, sizeof(ts));
        ts[0] = 0x47;
        ts[188] = 0x47;
        CHECK(segment_looks_like_mpeg_ts(ts, sizeof(ts)));
        CHECK(segment_elementary_stream(ts, sizeof(ts), "http://h/a/b.ts", &out) ==
              RWR_CODEC_UNKNOWN);
        CHECK(out.len == 0);
    }

    TEST("a truncated ID3 tag yields nothing rather than reading past the end");
    buf_clear(&segment);
    buf_clear(&out);
    id3v2_tag(&segment, 1000); /* claims more payload than it has */
    segment.len = 200;
    CHECK(segment_elementary_stream((unsigned char *)segment.data, segment.len,
                                    "http://h/a/b.aac", &out) == RWR_CODEC_UNKNOWN);
    CHECK(segment_elementary_stream((unsigned char *)"", 0, "http://h/a/b.aac", &out) ==
          RWR_CODEC_UNKNOWN);

    buf_free(&segment);
    buf_free(&out);
    buf_free(&expected);
}

/* --------------------------------------------------------------- format --- */

static void test_format(void)
{
    StreamFormat format;

    TEST("the syncword beats a wrong Content-Type");
    {
        static const unsigned char aac[] = { 0xFF, 0xF9, 0x50, 0x80 };
        static const unsigned char mpa[] = { 0xFF, 0xFB, 0x90, 0x00 };
        format = format_for_http(aac, sizeof(aac), "audio/mpeg", "http://h/s", "MP3");
        CHECK_STR(format.description, "raw ADTS AAC");
        format = format_for_http(mpa, sizeof(mpa), "audio/aac", "http://h/s", "AAC");
        CHECK_STR(format.description, "raw MPEG audio");
    }

    TEST("an ID3-prefixed body is treated as MPEG audio");
    format = format_for_http((const unsigned char *)"ID3\x04\x00\x00\x00\x00\x00\x20",
                             10, NULL, "http://h/s", "");
    CHECK_STR(format.description, "raw MPEG audio");

    TEST("Content-Type is used when the body gives nothing away");
    {
        static const unsigned char nothing[] = { 0x00, 0x01 };
        format = format_for_http(nothing, 2, "audio/aacp; charset=utf-8", "http://h/s", "");
        CHECK_STR(format.description, "raw ADTS AAC");
        format = format_for_http(nothing, 2, "audio/mpeg", "http://h/s", "");
        CHECK_STR(format.description, "raw MPEG audio");
        format = format_for_http(nothing, 2, "application/ogg", "http://h/s", "");
        CHECK(format.codec == RWR_CODEC_UNKNOWN);
        CHECK_STR(format.description, "Ogg (unsupported by this build)");
    }

    TEST("the URL extension then the dataset codec are the last resorts");
    {
        static const unsigned char nothing[] = { 0x00 };
        format = format_for_http(nothing, 1, NULL, "http://h/s.aac?t=1", "");
        CHECK_STR(format.description, "raw ADTS AAC");
        format = format_for_http(nothing, 1, NULL, "http://h/s.ogg", "");
        CHECK_STR(format.description, "Ogg (unsupported by this build)");
        format = format_for_http(nothing, 1, NULL, "http://h/listen", "MP3");
        CHECK_STR(format.description, "raw MPEG audio");
        format = format_for_http(nothing, 1, NULL, "http://h/listen", "UNKNOWN");
        CHECK_STR(format.description, "unrecognized stream");
    }

    TEST("the HLS codec maps directly");
    CHECK_STR(format_for_hls(RWR_CODEC_ADTS_AAC).description, "raw ADTS AAC");
    CHECK_STR(format_for_hls(RWR_CODEC_MPEG_AUDIO).description, "raw MPEG audio");
    CHECK(format_for_hls(RWR_CODEC_UNKNOWN).codec == RWR_CODEC_UNKNOWN);
}

/* -------------------------------------------------------------- dataset --- */

static void test_dataset(const char *path)
{
    char error[512];
    Dataset *dataset = dataset_open(path, error, sizeof(error));
    int countries, index, total = 0;
    Station station;

    if (!dataset) {
        fprintf(stderr, "skipping dataset tests: %s\n", error);
        return;
    }

    TEST("the dataset opens and its country list is sorted and non-empty");
    countries = dataset_country_count(dataset);
    CHECK(countries > 100);
    for (index = 1; index < countries; index++) {
        if (strcmp(dataset_country_name(dataset, index - 1),
                   dataset_country_name(dataset, index)) >= 0) {
            CHECK(0 && "country names must be strictly ascending");
            break;
        }
    }

    TEST("every station in every country reads back with a name and a URL");
    for (index = 0; index < countries; index++) {
        int count = dataset_station_count(dataset, index), which;
        total += count;
        for (which = 0; which < count; which++) {
            if (dataset_station(dataset, index, which, &station) != 0) {
                CHECK(0 && "station index in range must resolve");
                break;
            }
            if (station.name[0] == '\0' || station.url[0] == '\0') {
                CHECK(0 && "mkdataset.py drops nameless and URL-less stations");
                break;
            }
        }
    }
    CHECK(total == dataset_total_station_count(dataset));

    TEST("out-of-range indices are rejected rather than read");
    CHECK(dataset_station(dataset, -1, 0, &station) == -1);
    CHECK(dataset_station(dataset, countries, 0, &station) == -1);
    CHECK(dataset_station(dataset, 0, -1, &station) == -1);
    CHECK(dataset_station(dataset, 0, dataset_station_count(dataset, 0), &station) == -1);
    CHECK_STR(dataset_country_name(dataset, countries), "");

    TEST("every accessor tolerates a NULL dataset");
    /* dataset_open failing is a normal outcome - the UI keeps running and says
     * so in the status row - so nothing may dereference a NULL Dataset. */
    CHECK(dataset_country_count(NULL) == 0);
    CHECK(dataset_total_station_count(NULL) == 0);
    CHECK(dataset_station_count(NULL, 0) == 0);
    CHECK_STR(dataset_country_name(NULL, 0), "");
    CHECK_STR(dataset_path(NULL), "");
    CHECK(dataset_station(NULL, 0, 0, &station) == -1);

    TEST("codec/bitrate labels follow the Rust port's rules");
    {
        char label[128];
        Station fake;
        memset(&fake, 0, sizeof(fake));
        fake.name = "N"; fake.url = "U"; fake.country = ""; fake.language = "";

        fake.codec = "MP3"; fake.bitrate = 128;
        CHECK_STR(station_codec_bitrate_label(&fake, label, sizeof(label)), "MP3 128kbps");
        fake.codec = "AAC"; fake.bitrate = 0;
        CHECK_STR(station_codec_bitrate_label(&fake, label, sizeof(label)), "AAC");
        /* "UNKNOWN" is radio-browser's placeholder, not something to show. */
        fake.codec = "UNKNOWN"; fake.bitrate = 96;
        CHECK_STR(station_codec_bitrate_label(&fake, label, sizeof(label)), "96kbps");
        fake.codec = "UNKNOWN"; fake.bitrate = 0;
        CHECK_STR(station_codec_bitrate_label(&fake, label, sizeof(label)), "");
    }

    fprintf(stderr, "dataset: %d countries, %d stations\n", countries, total);
    dataset_close(dataset);
}

/* -------------------------------------------------------------- playlist --- */

static void test_playlist(void)
{
    char out[512];

    TEST(".m3u8 is recognized regardless of query string or case");
    CHECK(playlist_is_hls_url("http://h/live.m3u8"));
    CHECK(playlist_is_hls_url("https://h/a/b.M3U8?Policy=abc&Signature=def"));
    CHECK(!playlist_is_hls_url("http://h/listen"));
    CHECK(!playlist_is_hls_url("http://h/stream.mp3"));
    /* An .m3u is the legacy pointer format, not HLS - it must not go down the
     * HLS path. */
    CHECK(!playlist_is_hls_url("http://h/stream.m3u"));

    TEST("a playlist body is recognized from its own first bytes");
    CHECK(playlist_looks_like_playlist((const unsigned char *)"#EXTM3U\n#EXT", 12, NULL));
    CHECK(playlist_looks_like_playlist((const unsigned char *)"#EXT-X-VERSION", 14, NULL));
    CHECK(playlist_looks_like_playlist((const unsigned char *)"[playlist]\nNum", 14, NULL));
    CHECK(playlist_looks_like_playlist((const unsigned char *)"[PlayList]\nNum", 14, NULL));
    /* Some .m3u endpoints return just the stream URL with a content type of
     * application/octet-stream. No audio bitstream begins with ASCII "http". */
    CHECK(playlist_looks_like_playlist((const unsigned char *)"http://h/x", 10,
                                       "application/octet-stream"));
    CHECK(playlist_looks_like_playlist((const unsigned char *)"https://h/x", 11, NULL));

    TEST("audio is not mistaken for a playlist");
    {
        static const unsigned char adts[] = { 0xFF, 0xF1, 0x50, 0x80 };
        static const unsigned char mpa[]  = { 0xFF, 0xFB, 0x90, 0x00 };
        static const unsigned char id3[]  = { 'I', 'D', '3', 0x04, 0x00, 0x00 };
        CHECK(!playlist_looks_like_playlist(adts, sizeof(adts), "audio/aac"));
        CHECK(!playlist_looks_like_playlist(mpa, sizeof(mpa), "audio/mpeg"));
        CHECK(!playlist_looks_like_playlist(id3, sizeof(id3), NULL));
        CHECK(!playlist_looks_like_playlist((const unsigned char *)"", 0, NULL));
    }

    TEST("a playlist content type counts even when the body is unhelpful");
    {
        static const unsigned char nothing[] = { 0x00, 0x01 };
        CHECK(playlist_looks_like_playlist(nothing, 2, "application/x-mpegurl"));
        CHECK(playlist_looks_like_playlist(nothing, 2, "audio/x-scpls; charset=utf-8"));
        CHECK(playlist_looks_like_playlist(nothing, 2,
                                           "  application/vnd.apple.mpegurl  "));
        CHECK(!playlist_looks_like_playlist(nothing, 2, "audio/mpeg"));
    }

    TEST(".pls File1= entries are followed, other keys skipped");
    CHECK(playlist_first_stream_url(
        "[playlist]\nnumberofentries=1\nTitle1=Some Station\n"
        "File1=http://stream.example/live\nLength1=-1\n", out, sizeof(out)));
    CHECK_STR(out, "http://stream.example/live");

    TEST(".m3u bare URL lines are followed and comments skipped");
    CHECK(playlist_first_stream_url("#EXTM3U\n#EXTINF:-1,Station\n"
                                    "https://stream.example/aac\n", out, sizeof(out)));
    CHECK_STR(out, "https://stream.example/aac");

    TEST("a playlist with no URL reports nothing rather than garbage");
    CHECK(!playlist_first_stream_url("[playlist]\nnumberofentries=0\n", out, sizeof(out)));
    CHECK(!playlist_first_stream_url("", out, sizeof(out)));
    CHECK(!playlist_first_stream_url("#EXTM3U\n#EXTINF:-1,Nothing\n", out, sizeof(out)));

    TEST("the TuneIn resolver response is trimmed to its first real line");
    CHECK(playlist_first_line("\r\n  \r\nhttp://stream.example/live?x=1  \r\n",
                              out, sizeof(out)));
    CHECK_STR(out, "http://stream.example/live?x=1");
    CHECK(!playlist_first_line("   \r\n\t\n", out, sizeof(out)));
    CHECK(!playlist_first_line("", out, sizeof(out)));
}

/* ------------------------------------------------------------ bytequeue --- */

typedef struct {
    ByteQueue *queue;
    size_t     total;
} PushArgs;

/* Pushes more than the queue's 2 MB capacity, so the producer must actually
 * block and be released by the reader rather than the test passing by accident
 * on a queue that never fills. */
static void *push_thread(void *argument)
{
    PushArgs *args = (PushArgs *)argument;
    unsigned char chunk[4096];
    size_t sent = 0, index;

    for (index = 0; index < sizeof(chunk); index++)
        chunk[index] = (unsigned char)(index & 0xFF);

    while (sent < args->total) {
        size_t want = args->total - sent;
        if (want > sizeof(chunk))
            want = sizeof(chunk);
        if (!bytequeue_push(args->queue, chunk, want))
            break;
        sent += want;
    }
    bytequeue_close(args->queue, NULL);
    return NULL;
}

static void test_bytequeue(void)
{
    ByteQueue *queue;
    char error[256];
    unsigned char out[8192];
    long got;

    TEST("pushed bytes read back in order, then a clean EOF");
    queue = bytequeue_new();
    CHECK(queue != NULL);
    CHECK(bytequeue_push(queue, "hello ", 6));
    CHECK(bytequeue_push(queue, "world", 5));
    bytequeue_close(queue, NULL);

    error[0] = '\0';
    got = bytequeue_read(queue, out, sizeof(out), error, sizeof(error));
    CHECK(got == 11);
    CHECK(memcmp(out, "hello world", 11) == 0);
    CHECK(bytequeue_read(queue, out, sizeof(out), error, sizeof(error)) == 0);
    bytequeue_release(queue);

    TEST("close with an error surfaces after the buffered bytes drain");
    queue = bytequeue_new();
    bytequeue_push(queue, "partial", 7);
    bytequeue_close(queue, "segment fetch failed");
    CHECK(bytequeue_read(queue, out, sizeof(out), error, sizeof(error)) == 7);
    error[0] = '\0';
    CHECK(bytequeue_read(queue, out, sizeof(out), error, sizeof(error)) == -1);
    CHECK_STR(error, "segment fetch failed");
    /* The error is reported once; a reader that keeps going sees a clean EOF. */
    CHECK(bytequeue_read(queue, out, sizeof(out), error, sizeof(error)) == 0);
    bytequeue_release(queue);

    TEST("peek does not consume");
    queue = bytequeue_new();
    bytequeue_push(queue, "\xff\xf9rest", 6);
    CHECK(bytequeue_peek(queue, out, 2, 50) == 2);
    CHECK(out[0] == 0xFF && out[1] == 0xF9);
    bytequeue_close(queue, NULL);
    CHECK(bytequeue_read(queue, out, sizeof(out), error, sizeof(error)) == 6);
    CHECK(memcmp(out, "\xff\xf9rest", 6) == 0);
    bytequeue_release(queue);

    TEST("peek returns what it has when the stream is short");
    queue = bytequeue_new();
    bytequeue_push(queue, "\xff", 1);
    bytequeue_close(queue, NULL);
    CHECK(bytequeue_peek(queue, out, 4, 50) == 1);
    bytequeue_release(queue);

    TEST("push after close is rejected");
    queue = bytequeue_new();
    bytequeue_close(queue, NULL);
    CHECK(!bytequeue_push(queue, "ignored", 7));
    CHECK(bytequeue_is_closed(queue));
    bytequeue_release(queue);

    TEST("a producer blocked on a full ring is released by the reader");
    {
        pthread_t thread;
        PushArgs args;
        size_t received = 0;
        size_t expected = 5u * 1024u * 1024u; /* well over the 2 MB capacity */

        queue = bytequeue_new();
        args.queue = bytequeue_retain(queue);
        args.total = expected;
        CHECK(pthread_create(&thread, NULL, push_thread, &args) == 0);

        for (;;) {
            got = bytequeue_read(queue, out, sizeof(out), error, sizeof(error));
            if (got <= 0)
                break;
            /* The ring wraps many times over 5 MB; a wrap that lost or
             * duplicated bytes would show up here. */
            {
                long index;
                for (index = 0; index < got; index++) {
                    if (out[index] != (unsigned char)((received + (size_t)index) % 4096)) {
                        CHECK(0 && "byte stream corrupted across a ring wrap");
                        goto drained;
                    }
                }
            }
            received += (size_t)got;
        }
    drained:
        pthread_join(thread, NULL);
        CHECK(received == expected);
        bytequeue_release(args.queue);
        bytequeue_release(queue);
    }

    TEST("wait_for_data times out rather than blocking forever");
    queue = bytequeue_new();
    CHECK(!bytequeue_wait_for_data(queue, 60));
    bytequeue_push(queue, "x", 1);
    CHECK(bytequeue_wait_for_data(queue, 60));
    bytequeue_release(queue);
}

/* ----------------------------------------------------------- audio sink --- */

static void test_audiosink(void)
{
    AudioSink *sink = audiosink_new(2);
    float in[512], out[512];
    int index;

    TEST("the sink pads an underrun with silence and tracks the peak");
    CHECK(sink != NULL);
    for (index = 0; index < 512; index++)
        in[index] = (index == 7) ? -0.5f : 0.25f;
    CHECK(audiosink_push(sink, in, 256));

    for (index = 0; index < 512; index++)
        out[index] = 1234.0f;
    audiosink_fill(sink, out, 512);
    CHECK(out[0] == 0.25f);
    CHECK(out[7] == -0.5f);
    /* Everything past what was pushed must be silence, not stale data. */
    CHECK(out[256] == 0.0f);
    CHECK(out[511] == 0.0f);
    CHECK(audiosink_level(sink) == 0.5f);

    TEST("mute silences output and releases a blocked producer");
    audiosink_push(sink, in, 256);
    audiosink_mute(sink);
    for (index = 0; index < 512; index++)
        out[index] = 1234.0f;
    audiosink_fill(sink, out, 512);
    CHECK(out[0] == 0.0f);
    CHECK(out[511] == 0.0f);
    CHECK(audiosink_level(sink) == 0.0f);
    /* Once muted, push is the decoder's signal to stop. */
    CHECK(!audiosink_push(sink, in, 512 * 400));

    audiosink_release(sink);
}

/* ------------------------------------------------------------------ der --- */

/* The concatenated-DER CA store is walked by hand rather than by mbedTLS (which
 * parses only the first certificate out of non-PEM input), so the walk is worth
 * checking against the real file this build ships. */
static void test_der(const char *path)
{
    FILE *file = fopen(path, "rb");
    unsigned char *data;
    long size;
    size_t offset = 0;
    int count = 0;

    TEST("the CA store walks cleanly end to end");

    if (!file) {
        fprintf(stderr, "skipping DER tests: %s not present\n", path);
        return;
    }
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    data = (unsigned char *)malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        free(data);
        CHECK(0 && "could not read the CA store");
        return;
    }
    fclose(file);

    while (offset < (size_t)size) {
        size_t object = der_object_len(data + offset, (size_t)size - offset);
        if (object == 0)
            break;
        offset += object;
        count++;
    }

    /* Every byte must be accounted for: a walk that stops short would silently
     * drop trust anchors, and one that overruns would read past the buffer. */
    CHECK(offset == (size_t)size);
    CHECK(count > 100);
    fprintf(stderr, "CA store: %d certificates, %ld bytes\n", count, size);

    TEST("malformed DER is rejected rather than walked past");
    {
        static const unsigned char not_a_sequence[] = { 0x31, 0x02, 0x00, 0x00 };
        static const unsigned char length_overruns[] = { 0x30, 0x82, 0xFF, 0xFF, 0x00 };
        static const unsigned char truncated_header[] = { 0x30, 0x84, 0x00 };
        static const unsigned char short_form[] = { 0x30, 0x02, 0xAA, 0xBB };

        CHECK(der_object_len(not_a_sequence, sizeof(not_a_sequence)) == 0);
        CHECK(der_object_len(length_overruns, sizeof(length_overruns)) == 0);
        CHECK(der_object_len(truncated_header, sizeof(truncated_header)) == 0);
        CHECK(der_object_len(short_form, sizeof(short_form)) == 4);
        CHECK(der_object_len(short_form, 1) == 0);
    }

    free(data);
}

/* ------------------------------------------------------------------ main -- */

int main(int argc, char **argv)
{
    const char *dataset_path = argc > 1 ? argv[1] : "data/stations.dat";
    const char *ca_path = argc > 2 ? argv[2] : "data/cacert.der";

    test_buf();
    test_bytequeue();
    test_audiosink();
    test_m3u8();
    test_tsdemux();
    test_segment();
    test_format();
    test_playlist();
    test_dataset(dataset_path);
    test_der(ca_path);

    if (failures == 0) {
        printf("all %d checks passed\n", checks);
        return 0;
    }
    printf("%d of %d checks FAILED\n", failures, checks);
    return 1;
}
