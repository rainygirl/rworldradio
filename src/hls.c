#include "hls.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "buf.h"
#include "m3u8.h"
#include "net.h"
#include "segment.h"

/* How long hls_open waits for the first decoded segment before giving up.
 * Matches the spirit of Haiku's own HTTPMediaIO (10s for a single HTTP
 * response); HLS involves an extra playlist round-trip first, so this is a bit
 * more generous. */
#define OPEN_TIMEOUT_MS 20000

/* A live sliding-window playlist won't repeat old segment URIs, so the seen-set
 * can be dropped periodically rather than growing forever across a multi-hour
 * listen. */
#define MAX_SEEN_SEGMENTS 500

/* Playlists in the wild occasionally carry a nonsense #EXT-X-TARGETDURATION (or
 * one strtod reads as inf). Clamping keeps a bad value from either hammering the
 * server or wedging the poll loop for hours. */
#define MIN_POLL_SECONDS 0.5
#define MAX_POLL_SECONDS 30.0

/* Shared between hls_open and the worker: the worker publishes the codec it
 * found and the last thing that went wrong, so a failed open can explain
 * itself. */
typedef struct {
    pthread_mutex_t lock;
    RwrAudioCodec   codec;
    char            last_issue[512];
    int             refcount;
} WorkerState;

typedef struct {
    char        *playlist_url;
    ByteQueue   *queue;
    StopFlag    *stop;
    WorkerState *state;
} Worker;

/* A plain array of strdup'd URIs. The set never exceeds MAX_SEEN_SEGMENTS
 * entries, and a live playlist lists a handful of segments per round, so a
 * linear scan is cheaper here than a hash table's bookkeeping. */
typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} SeenSet;

static WorkerState *state_new(void)
{
    WorkerState *state = (WorkerState *)calloc(1, sizeof(*state));

    if (!state)
        return NULL;
    pthread_mutex_init(&state->lock, NULL);
    state->codec = RWR_CODEC_UNKNOWN;
    snprintf(state->last_issue, sizeof(state->last_issue),
             "no segments were listed in the media playlist");
    state->refcount = 1;
    return state;
}

static WorkerState *state_retain(WorkerState *state)
{
    pthread_mutex_lock(&state->lock);
    state->refcount++;
    pthread_mutex_unlock(&state->lock);
    return state;
}

static void state_release(WorkerState *state)
{
    int dead;

    pthread_mutex_lock(&state->lock);
    dead = (--state->refcount == 0);
    pthread_mutex_unlock(&state->lock);
    if (!dead)
        return;
    pthread_mutex_destroy(&state->lock);
    free(state);
}

static void set_issue(WorkerState *state, const char *format, ...)
{
    va_list args;

    pthread_mutex_lock(&state->lock);
    va_start(args, format);
    vsnprintf(state->last_issue, sizeof(state->last_issue), format, args);
    va_end(args);
    pthread_mutex_unlock(&state->lock);
}

static void get_issue(WorkerState *state, char *out, size_t size)
{
    pthread_mutex_lock(&state->lock);
    snprintf(out, size, "%s", state->last_issue);
    pthread_mutex_unlock(&state->lock);
}

static void seen_init(SeenSet *seen)
{
    seen->items = NULL;
    seen->count = 0;
    seen->capacity = 0;
}

static void seen_clear(SeenSet *seen)
{
    size_t index;
    for (index = 0; index < seen->count; index++)
        free(seen->items[index]);
    seen->count = 0;
}

static void seen_free(SeenSet *seen)
{
    seen_clear(seen);
    free(seen->items);
    seen_init(seen);
}

/* Returns 1 if `uri` was newly inserted, 0 if it was already there. */
static int seen_insert(SeenSet *seen, const char *uri)
{
    size_t index;
    char *copy;

    for (index = 0; index < seen->count; index++) {
        if (strcmp(seen->items[index], uri) == 0)
            return 0;
    }
    if (seen->count == seen->capacity) {
        size_t capacity = seen->capacity ? seen->capacity * 2 : 32;
        char **grown = (char **)realloc(seen->items, capacity * sizeof(*grown));
        if (!grown)
            return 0; /* treat as already-seen rather than refetching forever */
        seen->items = grown;
        seen->capacity = capacity;
    }
    copy = strdup(uri);
    if (!copy)
        return 0;
    seen->items[seen->count++] = copy;
    return 1;
}

/* Sleeps in short slices so a stop request doesn't have to wait out a full
 * target-duration poll interval. */
static void sleep_interruptibly(double seconds, StopFlag *stop, ByteQueue *queue)
{
    int slices = (int)(seconds * 10.0);
    int index;

    for (index = 0; index < slices; index++) {
        if (stopflag_is_set(stop) || bytequeue_is_closed(queue))
            return;
        usleep(100000);
    }
}

static int stopped(const Worker *worker)
{
    return stopflag_is_set(worker->stop) || bytequeue_is_closed(worker->queue);
}

static void *run_worker(void *argument)
{
    Worker *worker = (Worker *)argument;
    WorkerState *state = worker->state;
    ByteQueue *queue = worker->queue;
    char *media_playlist_url = NULL;
    char error[512];
    Buf body;
    SeenSet seen;
    int wrote_anything = 0;

    buf_init(&body);
    seen_init(&seen);

    if (net_get_text(worker->playlist_url, &body, error, sizeof(error)) != 0) {
        set_issue(state, "could not fetch playlist: %s", error);
        get_issue(state, error, sizeof(error));
        bytequeue_close(queue, error);
        goto done;
    }

    media_playlist_url = strdup(worker->playlist_url);
    if (!media_playlist_url) {
        bytequeue_close(queue, "out of memory");
        goto done;
    }

    /* A master playlist: pick the highest-bandwidth variant. Internet radio
     * bitrates are all small enough that "best available" is not a real
     * bandwidth concern for anyone able to stream at all. */
    if (m3u8_is_master_playlist(body.data)) {
        M3u8Variant *variants = NULL;
        size_t count = 0, index, best = 0;

        m3u8_parse_master(body.data, worker->playlist_url, &variants, &count);
        if (count == 0) {
            m3u8_free_variants(variants, count);
            set_issue(state, "master playlist has no variants");
            bytequeue_close(queue, "master playlist has no variants");
            goto done;
        }
        for (index = 1; index < count; index++) {
            if (variants[index].bandwidth > variants[best].bandwidth)
                best = index;
        }
        free(media_playlist_url);
        media_playlist_url = strdup(variants[best].uri);
        m3u8_free_variants(variants, count);
        if (!media_playlist_url) {
            bytequeue_close(queue, "out of memory");
            goto done;
        }
    }

    while (!stopped(worker)) {
        M3u8MediaPlaylist playlist;
        size_t index;
        int wrote_this_round = 0;

        buf_clear(&body);
        if (net_get_text(media_playlist_url, &body, error, sizeof(error)) != 0) {
            set_issue(state, "could not fetch media playlist: %s", error);
            if (!wrote_anything) {
                /* Nothing has played yet, so this is a hard failure - don't
                 * leave hls_open waiting out its whole timeout. */
                get_issue(state, error, sizeof(error));
                bytequeue_close(queue, error);
                goto done;
            }
            sleep_interruptibly(2.0, worker->stop, queue);
            continue;
        }

        if (m3u8_parse_media(body.data, media_playlist_url, &playlist) != 0) {
            sleep_interruptibly(2.0, worker->stop, queue);
            continue;
        }

        for (index = 0; index < playlist.segment_count; index++) {
            const char *uri = playlist.segments[index].uri;
            Buf segment_data, elementary;
            RwrAudioCodec codec;

            if (stopped(worker))
                break;
            if (!seen_insert(&seen, uri))
                continue;

            buf_init(&segment_data);
            if (net_get_bytes(uri, &segment_data, error, sizeof(error)) != 0) {
                set_issue(state, "segment fetch failed: %s", error);
                buf_free(&segment_data);
                continue;
            }

            buf_init(&elementary);
            codec = segment_elementary_stream((const unsigned char *)segment_data.data,
                                              segment_data.len, uri, &elementary);
            buf_free(&segment_data);

            if (elementary.len == 0) {
                set_issue(state, "segment produced no audio elementary stream "
                                 "(unsupported codec/container?)");
                buf_free(&elementary);
                continue;
            }
            if (codec != RWR_CODEC_UNKNOWN) {
                pthread_mutex_lock(&state->lock);
                state->codec = codec;
                pthread_mutex_unlock(&state->lock);
            }

            if (!bytequeue_push(queue, elementary.data, elementary.len)) {
                buf_free(&elementary);
                m3u8_free_media(&playlist);
                goto done; /* consumer went away */
            }
            buf_free(&elementary);
            wrote_this_round = 1;
            wrote_anything = 1;
        }

        if (playlist.is_end_list || stopped(worker)) {
            m3u8_free_media(&playlist);
            break;
        }

        if (!wrote_this_round) {
            /* Nothing new this round - wait roughly a target-duration before
             * asking again, matching standard HLS live polling. */
            double poll = playlist.target_duration;
            if (!(poll > 0.0) && playlist.segment_count > 0)
                poll = playlist.segments[playlist.segment_count - 1].duration;
            if (!(poll > 0.0))
                poll = 3.0;
            if (poll < MIN_POLL_SECONDS)
                poll = MIN_POLL_SECONDS;
            if (!(poll < MAX_POLL_SECONDS))
                poll = MAX_POLL_SECONDS;
            sleep_interruptibly(poll, worker->stop, queue);
        }

        m3u8_free_media(&playlist);

        if (seen.count > MAX_SEEN_SEGMENTS)
            seen_clear(&seen);
    }

    if (wrote_anything) {
        bytequeue_close(queue, NULL);
    } else {
        get_issue(state, error, sizeof(error));
        bytequeue_close(queue, error);
    }

done:
    free(media_playlist_url);
    buf_free(&body);
    seen_free(&seen);
    free(worker->playlist_url);
    bytequeue_release(worker->queue);
    stopflag_release(worker->stop);
    state_release(worker->state);
    free(worker);
    return NULL;
}

int hls_open(const char *playlist_url, StopFlag *stop, HlsStream *out,
             char *error, size_t error_size)
{
    Worker *worker;
    WorkerState *state;
    ByteQueue *queue;
    pthread_t thread;
    pthread_attr_t attributes;

    memset(out, 0, sizeof(*out));

    queue = bytequeue_new();
    state = state_new();
    worker = (Worker *)calloc(1, sizeof(*worker));
    if (!queue || !state || !worker) {
        snprintf(error, error_size, "out of memory");
        goto setup_failed;
    }

    worker->playlist_url = strdup(playlist_url);
    if (!worker->playlist_url) {
        snprintf(error, error_size, "out of memory");
        goto setup_failed;
    }
    worker->queue = bytequeue_retain(queue);
    worker->stop = stopflag_retain(stop);
    worker->state = state_retain(state);

    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attributes, 256 * 1024);
    if (pthread_create(&thread, &attributes, run_worker, worker) != 0) {
        pthread_attr_destroy(&attributes);
        snprintf(error, error_size, "could not start the HLS worker");
        bytequeue_release(worker->queue);
        stopflag_release(worker->stop);
        state_release(worker->state);
        free(worker->playlist_url);
        goto setup_failed;
    }
    pthread_attr_destroy(&attributes);

    if (bytequeue_wait_for_data(queue, OPEN_TIMEOUT_MS)) {
        pthread_mutex_lock(&state->lock);
        out->codec = state->codec;
        pthread_mutex_unlock(&state->lock);
        out->queue = queue;
        state_release(state);
        return 0;
    }

    /* No data within the timeout. Either the worker gave up (the queue is closed
     * and last_issue says why) or it is still spinning on a playlist that never
     * produces anything - close it either way so the worker exits. */
    get_issue(state, error, error_size);
    bytequeue_close(queue, NULL);
    stopflag_set(stop);
    bytequeue_release(queue);
    state_release(state);
    return -1;

setup_failed:
    free(worker);
    if (state)
        state_release(state);
    if (queue)
        bytequeue_release(queue);
    return -1;
}
