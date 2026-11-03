/* A bounded producer/consumer byte pipe with an explicit close. Port of the Rust
 * port's byte_queue.rs, which in turn replaced the Haiku build's HlsAdapterIO
 * (a BAdapterIO feeding BInputAdapter).
 *
 * A network thread pushes bytes in and the decoder thread reads them out. The
 * bound gives the producer backpressure so a live stream cannot buffer without
 * limit, and closing is what makes stopping responsive: a blocked reader wakes
 * up and sees EOF instead of waiting out a socket timeout. */
#ifndef RWR_BYTEQUEUE_H
#define RWR_BYTEQUEUE_H

#include <stddef.h>

typedef struct ByteQueue ByteQueue;

/* Refcounted because the producer thread, the decoder thread and the player's
 * session record all hold one, and any of the three may outlive the others. */
ByteQueue *bytequeue_new(void);
ByteQueue *bytequeue_retain(ByteQueue *queue);
void       bytequeue_release(ByteQueue *queue);

/* Appends `data`, blocking while the queue is over capacity. Returns 0 once the
 * queue is closed, which is the producer's signal to stop. */
int bytequeue_push(ByteQueue *queue, const void *data, size_t len);

/* Marks the stream finished. A non-NULL `error` is reported to the reader once
 * the buffered bytes are drained, instead of a clean EOF. The string is copied.
 * Only the first close of a queue takes effect. */
void bytequeue_close(ByteQueue *queue, const char *error);
int  bytequeue_is_closed(ByteQueue *queue);

/* Reads up to `len` bytes, blocking until at least one is available.
 * Returns  >0  bytes read
 *           0  clean end of stream
 *          -1  the stream was closed with an error; it is copied into
 *              `error` (which may be NULL) */
long bytequeue_read(ByteQueue *queue, void *out, size_t len,
                    char *error, size_t error_size);

/* Copies up to `count` leading bytes WITHOUT consuming them, waiting up to
 * `timeout_ms` for that many to arrive. Used to identify a stream's codec from
 * its first frame header before handing it to a decoder. Returns how many bytes
 * were copied. */
size_t bytequeue_peek(ByteQueue *queue, void *out, size_t count, int timeout_ms);

/* Blocks until at least one byte is available, the queue is closed, or
 * `timeout_ms` elapses. Returns 1 if data is waiting. */
int bytequeue_wait_for_data(ByteQueue *queue, int timeout_ms);

#endif
