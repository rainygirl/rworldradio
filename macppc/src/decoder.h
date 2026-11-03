/* Turns the byte stream in a ByteQueue into interleaved float PCM.
 *
 * The Rust ports hand the stream to symphonia, which picks a reader and a
 * decoder. Here there are exactly two decoders - libmad for MPEG audio and
 * faad2 for AAC - and format.c/hls.c have already said which one to use, so
 * this is a thin dispatch rather than a probe.
 *
 * Two pieces of the Rust port have no counterpart here on purpose:
 *
 *  - adts.rs, the ADTS normalizer, existed only because symphonia 0.5's
 *    AdtsReader syncs on the exact pattern 0xFFF1 and would block forever on
 *    MPEG-2 or CRC-protected ADTS. faad2 syncs on all three variants itself and
 *    skips the CRC, so rewriting the headers would be pure work.
 *  - the linear resampler in audio.rs is gone: the CoreAudio output unit is
 *    opened at the stream's own sample rate and Apple's converter, which is
 *    AltiVec-optimized, does the rate change. See audio_ca.c. */
#ifndef RWR_DECODER_H
#define RWR_DECODER_H

#include <stddef.h>

#include "bytequeue.h"
#include "tsdemux.h"

typedef struct Decoder Decoder;

typedef struct {
    const float *samples;     /* interleaved, valid until the next decoder_next */
    size_t       frame_count;
    int          channels;
    int          sample_rate;
} DecodedFrame;

/* `codec` must be RWR_CODEC_ADTS_AAC or RWR_CODEC_MPEG_AUDIO. The decoder holds
 * its own reference to `queue`. Returns NULL on failure. */
Decoder *decoder_new(RwrAudioCodec codec, ByteQueue *queue);
void     decoder_free(Decoder *decoder);

/* Decodes the next frame.
 *   1  `out` holds PCM
 *   0  clean end of stream
 *  -1  fatal error, described in `error`
 *
 * Recoverable damage - a stream joined mid-frame, a corrupt packet - is handled
 * internally by resynchronizing, the way the Rust port skips symphonia's
 * DecodeError. Only a decoder that never produced a single frame reports -1. */
int decoder_next(Decoder *decoder, DecodedFrame *out, char *error, size_t error_size);

#endif
