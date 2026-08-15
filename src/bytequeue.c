#include "bytequeue.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* ~2 MB of undecoded audio: several seconds even for a 320kbps stream, which is
 * plenty of slack for an HLS segment fetch without letting a stalled decoder
 * grow the buffer unbounded. */
#define CAPACITY (2 * 1024 * 1024)

/* The ring is a fixed allocation rather than a growable deque: on a G4 with
 * 256 MB this is the single largest thing the app allocates per session, and a
 * fixed one cannot fragment the heap across a long listen. */
struct ByteQueue {
    pthread_mutex_t lock;
    pthread_cond_t  signal;

    unsigned char *ring;
    size_t         head;    /* next byte to read */
    size_t         count;   /* bytes currently buffered */

    int   closed;
    char *error;            /* malloc'd, NULL for a clean EOF */

    int refcount;
};

static void ms_from_now(int timeout_ms, struct timespec *out)
{
    struct timeval now;

    gettimeofday(&now, NULL);
    out->tv_sec = now.tv_sec + timeout_ms / 1000;
    out->tv_nsec = (long)now.tv_usec * 1000 + (long)(timeout_ms % 1000) * 1000000L;
    if (out->tv_nsec >= 1000000000L) {
        out->tv_sec += 1;
        out->tv_nsec -= 1000000000L;
    }
}

ByteQueue *bytequeue_new(void)
{
    ByteQueue *queue = (ByteQueue *)calloc(1, sizeof(*queue));

    if (!queue)
        return NULL;
    queue->ring = (unsigned char *)malloc(CAPACITY);
    if (!queue->ring) {
        free(queue);
        return NULL;
    }
    pthread_mutex_init(&queue->lock, NULL);
    pthread_cond_init(&queue->signal, NULL);
    queue->refcount = 1;
    return queue;
}

ByteQueue *bytequeue_retain(ByteQueue *queue)
{
    if (!queue)
        return NULL;
    pthread_mutex_lock(&queue->lock);
    queue->refcount++;
    pthread_mutex_unlock(&queue->lock);
    return queue;
}

void bytequeue_release(ByteQueue *queue)
{
    int dead;

    if (!queue)
        return;
    pthread_mutex_lock(&queue->lock);
    dead = (--queue->refcount == 0);
    pthread_mutex_unlock(&queue->lock);
    if (!dead)
        return;

    pthread_cond_destroy(&queue->signal);
    pthread_mutex_destroy(&queue->lock);
    free(queue->error);
    free(queue->ring);
    free(queue);
}

/* Copies as much of `data` as fits into the free space; caller holds the lock. */
static size_t write_locked(ByteQueue *queue, const unsigned char *data, size_t len)
{
    size_t free_space = CAPACITY - queue->count;
    size_t tail = (queue->head + queue->count) % CAPACITY;
    size_t first, second;

    if (len > free_space)
        len = free_space;
    first = CAPACITY - tail;
    if (first > len)
        first = len;
    memcpy(queue->ring + tail, data, first);
    second = len - first;
    if (second)
        memcpy(queue->ring, data + first, second);
    queue->count += len;
    return len;
}

int bytequeue_push(ByteQueue *queue, const void *data, size_t len)
{
    const unsigned char *cursor = (const unsigned char *)data;

    pthread_mutex_lock(&queue->lock);
    while (len > 0) {
        size_t written;

        while (!queue->closed && queue->count == CAPACITY)
            pthread_cond_wait(&queue->signal, &queue->lock);
        if (queue->closed) {
            pthread_mutex_unlock(&queue->lock);
            return 0;
        }
        written = write_locked(queue, cursor, len);
        cursor += written;
        len -= written;
        pthread_cond_broadcast(&queue->signal);
    }
    pthread_mutex_unlock(&queue->lock);
    return 1;
}

void bytequeue_close(ByteQueue *queue, const char *error)
{
    pthread_mutex_lock(&queue->lock);
    if (!queue->closed) {
        queue->closed = 1;
        if (error) {
            queue->error = strdup(error);
        }
    }
    pthread_cond_broadcast(&queue->signal);
    pthread_mutex_unlock(&queue->lock);
}

int bytequeue_is_closed(ByteQueue *queue)
{
    int closed;

    pthread_mutex_lock(&queue->lock);
    closed = queue->closed;
    pthread_mutex_unlock(&queue->lock);
    return closed;
}

long bytequeue_read(ByteQueue *queue, void *out, size_t len,
                    char *error, size_t error_size)
{
    unsigned char *cursor = (unsigned char *)out;
    size_t taken, first, second;

    if (len == 0)
        return 0;

    pthread_mutex_lock(&queue->lock);
    while (queue->count == 0) {
        if (queue->closed) {
            /* The error is reported once and then cleared, so a reader that
             * keeps calling after the failure sees a clean EOF rather than the
             * same message forever. */
            if (queue->error) {
                if (error && error_size)
                    snprintf(error, error_size, "%s", queue->error);
                free(queue->error);
                queue->error = NULL;
                pthread_mutex_unlock(&queue->lock);
                return -1;
            }
            pthread_mutex_unlock(&queue->lock);
            return 0;
        }
        pthread_cond_wait(&queue->signal, &queue->lock);
    }

    taken = len < queue->count ? len : queue->count;
    first = CAPACITY - queue->head;
    if (first > taken)
        first = taken;
    memcpy(cursor, queue->ring + queue->head, first);
    second = taken - first;
    if (second)
        memcpy(cursor + first, queue->ring, second);
    queue->head = (queue->head + taken) % CAPACITY;
    queue->count -= taken;

    pthread_cond_broadcast(&queue->signal);
    pthread_mutex_unlock(&queue->lock);
    return (long)taken;
}

size_t bytequeue_peek(ByteQueue *queue, void *out, size_t count, int timeout_ms)
{
    struct timespec deadline;
    unsigned char *cursor = (unsigned char *)out;
    size_t available, index;

    if (count == 0)
        return 0;
    if (count > CAPACITY)
        count = CAPACITY;
    ms_from_now(timeout_ms, &deadline);

    pthread_mutex_lock(&queue->lock);
    while (queue->count < count && !queue->closed) {
        if (pthread_cond_timedwait(&queue->signal, &queue->lock, &deadline) == ETIMEDOUT)
            break;
    }
    available = queue->count < count ? queue->count : count;
    /* Byte at a time through the modulo: this runs once per session on at most
     * 16 bytes, so the wrap-aware memcpy pair would be more code than it saves. */
    for (index = 0; index < available; index++)
        cursor[index] = queue->ring[(queue->head + index) % CAPACITY];
    pthread_mutex_unlock(&queue->lock);
    return available;
}

int bytequeue_wait_for_data(ByteQueue *queue, int timeout_ms)
{
    struct timespec deadline;
    int result;

    ms_from_now(timeout_ms, &deadline);
    pthread_mutex_lock(&queue->lock);
    for (;;) {
        if (queue->count > 0) {
            result = 1;
            break;
        }
        if (queue->closed) {
            result = 0;
            break;
        }
        if (pthread_cond_timedwait(&queue->signal, &queue->lock, &deadline) == ETIMEDOUT) {
            result = queue->count > 0;
            break;
        }
    }
    pthread_mutex_unlock(&queue->lock);
    return result;
}
