#include "buf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void buf_init(Buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(Buf *b)
{
    free(b->data);
    buf_init(b);
}

void buf_clear(Buf *b)
{
    b->len = 0;
    if (b->data)
        b->data[0] = '\0';
}

int buf_reserve(Buf *b, size_t extra)
{
    size_t needed = b->len + extra + 1; /* +1 for the trailing NUL */
    size_t cap;
    char *grown;

    if (needed <= b->cap)
        return 0;

    cap = b->cap ? b->cap : 64;
    while (cap < needed) {
        /* Doubling would overflow on a 32-bit size_t long before this matters
         * for real data, but a corrupt Content-Length could get here. */
        if (cap > (size_t)-1 / 2)
            return -1;
        cap *= 2;
    }

    grown = (char *)realloc(b->data, cap);
    if (!grown)
        return -1;
    b->data = grown;
    b->cap = cap;
    if (b->len == 0)
        b->data[0] = '\0';
    return 0;
}

int buf_append(Buf *b, const void *data, size_t len)
{
    if (len == 0)
        return 0;
    if (buf_reserve(b, len) != 0)
        return -1;
    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = '\0';
    return 0;
}

int buf_append_str(Buf *b, const char *text)
{
    return text ? buf_append(b, text, strlen(text)) : 0;
}

int buf_appendf(Buf *b, const char *format, ...)
{
    va_list args;
    int needed;
    char probe[256];

    va_start(args, format);
    needed = vsnprintf(probe, sizeof(probe), format, args);
    va_end(args);
    if (needed < 0)
        return -1;

    if ((size_t)needed < sizeof(probe))
        return buf_append(b, probe, (size_t)needed);

    /* Too long for the stack probe: make room and format straight into the
     * buffer. */
    if (buf_reserve(b, (size_t)needed) != 0)
        return -1;
    va_start(args, format);
    vsnprintf(b->data + b->len, (size_t)needed + 1, format, args);
    va_end(args);
    b->len += (size_t)needed;
    return 0;
}

void buf_drain_front(Buf *b, size_t count)
{
    if (count >= b->len) {
        buf_clear(b);
        return;
    }
    memmove(b->data, b->data + count, b->len - count);
    b->len -= count;
    b->data[b->len] = '\0';
}

char *buf_release(Buf *b)
{
    char *data = b->data;
    buf_init(b);
    return data;
}
