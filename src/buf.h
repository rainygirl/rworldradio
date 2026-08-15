/* Growable byte buffer, used everywhere a Vec<u8>/String appears in the Rust
 * ports: HTTP bodies, demuxed elementary streams, URL building.
 *
 * Always keeps a NUL one past `len` so `b->data` can be handed to str* functions
 * when the contents happen to be text. `len` never counts that NUL. */
#ifndef RWR_BUF_H
#define RWR_BUF_H

#include <stddef.h>

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} Buf;

void  buf_init(Buf *b);
void  buf_free(Buf *b);
void  buf_clear(Buf *b);

/* All four return 0 on success and -1 if the allocation failed; on failure the
 * buffer is left valid and unchanged, so a caller may keep using it. */
int   buf_reserve(Buf *b, size_t extra);
int   buf_append(Buf *b, const void *data, size_t len);
int   buf_append_str(Buf *b, const char *text);
int   buf_appendf(Buf *b, const char *format, ...);

/* Removes the first `count` bytes, shifting the rest down. */
void  buf_drain_front(Buf *b, size_t count);

/* Hands the buffer's storage to the caller, who must free() it. The buffer is
 * reset to empty. Returns NULL only if nothing was ever allocated. */
char *buf_release(Buf *b);

#endif
