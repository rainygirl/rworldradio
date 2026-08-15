#include "segment.h"

#include <ctype.h>
#include <string.h>

#define TS_PACKET_SIZE 188

/* Number of leading bytes taken up by ID3v2 tags. Returns `len` when a tag
 * claims more payload than the segment actually has, which leaves no audio -
 * the same outcome as the Rust port returning an empty slice. */
size_t segment_id3_prefix_len(const unsigned char *data, size_t len)
{
    size_t offset = 0;

    while (len - offset >= 10 && memcmp(data + offset, "ID3", 3) == 0) {
        const unsigned char *tag = data + offset;
        unsigned char flags = tag[5];
        /* Size is four syncsafe bytes: 7 significant bits each. */
        size_t size = ((size_t)(tag[6] & 0x7F) << 21) | ((size_t)(tag[7] & 0x7F) << 14) |
                      ((size_t)(tag[8] & 0x7F) << 7)  |  (size_t)(tag[9] & 0x7F);
        size_t footer = (flags & 0x10) ? 10 : 0;
        size_t total = 10 + size + footer;

        if (total >= len - offset)
            return len;
        offset += total;
    }
    return offset;
}

/* MPEG-TS is a stream of 188-byte packets each starting with 0x47. Checking two
 * consecutive sync bytes at the right stride avoids mistaking an ADTS frame that
 * happens to contain a 0x47 for a transport stream. */
int segment_looks_like_mpeg_ts(const unsigned char *data, size_t len)
{
    size_t limit = len < TS_PACKET_SIZE ? len : TS_PACKET_SIZE;
    size_t start;

    if (limit == 0)
        limit = 1; /* matches the Rust `.max(1)`; the bounds check below still fails */

    for (start = 0; start < limit; start++) {
        if (start >= len || data[start] != 0x47)
            continue;
        if (start + TS_PACKET_SIZE < len && data[start + TS_PACKET_SIZE] == 0x47)
            return 1;
    }
    return 0;
}

/* Identifies a packed-audio bitstream from its leading frame header. */
static RwrAudioCodec syncword_codec(const unsigned char *audio, size_t len)
{
    if (len < 2 || audio[0] != 0xFF)
        return RWR_CODEC_UNKNOWN;
    /* ADTS: syncword 0xFFF, layer bits (0x06) must be zero. */
    if ((audio[1] & 0xF6) == 0xF0)
        return RWR_CODEC_ADTS_AAC;
    /* MPEG audio frame: 11 sync bits, then a non-reserved layer. */
    if ((audio[1] & 0xE0) == 0xE0 && (audio[1] & 0x06) != 0x00)
        return RWR_CODEC_MPEG_AUDIO;
    return RWR_CODEC_UNKNOWN;
}

static int ends_with_ci(const char *text, const char *suffix)
{
    size_t text_len = strlen(text), suffix_len = strlen(suffix), index;

    if (suffix_len > text_len)
        return 0;
    text += text_len - suffix_len;
    for (index = 0; index < suffix_len; index++) {
        if (tolower((unsigned char)text[index]) != (unsigned char)suffix[index])
            return 0;
    }
    return 1;
}

static RwrAudioCodec extension_codec(const char *uri)
{
    char path[1024];
    const char *query = strchr(uri, '?');
    size_t len = query ? (size_t)(query - uri) : strlen(uri);

    if (len >= sizeof(path))
        len = sizeof(path) - 1;
    memcpy(path, uri, len);
    path[len] = '\0';

    if (ends_with_ci(path, ".aac"))
        return RWR_CODEC_ADTS_AAC;
    if (ends_with_ci(path, ".mp3"))
        return RWR_CODEC_MPEG_AUDIO;
    return RWR_CODEC_UNKNOWN;
}

RwrAudioCodec segment_elementary_stream(const unsigned char *data, size_t len,
                                        const char *uri, Buf *out)
{
    size_t id3 = segment_id3_prefix_len(data, len);
    const unsigned char *audio = data + id3;
    size_t audio_len = len - id3;
    RwrAudioCodec codec;

    if (audio_len == 0)
        return RWR_CODEC_UNKNOWN;

    /* A packed-audio segment opens with a codec syncword and MPEG-TS always
     * opens with 0x47, so when a syncword is present there is no ambiguity.
     * Checked before the (necessarily heuristic) TS detection: compressed audio
     * can contain any byte pattern, including one that satisfies the 188-byte
     * stride. */
    codec = syncword_codec(audio, audio_len);
    if (codec != RWR_CODEC_UNKNOWN) {
        buf_append(out, audio, audio_len);
        return codec;
    }

    if (segment_looks_like_mpeg_ts(data, len)) {
        /* A TS container we could not get audio out of is encrypted, video-only
         * or a codec the PMT parser does not recognize. Don't fall through to
         * the packed-audio path - TS bytes are not an elementary stream. */
        return tsdemux_extract(data, len, out);
    }

    /* Neither a syncword nor TS: we probably joined a segment mid-frame. Trust
     * the segment's extension and let the decoder resynchronize. */
    codec = extension_codec(uri);
    if (codec == RWR_CODEC_UNKNOWN)
        return RWR_CODEC_UNKNOWN;
    buf_append(out, audio, audio_len);
    return codec;
}
