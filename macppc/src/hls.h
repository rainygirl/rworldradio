/* Bridges a live HLS audio stream into the decoder: fetches and refreshes the
 * playlist, downloads new segments, demuxes each one, and writes the resulting
 * elementary stream into a ByteQueue the decoder reads as a plain byte stream.
 *
 * Port of the Rust port's hls.rs, which was the Haiku build's HlsAdapterIO (a
 * BAdapterIO feeding BMediaFile) with the Haiku plumbing removed. The worker
 * loop, the "block until the first segment decoded, then report success or a
 * useful reason" open semantics and the sliding-window bookkeeping are the
 * same. */
#ifndef RWR_HLS_H
#define RWR_HLS_H

#include <stddef.h>

#include "bytequeue.h"
#include "stopflag.h"
#include "tsdemux.h"

typedef struct {
    ByteQueue    *queue;   /* caller owns this reference */
    RwrAudioCodec codec;   /* from the PMT or the packed-audio syncword */
} HlsStream;

/* Starts the worker and blocks until it has written the first segment's audio
 * (success) or determined the stream is unusable (failure). Returns 0 on
 * success, or -1 with a reason written into `error`. */
int hls_open(const char *playlist_url, StopFlag *stop, HlsStream *out,
             char *error, size_t error_size);

#endif
