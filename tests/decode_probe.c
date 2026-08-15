/* Decodes one file through the real playback path and prints what came out.
 *
 * This drives src/decoder.c over src/bytequeue.c exactly the way player.c does,
 * so it exercises libmad and faad2 plus this port's own framing and resync -
 * not just the libraries. It is separate from tests/test_core.c because it needs
 * the vendored decoders, which `make check` deliberately does not.
 *
 * The point is that the output is comparable across machines: run it on the
 * host and on a big-endian PowerPC and the numbers should agree. libmad is
 * fixed-point, so its checksum is bit-exact; faad2 is floating point, so compare
 * its RMS rather than its checksum.
 *
 *   decode_probe <file> mp3|aac
 */

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bytequeue.h"
#include "decoder.h"

typedef struct {
    ByteQueue *queue;
    char      *path;
} Feeder;

/* Feeds the file in small pieces on its own thread, which is what the network
 * layer does - and which makes the decoder cross buffer boundaries mid-frame
 * the way a real stream does. */
static void *feed(void *argument)
{
    Feeder *feeder = (Feeder *)argument;
    FILE *file = fopen(feeder->path, "rb");
    unsigned char chunk[1337]; /* deliberately not a round number */
    size_t got;

    if (!file) {
        bytequeue_close(feeder->queue, "could not open the input");
        return NULL;
    }
    while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        if (!bytequeue_push(feeder->queue, chunk, got))
            break;
    }
    fclose(file);
    bytequeue_close(feeder->queue, NULL);
    return NULL;
}

int main(int argc, char **argv)
{
    RwrAudioCodec codec;
    ByteQueue *queue;
    Decoder *decoder;
    Feeder feeder;
    pthread_t thread;
    DecodedFrame frame;
    char error[512];
    unsigned long frames = 0, samples = 0;
    unsigned long checksum = 0;
    /* Same idea at reduced precision. faad2 is floating point, so the last bit
     * of a sample legitimately differs between architectures (ARM64 contracts
     * multiply-add, PowerPC does not) and the 16-bit checksum diverges from one
     * such sample onwards. Quantizing to 9 bits absorbs that while still
     * catching anything that is actually wrong - a byte-order bug moves samples
     * by far more than 1/256. */
    unsigned long coarse = 0;
    /* Sampled from the middle of the stream rather than the start: an encoder's
     * priming samples are silence, and comparing zeros proves nothing. */
    float probe[8];
    int probe_count = 0;
    unsigned long total_index = 0;
    double square_sum = 0.0;
    float peak = 0.0f;
    int rate = 0, channels = 0, status;

    if (argc != 3) {
        fprintf(stderr, "usage: decode_probe <file> mp3|aac\n");
        return 2;
    }
    if (strcmp(argv[2], "mp3") == 0) {
        codec = RWR_CODEC_MPEG_AUDIO;
    } else if (strcmp(argv[2], "aac") == 0) {
        codec = RWR_CODEC_ADTS_AAC;
    } else {
        fprintf(stderr, "unknown codec %s\n", argv[2]);
        return 2;
    }

    queue = bytequeue_new();
    if (!queue)
        return 1;
    feeder.queue = bytequeue_retain(queue);
    feeder.path = argv[1];
    if (pthread_create(&thread, NULL, feed, &feeder) != 0)
        return 1;

    decoder = decoder_new(codec, queue);
    if (!decoder) {
        fprintf(stderr, "decoder_new failed\n");
        return 1;
    }

    error[0] = '\0';
    while ((status = decoder_next(decoder, &frame, error, sizeof(error))) == 1) {
        size_t index, count = frame.frame_count * (size_t)frame.channels;

        if (frames == 0) {
            rate = frame.sample_rate;
            channels = frame.channels;
        }
        for (index = 0; index < count; index++) {
            float value = frame.samples[index];
            /* Quantized to 16 bits so the checksum is stable against the last
             * bit of float noise, and rotated so sample order matters. */
            long quantized = (long)(value * 32767.0f);
            long coarse_q = (long)(value * 256.0f);
            checksum = (checksum << 1 | (checksum >> 31)) & 0xFFFFFFFFul;
            checksum ^= (unsigned long)(quantized & 0xFFFF);
            coarse = (coarse << 1 | (coarse >> 31)) & 0xFFFFFFFFul;
            coarse ^= (unsigned long)(coarse_q & 0x1FF);
            if (total_index >= 80000 && probe_count < 8)
                probe[probe_count++] = value;
            total_index++;
            square_sum += (double)value * (double)value;
            if (fabsf(value) > peak)
                peak = fabsf(value);
        }
        samples += count;
        frames++;
    }

    printf("codec      %s\n", argv[2]);
    printf("result     %s\n", status == 0 ? "clean end of stream" : error);
    printf("frames     %lu\n", frames);
    printf("rate       %d\n", rate);
    printf("channels   %d\n", channels);
    printf("samples    %lu\n", samples);
    printf("checksum   %08lX\n", checksum);
    printf("coarse     %08lX\n", coarse);
    printf("rms        %.6f\n", samples ? sqrt(square_sum / (double)samples) : 0.0);
    printf("peak       %.6f\n", (double)peak);
    printf("mid8      ");
    {
        int i;
        for (i = 0; i < probe_count; i++)
            printf(" %+.6f", (double)probe[i]);
        printf("\n");
    }

    decoder_free(decoder);
    pthread_join(thread, NULL);
    bytequeue_release(feeder.queue);
    bytequeue_release(queue);
    return (frames > 0) ? 0 : 1;
}
