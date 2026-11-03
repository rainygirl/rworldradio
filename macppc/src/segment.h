/* Turns one downloaded HLS segment into a raw elementary audio stream, whichever
 * of the two segment formats it happens to be. Port of the Rust port's
 * segment.rs.
 *
 * The Haiku build assumed every segment was MPEG-TS. The HLS spec also allows
 * "packed audio" segments - a bare ADTS AAC or MPEG audio bitstream with no TS
 * wrapper - and real stations use them (qingting.fm serves .aac segments this
 * way). Those need no demuxing, just their per-segment ID3 tag stripped. */
#ifndef RWR_SEGMENT_H
#define RWR_SEGMENT_H

#include <stddef.h>

#include "buf.h"
#include "tsdemux.h"

/* Appends the segment's elementary stream to `out` and returns its codec, or
 * RWR_CODEC_UNKNOWN (appending nothing) when there is no recognizable audio.
 * `uri` is used only as the last-resort codec hint. */
RwrAudioCodec segment_elementary_stream(const unsigned char *data, size_t len,
                                        const char *uri, Buf *out);

/* Exposed for the tests. */
int    segment_looks_like_mpeg_ts(const unsigned char *data, size_t len);
size_t segment_id3_prefix_len(const unsigned char *data, size_t len);

#endif
