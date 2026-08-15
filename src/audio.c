#include "audio.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* Roughly 400ms of stereo audio at 48kHz. Big enough to ride out scheduler
 * jitter and an HLS segment boundary, small enough that stopping is audibly
 * immediate. */
#define RING_FRAMES  (48000 * 2 / 5)
#define MAX_CHANNELS 8

struct AudioSink {
    pthread_mutex_t lock;
    pthread_cond_t  space_available;

    float *ring;
    size_t capacity;   /* in samples, i.e. frames * channels */
    size_t head;
    size_t count;

    /* Set by mute() - the callback outputs silence at once instead of draining
     * whatever is still buffered. */
    volatile int muted;
    /* Peak amplitude of the most recent callback. A float written by the render
     * thread and read by the UI thread; an aligned 32-bit store is atomic on
     * both targets, and a stale value only ever means one late meter frame. */
    volatile float level;

    int refcount;
};

AudioSink *audiosink_new(int channels)
{
    AudioSink *sink = (AudioSink *)calloc(1, sizeof(*sink));

    if (!sink)
        return NULL;
    if (channels < 1)
        channels = 1;
    if (channels > MAX_CHANNELS)
        channels = MAX_CHANNELS;

    sink->capacity = (size_t)RING_FRAMES * (size_t)channels;
    sink->ring = (float *)malloc(sink->capacity * sizeof(float));
    if (!sink->ring) {
        free(sink);
        return NULL;
    }
    pthread_mutex_init(&sink->lock, NULL);
    pthread_cond_init(&sink->space_available, NULL);
    sink->refcount = 1;
    return sink;
}

AudioSink *audiosink_retain(AudioSink *sink)
{
    if (!sink)
        return NULL;
    pthread_mutex_lock(&sink->lock);
    sink->refcount++;
    pthread_mutex_unlock(&sink->lock);
    return sink;
}

void audiosink_release(AudioSink *sink)
{
    int dead;

    if (!sink)
        return;
    pthread_mutex_lock(&sink->lock);
    dead = (--sink->refcount == 0);
    pthread_mutex_unlock(&sink->lock);
    if (!dead)
        return;

    pthread_cond_destroy(&sink->space_available);
    pthread_mutex_destroy(&sink->lock);
    free(sink->ring);
    free(sink);
}

int audiosink_push(AudioSink *sink, const float *samples, size_t count)
{
    pthread_mutex_lock(&sink->lock);

    while (count > 0) {
        size_t space, tail, first, second, taken;

        while (sink->count == sink->capacity) {
            struct timeval now;
            struct timespec deadline;

            if (sink->muted) {
                pthread_mutex_unlock(&sink->lock);
                return 0;
            }
            /* A bounded wait rather than a plain one: mute() broadcasts, but a
             * device that stopped calling back without an error would otherwise
             * park the decoder here forever. */
            gettimeofday(&now, NULL);
            deadline.tv_sec = now.tv_sec;
            deadline.tv_nsec = (long)now.tv_usec * 1000 + 100000000L;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec += 1;
                deadline.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&sink->space_available, &sink->lock, &deadline);
        }
        if (sink->muted) {
            pthread_mutex_unlock(&sink->lock);
            return 0;
        }

        space = sink->capacity - sink->count;
        taken = count < space ? count : space;
        tail = (sink->head + sink->count) % sink->capacity;
        first = sink->capacity - tail;
        if (first > taken)
            first = taken;
        memcpy(sink->ring + tail, samples, first * sizeof(float));
        second = taken - first;
        if (second)
            memcpy(sink->ring, samples + first, second * sizeof(float));

        sink->count += taken;
        samples += taken;
        count -= taken;
    }

    pthread_mutex_unlock(&sink->lock);
    return 1;
}

void audiosink_fill(AudioSink *sink, float *out, size_t count)
{
    size_t produced, first, second, index;
    float peak = 0.0f;

    if (sink->muted) {
        memset(out, 0, count * sizeof(float));
        sink->level = 0.0f;
        return;
    }

    pthread_mutex_lock(&sink->lock);
    produced = count < sink->count ? count : sink->count;
    first = sink->capacity - sink->head;
    if (first > produced)
        first = produced;
    memcpy(out, sink->ring + sink->head, first * sizeof(float));
    second = produced - first;
    if (second)
        memcpy(out + first, sink->ring, second * sizeof(float));
    sink->head = (sink->head + produced) % sink->capacity;
    sink->count -= produced;
    pthread_cond_broadcast(&sink->space_available);
    pthread_mutex_unlock(&sink->lock);

    if (produced < count)
        memset(out + produced, 0, (count - produced) * sizeof(float));

    for (index = 0; index < produced; index++) {
        float magnitude = out[index] < 0.0f ? -out[index] : out[index];
        if (magnitude > peak)
            peak = magnitude;
    }
    sink->level = peak > 1.0f ? 1.0f : peak;
}

void audiosink_mute(AudioSink *sink)
{
    if (!sink)
        return;
    sink->muted = 1;
    pthread_mutex_lock(&sink->lock);
    sink->count = 0;
    sink->head = 0;
    pthread_cond_broadcast(&sink->space_available);
    pthread_mutex_unlock(&sink->lock);
    sink->level = 0.0f;
}

float audiosink_level(AudioSink *sink)
{
    return sink ? sink->level : 0.0f;
}
