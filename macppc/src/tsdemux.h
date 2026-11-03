/* Extracts the raw elementary audio stream from one MPEG-TS segment, stripping
 * TS packet headers and PES framing: PAT to find the PMT, PMT to find the audio
 * elementary PID and its codec, then the de-PES'd payload of that PID.
 *
 * Port of the Rust port's ts_demuxer.rs, itself a port of the Haiku build's
 * TsDemuxer. Stateless: PAT/PMT are re-parsed from scratch for every segment.
 * Real encoders repeat them periodically precisely so a demuxer can start from
 * any segment, so there is nothing to carry across. */
#ifndef RWR_TSDEMUX_H
#define RWR_TSDEMUX_H

#include <stddef.h>

#include "buf.h"

typedef enum {
    RWR_CODEC_UNKNOWN = 0,
    RWR_CODEC_ADTS_AAC,   /* stream_type 0x0F */
    RWR_CODEC_MPEG_AUDIO  /* stream_type 0x03 / 0x04 */
} RwrAudioCodec;

/* Appends the extracted elementary stream to `out` and returns the codec the PMT
 * declared. Returns RWR_CODEC_UNKNOWN and appends nothing when there is no
 * parseable audio in the data. */
RwrAudioCodec tsdemux_extract(const unsigned char *data, size_t len, Buf *out);

#endif
