/* Decides which decoder to open a progressive HTTP stream with. Port of the Rust
 * port's format.rs, minus the container path.
 *
 * The Rust ports hand anything that isn't a raw ADTS/MPEG-audio bitstream to
 * symphonia's probe, which covers Ogg, FLAC, MP4 and WAV. This build has only
 * libmad and faad2, so a container is recognized but reported as unsupported
 * rather than opened - see README.md, "What this port does not do". The
 * detection order is unchanged, because it is what makes a wrong Content-Type
 * harmless: the body's own first bytes are the most reliable signal there is. */
#ifndef RWR_FORMAT_H
#define RWR_FORMAT_H

#include <stddef.h>

#include "tsdemux.h"

typedef struct {
    RwrAudioCodec codec;      /* RWR_CODEC_UNKNOWN when nothing playable was identified */
    /* Short name for the status line, e.g. "raw ADTS AAC" or "Ogg (unsupported
     * by this build)". Always a static string. */
    const char   *description;
} StreamFormat;

/* `head` is the first few bytes of the body. `content_type` and `dataset_codec`
 * may be NULL or "". */
StreamFormat format_for_http(const unsigned char *head, size_t head_len,
                             const char *content_type, const char *url,
                             const char *dataset_codec);

/* For an HLS stream, whose codec the PMT or the packed-audio syncword already
 * told us. */
StreamFormat format_for_hls(RwrAudioCodec codec);

#endif
