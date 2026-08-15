#include "format.h"

#include <ctype.h>
#include <string.h>

static const StreamFormat ADTS      = { RWR_CODEC_ADTS_AAC,   "raw ADTS AAC" };
static const StreamFormat MPEG      = { RWR_CODEC_MPEG_AUDIO, "raw MPEG audio" };
static const StreamFormat UNKNOWN   = { RWR_CODEC_UNKNOWN,    "unrecognized stream" };
static const StreamFormat OGG       = { RWR_CODEC_UNKNOWN,    "Ogg (unsupported by this build)" };
static const StreamFormat FLAC      = { RWR_CODEC_UNKNOWN,    "FLAC (unsupported by this build)" };
static const StreamFormat MP4       = { RWR_CODEC_UNKNOWN,    "MP4/M4A (unsupported by this build)" };
static const StreamFormat WAV       = { RWR_CODEC_UNKNOWN,    "WAV (unsupported by this build)" };

/* Lowercased copy of the media type with any ";charset=..." parameters removed. */
static void normalize_mime(const char *content_type, char *out, size_t size)
{
    size_t index = 0;

    out[0] = '\0';
    if (!content_type)
        return;
    while (*content_type == ' ' || *content_type == '\t')
        content_type++;
    while (*content_type && *content_type != ';' && index + 1 < size) {
        out[index++] = (char)tolower((unsigned char)*content_type);
        content_type++;
    }
    while (index > 0 && (out[index - 1] == ' ' || out[index - 1] == '\t'))
        index--;
    out[index] = '\0';
}

static int equals_any(const char *text, const char *const *options, size_t count)
{
    size_t index;
    for (index = 0; index < count; index++) {
        if (strcmp(text, options[index]) == 0)
            return 1;
    }
    return 0;
}

/* Identifies a raw elementary stream from its leading frame header. Shares the
 * bit patterns with the HLS packed-audio check in segment.c. */
static const StreamFormat *from_syncword(const unsigned char *head, size_t len)
{
    /* An icecast server sometimes prepends an ID3v2 tag to an MP3 body; nothing
     * else this app plays does that, so the tag alone identifies the stream. */
    if (len >= 3 && memcmp(head, "ID3", 3) == 0)
        return &MPEG;
    if (len < 2 || head[0] != 0xFF)
        return NULL;
    if ((head[1] & 0xF6) == 0xF0)
        return &ADTS;
    if ((head[1] & 0xE0) == 0xE0 && (head[1] & 0x06) != 0x00)
        return &MPEG;
    return NULL;
}

static const StreamFormat *container_for_mime(const char *mime)
{
    static const char *const ogg[]  = { "audio/ogg", "application/ogg", "audio/opus",
                                        "audio/vorbis" };
    static const char *const flac[] = { "audio/flac", "audio/x-flac" };
    static const char *const mp4[]  = { "audio/mp4", "audio/x-m4a", "video/mp4" };
    static const char *const wav[]  = { "audio/wav", "audio/x-wav", "audio/wave",
                                        "audio/vnd.wave" };

    if (equals_any(mime, ogg,  sizeof(ogg)  / sizeof(*ogg)))  return &OGG;
    if (equals_any(mime, flac, sizeof(flac) / sizeof(*flac))) return &FLAC;
    if (equals_any(mime, mp4,  sizeof(mp4)  / sizeof(*mp4)))  return &MP4;
    if (equals_any(mime, wav,  sizeof(wav)  / sizeof(*wav)))  return &WAV;
    return NULL;
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

static int equals_ci(const char *text, const char *other)
{
    while (*text && *other) {
        if (tolower((unsigned char)*text) != tolower((unsigned char)*other))
            return 0;
        text++;
        other++;
    }
    return *text == *other;
}

StreamFormat format_for_http(const unsigned char *head, size_t head_len,
                             const char *content_type, const char *url,
                             const char *dataset_codec)
{
    char mime[128];
    char path[1024];
    const StreamFormat *found;
    const char *query;
    size_t len;

    /* 1. The body's own first bytes. A server's content type is frequently
     *    wrong and a URL extension is a guess, so this comes first. */
    found = from_syncword(head, head_len);
    if (found)
        return *found;

    /* 2. Content-Type. */
    normalize_mime(content_type, mime, sizeof(mime));
    if (mime[0]) {
        static const char *const aac[] = { "audio/aac", "audio/aacp", "audio/x-aac",
                                           "audio/x-hx-aac-adts" };
        static const char *const mpa[] = { "audio/mpeg", "audio/mp3", "audio/mpeg3",
                                           "audio/x-mpeg" };
        if (equals_any(mime, aac, sizeof(aac) / sizeof(*aac)))
            return ADTS;
        if (equals_any(mime, mpa, sizeof(mpa) / sizeof(*mpa)))
            return MPEG;
        found = container_for_mime(mime);
        if (found)
            return *found;
    }

    /* 3. The URL's extension. */
    query = url ? strchr(url, '?') : NULL;
    len = url ? (query ? (size_t)(query - url) : strlen(url)) : 0;
    if (len >= sizeof(path))
        len = sizeof(path) - 1;
    if (len)
        memcpy(path, url, len);
    path[len] = '\0';

    if (ends_with_ci(path, ".aac")) return ADTS;
    if (ends_with_ci(path, ".mp3")) return MPEG;
    if (ends_with_ci(path, ".ogg") || ends_with_ci(path, ".oga") ||
        ends_with_ci(path, ".opus"))                return OGG;
    if (ends_with_ci(path, ".flac"))                return FLAC;
    if (ends_with_ci(path, ".m4a") || ends_with_ci(path, ".mp4")) return MP4;
    if (ends_with_ci(path, ".wav"))                 return WAV;

    /* 4. Whatever the dataset claimed. */
    if (dataset_codec && dataset_codec[0]) {
        if (equals_ci(dataset_codec, "MP3"))
            return MPEG;
        if (equals_ci(dataset_codec, "AAC") || equals_ci(dataset_codec, "AAC+") ||
            equals_ci(dataset_codec, "AACP"))
            return ADTS;
        if (equals_ci(dataset_codec, "OGG") || equals_ci(dataset_codec, "VORBIS") ||
            equals_ci(dataset_codec, "OPUS"))
            return OGG;
        if (equals_ci(dataset_codec, "FLAC"))
            return FLAC;
    }

    return UNKNOWN;
}

StreamFormat format_for_hls(RwrAudioCodec codec)
{
    switch (codec) {
    case RWR_CODEC_ADTS_AAC:   return ADTS;
    case RWR_CODEC_MPEG_AUDIO: return MPEG;
    default:                   return UNKNOWN;
    }
}
