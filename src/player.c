#include "player.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "audio.h"
#include "buf.h"
#include "bytequeue.h"
#include "decoder.h"
#include "format.h"
#include "hls.h"
#include "m3u8.h"
#include "net.h"
#include "playlist.h"
#include "stopflag.h"

/* How long to wait for enough of a stream's first bytes to identify its codec.
 * Only a few bytes are needed, so this is a stall guard, not a buffering delay. */
#define HEAD_PEEK_TIMEOUT_MS 15000

/* Enough leading bytes to recognize a playlist marker ("#EXTM3U", "[playlist]")
 * as well as an audio frame header. */
#define HEAD_PEEK_BYTES 16

/* How many playlist hops to follow. Radio-browser entries routinely point at a
 * .m3u/.pls that points at the stream, and a CDN can redirect to an HLS
 * playlist; more than two hops means something is looping. */
#define MAX_PLAYLIST_HOPS 2

#define EVENT_CAPACITY 16

/* A snapshot of the dataset record, because the session outlives the caller's
 * pointer into the mmap in principle and copying three short strings is
 * cheaper than reasoning about that. */
typedef struct {
    char name[256];
    char url[2048];
    char codec[64];
    int  needs_resolve;
} SessionStation;

typedef struct {
    StopFlag  *stop;
    ByteQueue *queue;   /* set once the byte source is open */
    AudioSink *sink;    /* set once the first frame has been decoded */
} ActiveSession;

struct Player {
    pthread_mutex_t lock;

    /* Refcounted because a session thread is detached and may still be
     * unwinding - and still calling emit_if_current, which takes this lock -
     * after the UI has released its own reference. */
    int refcount;

    unsigned long generation;
    int           has_active;
    ActiveSession active;

    PlayerEvent events[EVENT_CAPACITY];
    int         event_head;
    int         event_count;

    void (*wake)(void *context);
    void *wake_context;
};

typedef struct {
    Player        *player;   /* a retained reference, released when the session ends */
    SessionStation station;
    unsigned long  generation;
    StopFlag      *stop;
} SessionArgs;

static void player_release(Player *player);

/* ----------------------------------------------------------------- events - */

static void emit(Player *player, PlayerEventKind kind, const char *station,
                 const char *detail)
{
    void (*wake)(void *) = NULL;
    void *context = NULL;

    pthread_mutex_lock(&player->lock);
    if (player->event_count < EVENT_CAPACITY) {
        PlayerEvent *event =
            &player->events[(player->event_head + player->event_count) % EVENT_CAPACITY];
        event->kind = kind;
        snprintf(event->station, sizeof(event->station), "%s", station ? station : "");
        snprintf(event->detail, sizeof(event->detail), "%s", detail ? detail : "");
        player->event_count++;
    }
    wake = player->wake;
    context = player->wake_context;
    pthread_mutex_unlock(&player->lock);

    if (wake)
        wake(context);
}

static int is_current(Player *player, unsigned long generation)
{
    int current;

    pthread_mutex_lock(&player->lock);
    current = (player->generation == generation);
    pthread_mutex_unlock(&player->lock);
    return current;
}

/* Reports `event` only if this session hasn't already been superseded - a stale
 * session's error must not overwrite the new station's status. */
static void emit_if_current(Player *player, unsigned long generation,
                            PlayerEventKind kind, const char *station,
                            const char *detail)
{
    if (is_current(player, generation))
        emit(player, kind, station, detail);
}

int player_poll_event(Player *player, PlayerEvent *out)
{
    int have = 0;

    pthread_mutex_lock(&player->lock);
    if (player->event_count > 0) {
        *out = player->events[player->event_head];
        player->event_head = (player->event_head + 1) % EVENT_CAPACITY;
        player->event_count--;
        have = 1;
    }
    pthread_mutex_unlock(&player->lock);
    return have;
}

/* -------------------------------------------------------- opening a stream - */

typedef struct {
    ByteQueue   *queue;
    StreamFormat format;
} ByteSource;

static int open_stream_url(const char *url, const char *dataset_codec, StopFlag *stop,
                           int hops, ByteSource *out, char *error, size_t error_size);

/* Fetches a playlist and opens whatever it points at. */
static int follow_playlist(const char *url, const char *dataset_codec, StopFlag *stop,
                           int hops, ByteSource *out, char *error, size_t error_size)
{
    Buf text;
    char target[2048];
    int result;

    buf_init(&text);
    if (net_get_text(url, &text, error, error_size) != 0) {
        char detail[512];
        snprintf(detail, sizeof(detail), "could not fetch playlist: %s", error);
        snprintf(error, error_size, "%s", detail);
        buf_free(&text);
        return -1;
    }

    /* An HLS playlist (master or media) goes to the HLS pipeline; a legacy
     * .m3u/.pls is just a pointer to a normal stream. */
    if (m3u8_is_master_playlist(text.data) || strstr(text.data, "#EXT-X-")) {
        HlsStream stream;
        buf_free(&text);
        if (hls_open(url, stop, &stream, error, error_size) != 0) {
            char detail[512];
            snprintf(detail, sizeof(detail), "could not open HLS stream: %s", error);
            snprintf(error, error_size, "%s", detail);
            return -1;
        }
        out->queue = stream.queue;
        out->format = format_for_hls(stream.codec);
        return 0;
    }

    result = playlist_first_stream_url(text.data, target, sizeof(target));
    buf_free(&text);
    if (!result) {
        snprintf(error, error_size, "playlist contained no stream URL");
        return -1;
    }
    return open_stream_url(target, dataset_codec, stop, hops + 1, out, error, error_size);
}

/* Opens one URL, following playlist indirection up to MAX_PLAYLIST_HOPS. */
static int open_stream_url(const char *url, const char *dataset_codec, StopFlag *stop,
                           int hops, ByteSource *out, char *error, size_t error_size)
{
    NetStream stream;
    unsigned char head[HEAD_PEEK_BYTES];
    size_t head_len;

    if (playlist_is_hls_url(url)) {
        HlsStream hls;
        if (hls_open(url, stop, &hls, error, error_size) != 0) {
            char detail[512];
            snprintf(detail, sizeof(detail), "could not open HLS stream: %s", error);
            snprintf(error, error_size, "%s", detail);
            return -1;
        }
        out->queue = hls.queue;
        out->format = format_for_hls(hls.codec);
        return 0;
    }

    if (net_open_stream(url, stop, &stream, error, error_size) != 0) {
        char detail[512];
        snprintf(detail, sizeof(detail), "could not open stream: %s", error);
        snprintf(error, error_size, "%s", detail);
        return -1;
    }

    /* The body's own first bytes are the most reliable signal there is, so look
     * at them before trusting the content type or the URL. */
    head_len = bytequeue_peek(stream.queue, head, sizeof(head), HEAD_PEEK_TIMEOUT_MS);

    /* A redirect may have landed on a playlist (CDN-signed HLS URLs do this), or
     * the station's URL may simply be a .m3u/.pls pointing at the real stream. */
    if (playlist_is_hls_url(stream.final_url) ||
        playlist_looks_like_playlist(head, head_len, stream.content_type)) {
        if (hops >= MAX_PLAYLIST_HOPS) {
            snprintf(error, error_size, "playlist redirects went in circles");
            bytequeue_close(stream.queue, NULL);
            bytequeue_release(stream.queue);
            return -1;
        }
        /* This body is text, not audio - drop it and follow where it points. */
        {
            char final_url[2048];
            snprintf(final_url, sizeof(final_url), "%s", stream.final_url);
            bytequeue_close(stream.queue, NULL);
            bytequeue_release(stream.queue);
            return follow_playlist(final_url, dataset_codec, stop, hops, out,
                                   error, error_size);
        }
    }

    out->queue = stream.queue;
    out->format = format_for_http(head, head_len, stream.content_type,
                                  stream.final_url, dataset_codec);
    return 0;
}

static int open_byte_source(const SessionStation *station, StopFlag *stop,
                            ByteSource *out, char *error, size_t error_size)
{
    char url[2048];

    snprintf(url, sizeof(url), "%s", station->url);

    if (station->needs_resolve) {
        /* TuneIn's browse listings only ever give a Tune.ashx resolver link,
         * never a directly playable stream - resolve it to the real URL first.
         * The response is just the URL as plain text. */
        Buf resolved;
        buf_init(&resolved);
        if (net_get_text(url, &resolved, error, error_size) != 0) {
            char detail[512];
            snprintf(detail, sizeof(detail), "could not resolve TuneIn stream: %s", error);
            snprintf(error, error_size, "%s", detail);
            buf_free(&resolved);
            return -1;
        }
        if (!playlist_first_line(resolved.data, url, sizeof(url))) {
            snprintf(error, error_size, "TuneIn resolver returned an empty stream URL");
            buf_free(&resolved);
            return -1;
        }
        buf_free(&resolved);
    }

    return open_stream_url(url, station->codec, stop, 0, out, error, error_size);
}

/* ---------------------------------------------------------------- session - */

/* Publishes the session's queue so player_stop can close it and unblock the
 * decoder. Returns 0 if this session has been superseded meanwhile. */
static int publish_queue(Player *player, unsigned long generation, ByteQueue *queue)
{
    int ok = 0;

    pthread_mutex_lock(&player->lock);
    if (player->has_active && player->generation == generation) {
        player->active.queue = bytequeue_retain(queue);
        ok = 1;
    }
    pthread_mutex_unlock(&player->lock);
    return ok;
}

static int publish_sink(Player *player, unsigned long generation, AudioSink *sink)
{
    int ok = 0;

    pthread_mutex_lock(&player->lock);
    if (player->has_active && player->generation == generation) {
        player->active.sink = audiosink_retain(sink);
        ok = 1;
    }
    pthread_mutex_unlock(&player->lock);
    return ok;
}

typedef enum {
    END_STREAM_ENDED,  /* the server closed the connection or the playlist ended */
    END_SUPERSEDED,    /* another play/stop took over; it owns the status line now */
    END_FAILED
} EndReason;

static EndReason run_playback(Player *player, const SessionStation *station,
                              unsigned long generation, StopFlag *stop,
                              char *error, size_t error_size)
{
    ByteSource source;
    Decoder *decoder = NULL;
    AudioSink *sink = NULL;
    AudioOutput *output = NULL;
    EndReason reason = END_FAILED;

    memset(&source, 0, sizeof(source));

    if (!is_current(player, generation))
        return END_SUPERSEDED;

    if (open_byte_source(station, stop, &source, error, error_size) != 0)
        return END_FAILED;

    if (!publish_queue(player, generation, source.queue)) {
        bytequeue_close(source.queue, NULL);
        bytequeue_release(source.queue);
        return END_SUPERSEDED;
    }

    if (source.format.codec == RWR_CODEC_UNKNOWN) {
        snprintf(error, error_size, "%s", source.format.description);
        goto finished;
    }

    decoder = decoder_new(source.format.codec, source.queue);
    if (!decoder) {
        snprintf(error, error_size, "could not start the %s decoder",
                 source.format.description);
        goto finished;
    }

    for (;;) {
        DecodedFrame frame;
        int status;

        if (!is_current(player, generation) || stopflag_is_set(stop)) {
            reason = END_SUPERSEDED;
            goto finished;
        }

        status = decoder_next(decoder, &frame, error, error_size);
        if (status == 0) {
            reason = END_STREAM_ENDED;
            goto finished;
        }
        if (status < 0) {
            if (!is_current(player, generation) || stopflag_is_set(stop))
                reason = END_SUPERSEDED;
            else
                reason = output ? END_STREAM_ENDED : END_FAILED;
            goto finished;
        }

        /* First decoded frame: only now is the actual sample rate and channel
         * count known, so this is when the device can be opened. */
        if (!output) {
            sink = audiosink_new(frame.channels);
            if (!sink) {
                snprintf(error, error_size, "out of memory");
                goto finished;
            }
            output = audiooutput_open(sink, frame.sample_rate, frame.channels,
                                      error, error_size);
            if (!output)
                goto finished;
            if (!publish_sink(player, generation, sink)) {
                reason = END_SUPERSEDED;
                goto finished;
            }
            emit_if_current(player, generation, RWR_EVENT_PLAYING, station->name, "");
        }

        /* A full ring blocks here, which is what paces decoding to real time. */
        if (!audiosink_push(sink, frame.samples,
                            frame.frame_count * (size_t)frame.channels)) {
            reason = END_SUPERSEDED;
            goto finished;
        }

        {
            const char *failure = audiooutput_failure(output);
            if (failure) {
                snprintf(error, error_size, "audio output failed: %s", failure);
                reason = END_FAILED;
                goto finished;
            }
        }
    }

finished:
    if (output)
        audiooutput_close(output);
    if (sink)
        audiosink_release(sink);
    if (decoder)
        decoder_free(decoder);
    if (source.queue) {
        bytequeue_close(source.queue, NULL);
        bytequeue_release(source.queue);
    }
    return reason;
}

static void *run_session(void *argument)
{
    SessionArgs *args = (SessionArgs *)argument;
    char error[512];
    EndReason reason;

    error[0] = '\0';
    reason = run_playback(args->player, &args->station, args->generation, args->stop,
                          error, sizeof(error));

    switch (reason) {
    case END_STREAM_ENDED:
        emit_if_current(args->player, args->generation, RWR_EVENT_STOPPED, "", "");
        break;
    case END_SUPERSEDED:
        break;
    case END_FAILED:
        emit_if_current(args->player, args->generation, RWR_EVENT_ERROR,
                        args->station.name,
                        error[0] ? error : "the station could not be played");
        break;
    }

    stopflag_release(args->stop);
    player_release(args->player);
    free(args);
    return NULL;
}

/* ----------------------------------------------------------------- player - */

static Player *player_retain(Player *player)
{
    pthread_mutex_lock(&player->lock);
    player->refcount++;
    pthread_mutex_unlock(&player->lock);
    return player;
}

static void player_release(Player *player)
{
    int dead;

    pthread_mutex_lock(&player->lock);
    dead = (--player->refcount == 0);
    pthread_mutex_unlock(&player->lock);
    if (!dead)
        return;
    pthread_mutex_destroy(&player->lock);
    free(player);
}

Player *player_new(void (*wake)(void *context), void *context)
{
    Player *player = (Player *)calloc(1, sizeof(*player));

    if (!player)
        return NULL;
    pthread_mutex_init(&player->lock, NULL);
    player->refcount = 1;
    player->wake = wake;
    player->wake_context = context;
    return player;
}

/* Signals the running session to unwind. Returns 1 if there was one. */
static int tear_down_active(Player *player)
{
    ActiveSession session;
    int had_one;

    pthread_mutex_lock(&player->lock);
    player->generation++;
    had_one = player->has_active;
    session = player->active;
    memset(&player->active, 0, sizeof(player->active));
    player->has_active = 0;
    pthread_mutex_unlock(&player->lock);

    if (!had_one)
        return 0;

    /* Mute first so audio stops on the very next callback, then close the byte
     * queue so a blocked decoder read returns at once rather than waiting on the
     * network. */
    stopflag_set(session.stop);
    if (session.sink) {
        audiosink_mute(session.sink);
        audiosink_release(session.sink);
    }
    if (session.queue) {
        bytequeue_close(session.queue, NULL);
        bytequeue_release(session.queue);
    }
    stopflag_release(session.stop);

    /* The pooled sockets belonged to the station that just stopped; the next one
     * is almost always a different host, and the server would drop them anyway. */
    net_close_idle_connections();
    return 1;
}

void player_play(Player *player, const Station *station)
{
    SessionArgs *args;
    StopFlag *stop;
    unsigned long generation;
    pthread_t thread;
    pthread_attr_t attributes;

    tear_down_active(player);

    stop = stopflag_new();
    args = (SessionArgs *)calloc(1, sizeof(*args));
    if (!stop || !args) {
        stopflag_release(stop);
        free(args);
        emit(player, RWR_EVENT_ERROR, station->name, "out of memory");
        return;
    }

    snprintf(args->station.name, sizeof(args->station.name), "%s", station->name);
    snprintf(args->station.url, sizeof(args->station.url), "%s", station->url);
    snprintf(args->station.codec, sizeof(args->station.codec), "%s", station->codec);
    args->station.needs_resolve = (station->flags & RWR_STATION_NEEDS_RESOLVE) != 0;

    pthread_mutex_lock(&player->lock);
    generation = ++player->generation;
    player->has_active = 1;
    memset(&player->active, 0, sizeof(player->active));
    player->active.stop = stopflag_retain(stop);
    pthread_mutex_unlock(&player->lock);

    args->player = player_retain(player);
    args->generation = generation;
    args->stop = stop; /* the session thread owns this reference */

    emit(player, RWR_EVENT_CONNECTING, station->name, "");

    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    /* The decode loop keeps a 16 kB input chunk on its stack and calls into
     * faad2, which is not frugal either; 512 kB is comfortable and still a
     * sixteenth of the pthread default. */
    pthread_attr_setstacksize(&attributes, 512 * 1024);
    if (pthread_create(&thread, &attributes, run_session, args) != 0) {
        pthread_attr_destroy(&attributes);
        stopflag_release(args->stop);
        player_release(args->player);
        free(args);
        tear_down_active(player);
        emit(player, RWR_EVENT_ERROR, station->name, "could not start the playback thread");
        return;
    }
    pthread_attr_destroy(&attributes);
}

void player_stop(Player *player)
{
    if (tear_down_active(player))
        emit(player, RWR_EVENT_STOPPED, "", "");
}

int player_is_active(Player *player)
{
    int active;

    pthread_mutex_lock(&player->lock);
    active = player->has_active;
    pthread_mutex_unlock(&player->lock);
    return active;
}

float player_level(Player *player)
{
    AudioSink *sink;
    float level;

    pthread_mutex_lock(&player->lock);
    sink = player->has_active ? player->active.sink : NULL;
    if (sink)
        audiosink_retain(sink);
    pthread_mutex_unlock(&player->lock);

    if (!sink)
        return 0.0f;
    level = audiosink_level(sink);
    audiosink_release(sink);
    return level;
}

void player_free(Player *player)
{
    if (!player)
        return;
    /* Bumps the generation and closes the queue, so any running session unwinds
     * promptly. The Player itself outlives this call if one is still in flight;
     * the last session thread to exit frees it. */
    tear_down_active(player);
    pthread_mutex_lock(&player->lock);
    player->wake = NULL;   /* the UI is going away - do not call back into it */
    pthread_mutex_unlock(&player->lock);
    player_release(player);
}
