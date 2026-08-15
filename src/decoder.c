#include "decoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mad.h>
#include <neaacdec.h>

#include "buf.h"

/* How much undecoded input to keep in front of the decoder. An MPEG frame is at
 * most 1441 bytes and an ADTS frame at most 8191, so this always holds several
 * whole frames even at the highest bitrates. */
#define INPUT_CHUNK    (16 * 1024)
#define INPUT_HIGHWATER (64 * 1024)

/* A run of undecodable input this long means the stream is not what we were told
 * it was, rather than a few bytes of damage to skip past. */
#define MAX_RESYNC_BYTES (512 * 1024)

struct Decoder {
    RwrAudioCodec codec;
    ByteQueue    *queue;
    Buf           input;         /* undecoded bytes pulled from the queue */
    int           eof;           /* queue reached a clean end */
    int           eof_flushed;   /* the post-EOF remainder was handed to libmad once */
    char          queue_error[256];
    int           had_queue_error;
    int           produced_any;  /* at least one frame came out */
    size_t        skipped;       /* bytes dropped since the last good frame */

    float        *pcm;           /* interleaved output, grown as needed */
    size_t        pcm_capacity;  /* in floats */

    /* libmad */
    struct mad_stream mad_stream;
    struct mad_frame  mad_frame;
    struct mad_synth  mad_synth;
    int               mad_ready;

    /* faad2 */
    NeAACDecHandle faad;
    int            faad_initialized;
};

/* --------------------------------------------------------------- plumbing - */

/* Pulls more bytes from the queue into `input`. Returns 1 if anything was added,
 * 0 at end of stream, -1 if the queue reported an error. */
static int refill(Decoder *decoder)
{
    char chunk[INPUT_CHUNK];
    long got;

    if (decoder->eof || decoder->had_queue_error)
        return decoder->had_queue_error ? -1 : 0;

    got = bytequeue_read(decoder->queue, chunk, sizeof(chunk),
                         decoder->queue_error, sizeof(decoder->queue_error));
    if (got < 0) {
        decoder->had_queue_error = 1;
        return -1;
    }
    if (got == 0) {
        decoder->eof = 1;
        return 0;
    }
    if (buf_append(&decoder->input, chunk, (size_t)got) != 0)
        return -1;
    return 1;
}

/* Makes sure at least `wanted` bytes are buffered, or the stream ended. */
static int fill_to(Decoder *decoder, size_t wanted)
{
    while (decoder->input.len < wanted) {
        int result = refill(decoder);
        if (result <= 0)
            return result;
    }
    return 1;
}

static float *ensure_pcm(Decoder *decoder, size_t floats)
{
    if (floats > decoder->pcm_capacity) {
        float *grown = (float *)realloc(decoder->pcm, floats * sizeof(float));
        if (!grown)
            return NULL;
        decoder->pcm = grown;
        decoder->pcm_capacity = floats;
    }
    return decoder->pcm;
}

/* The error to report when nothing ever decoded. A queue error (the network
 * failing) is more useful to the user than "no frames", so it wins. */
static void report_failure(Decoder *decoder, const char *fallback,
                           char *error, size_t error_size)
{
    if (decoder->queue_error[0])
        snprintf(error, error_size, "%s", decoder->queue_error);
    else
        snprintf(error, error_size, "%s", fallback);
}

/* ----------------------------------------------------------------- libmad - */

/* mad_fixed_t is a 28-bit fraction; this is the reciprocal of its scale, applied
 * as a multiply because a G4 divide is many times the cost. */
#define MAD_TO_FLOAT (1.0f / (float)(1L << MAD_F_FRACBITS))

static int mad_next(Decoder *decoder, DecodedFrame *out, char *error, size_t error_size)
{
    for (;;) {
        int result;

        /* mad_stream_buffer() must be re-pointed at the buffer after every
         * refill, and next_frame tells us how much of the old buffer libmad had
         * not consumed yet. */
        if (!decoder->mad_stream.buffer ||
            decoder->mad_stream.error == MAD_ERROR_BUFLEN) {
            size_t leftover = 0;

            if (decoder->mad_stream.next_frame) {
                leftover = (size_t)(decoder->mad_stream.bufend -
                                    decoder->mad_stream.next_frame);
                buf_drain_front(&decoder->input, decoder->input.len - leftover);
            }

            result = refill(decoder);
            if (result < 0) {
                if (decoder->produced_any)
                    return 0;
                report_failure(decoder, "stream read failed", error, error_size);
                return -1;
            }
            if (result == 0) {
                /* End of stream. Hand libmad what is left exactly once - it may
                 * still hold one complete frame - and stop on the next pass
                 * rather than re-feeding the same partial frame forever. */
                if (decoder->input.len == 0 || decoder->eof_flushed) {
                    if (decoder->produced_any)
                        return 0;
                    report_failure(decoder, "the stream ended before any audio decoded",
                                   error, error_size);
                    return -1;
                }
                decoder->eof_flushed = 1;
            }
            mad_stream_buffer(&decoder->mad_stream,
                              (const unsigned char *)decoder->input.data,
                              decoder->input.len);
            decoder->mad_stream.error = MAD_ERROR_NONE;
        }

        if (mad_frame_decode(&decoder->mad_frame, &decoder->mad_stream) != 0) {
            if (decoder->mad_stream.error == MAD_ERROR_BUFLEN)
                continue; /* need more input */
            if (MAD_RECOVERABLE(decoder->mad_stream.error)) {
                /* A stream joined mid-frame produces a run of these before it
                 * syncs up; libmad has already advanced past the bad byte. */
                decoder->skipped += 1;
                if (!decoder->produced_any && decoder->skipped > MAX_RESYNC_BYTES) {
                    snprintf(error, error_size,
                             "no MPEG audio frames found - the stream is not MP3");
                    return -1;
                }
                continue;
            }
            snprintf(error, error_size, "MPEG audio decode failed: %s",
                     mad_stream_errorstr(&decoder->mad_stream));
            return decoder->produced_any ? 0 : -1;
        }

        mad_synth_frame(&decoder->mad_synth, &decoder->mad_frame);
        {
            struct mad_pcm *pcm = &decoder->mad_synth.pcm;
            unsigned int channels = pcm->channels;
            unsigned int frames = pcm->length;
            unsigned int index, channel;
            float *samples = ensure_pcm(decoder, (size_t)frames * channels);

            if (!samples) {
                snprintf(error, error_size, "out of memory");
                return -1;
            }
            for (index = 0; index < frames; index++) {
                for (channel = 0; channel < channels; channel++)
                    samples[index * channels + channel] =
                        (float)pcm->samples[channel][index] * MAD_TO_FLOAT;
            }

            out->samples = samples;
            out->frame_count = frames;
            out->channels = (int)channels;
            out->sample_rate = (int)pcm->samplerate;
        }
        decoder->produced_any = 1;
        decoder->skipped = 0;
        return 1;
    }
}

/* ------------------------------------------------------------------ faad2 - */

/* Walks to the next ADTS syncword, dropping everything before it. Returns 0 if
 * the buffer ran out first. faad2 can resynchronize on its own, but doing it
 * here keeps the "how much junk have we skipped" accounting in one place and
 * avoids handing it a buffer that starts mid-payload. */
static int faad_resync(Decoder *decoder)
{
    for (;;) {
        const unsigned char *data = (const unsigned char *)decoder->input.data;
        size_t len = decoder->input.len, index;

        for (index = 0; index + 1 < len; index++) {
            if (data[index] == 0xFF && (data[index + 1] & 0xF6) == 0xF0) {
                if (index > 0) {
                    decoder->skipped += index;
                    buf_drain_front(&decoder->input, index);
                }
                return 1;
            }
        }
        /* Keep the last byte: a syncword may straddle the buffer boundary. */
        if (len > 1) {
            decoder->skipped += len - 1;
            buf_drain_front(&decoder->input, len - 1);
        }
        if (refill(decoder) <= 0)
            return 0;
    }
}

static int faad_next(Decoder *decoder, DecodedFrame *out, char *error, size_t error_size)
{
    for (;;) {
        NeAACDecFrameInfo info;
        void *samples;

        if (!decoder->produced_any && decoder->skipped > MAX_RESYNC_BYTES) {
            snprintf(error, error_size,
                     "no AAC frames found - the stream is not AAC");
            return -1;
        }

        /* An ADTS frame is at most 8191 bytes; keeping well over one frame
         * buffered means faad2 never has to be handed a partial one. */
        if (fill_to(decoder, INPUT_HIGHWATER) < 0 && decoder->input.len == 0) {
            if (decoder->produced_any)
                return 0;
            report_failure(decoder, "stream read failed", error, error_size);
            return -1;
        }
        if (decoder->input.len == 0) {
            if (decoder->produced_any)
                return 0;
            report_failure(decoder, "the stream ended before any audio decoded",
                           error, error_size);
            return -1;
        }

        if (!faad_resync(decoder)) {
            if (decoder->produced_any)
                return 0;
            report_failure(decoder, "no AAC syncword in the stream", error, error_size);
            return -1;
        }

        if (!decoder->faad_initialized) {
            unsigned long sample_rate = 0;
            unsigned char channels = 0;
            long consumed = NeAACDecInit(decoder->faad,
                                         (unsigned char *)decoder->input.data,
                                         (unsigned long)decoder->input.len,
                                         &sample_rate, &channels);
            if (consumed < 0) {
                /* Not a usable header here - step past this syncword and look
                 * for the next one. */
                decoder->skipped += 1;
                buf_drain_front(&decoder->input, 1);
                continue;
            }
            buf_drain_front(&decoder->input, (size_t)consumed);
            decoder->faad_initialized = 1;
            continue;
        }

        samples = NeAACDecDecode(decoder->faad, &info,
                                 (unsigned char *)decoder->input.data,
                                 (unsigned long)decoder->input.len);

        if (info.bytesconsumed > 0)
            buf_drain_front(&decoder->input, (size_t)info.bytesconsumed);

        if (info.error != 0) {
            /* Recoverable: skip a byte and resynchronize, matching how the Rust
             * port skips symphonia's DecodeError rather than dropping the
             * station. */
            if (info.bytesconsumed == 0 && decoder->input.len > 0) {
                decoder->skipped += 1;
                buf_drain_front(&decoder->input, 1);
            }
            if (!decoder->produced_any && decoder->skipped > MAX_RESYNC_BYTES) {
                snprintf(error, error_size, "AAC decode failed: %s",
                         NeAACDecGetErrorMessage(info.error));
                return -1;
            }
            continue;
        }

        if (info.bytesconsumed == 0) {
            /* A frame that decoded but consumed nothing would spin here
             * forever; step past a byte and resynchronize. */
            if (decoder->input.len == 0)
                continue;
            decoder->skipped += 1;
            buf_drain_front(&decoder->input, 1);
            continue;
        }
        if (!samples || info.samples == 0)
            continue; /* a header-only or silent frame */

        /* faad2 is configured for FAAD_FMT_FLOAT output, so its buffer is
         * already the interleaved float the audio sink wants - no conversion
         * pass, which matters more on a G4 than the extra memory would. */
        out->samples = (const float *)samples;
        out->channels = info.channels ? (int)info.channels : 2;
        out->frame_count = info.samples / (size_t)out->channels;
        /* After decode this is the output rate, which for HE-AAC is twice the
         * core rate NeAACDecInit reported. */
        out->sample_rate = (int)info.samplerate;

        decoder->produced_any = 1;
        decoder->skipped = 0;
        return 1;
    }
}

/* -------------------------------------------------------------- interface - */

Decoder *decoder_new(RwrAudioCodec codec, ByteQueue *queue)
{
    Decoder *decoder;

    if (codec != RWR_CODEC_ADTS_AAC && codec != RWR_CODEC_MPEG_AUDIO)
        return NULL;

    decoder = (Decoder *)calloc(1, sizeof(*decoder));
    if (!decoder)
        return NULL;
    decoder->codec = codec;
    decoder->queue = bytequeue_retain(queue);
    buf_init(&decoder->input);

    if (codec == RWR_CODEC_MPEG_AUDIO) {
        mad_stream_init(&decoder->mad_stream);
        mad_frame_init(&decoder->mad_frame);
        mad_synth_init(&decoder->mad_synth);
        decoder->mad_ready = 1;
    } else {
        NeAACDecConfigurationPtr config;

        decoder->faad = NeAACDecOpen();
        if (!decoder->faad) {
            bytequeue_release(decoder->queue);
            free(decoder);
            return NULL;
        }
        config = NeAACDecGetCurrentConfiguration(decoder->faad);
        config->outputFormat = FAAD_FMT_FLOAT;
        config->downMatrix = 0;
        /* Leave defaultObjectType and useOldADTSFormat alone: the defaults
         * accept both MPEG-2 and MPEG-4 ADTS, which is the whole reason this
         * port does not need the Rust port's ADTS normalizer. */
        NeAACDecSetConfiguration(decoder->faad, config);
    }
    return decoder;
}

void decoder_free(Decoder *decoder)
{
    if (!decoder)
        return;
    if (decoder->mad_ready) {
        mad_synth_finish(&decoder->mad_synth);
        mad_frame_finish(&decoder->mad_frame);
        mad_stream_finish(&decoder->mad_stream);
    }
    if (decoder->faad)
        NeAACDecClose(decoder->faad);
    buf_free(&decoder->input);
    free(decoder->pcm);
    bytequeue_release(decoder->queue);
    free(decoder);
}

int decoder_next(Decoder *decoder, DecodedFrame *out, char *error, size_t error_size)
{
    memset(out, 0, sizeof(*out));
    if (decoder->codec == RWR_CODEC_MPEG_AUDIO)
        return mad_next(decoder, out, error, error_size);
    return faad_next(decoder, out, error, error_size);
}
