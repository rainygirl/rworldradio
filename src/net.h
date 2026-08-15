/* HTTP for the two things this app needs it for: small one-shot GETs (TuneIn
 * resolver links, HLS playlists and segments) and one long-lived streaming GET
 * (an icecast-style progressive audio body). Port of the Rust port's net.rs.
 *
 * Where the Rust ports get this from ureq + rustls, this is BSD sockets plus a
 * bundled mbedTLS - Tiger's own Secure Transport only speaks SSLv3/TLS 1.0 with
 * RSA key exchange, which today's streaming hosts refuse. See
 * vendor/README.md for how mbedTLS is configured and why. */
#ifndef RWR_NET_H
#define RWR_NET_H

#include <stddef.h>

#include "buf.h"
#include "bytequeue.h"
#include "stopflag.h"

/* Called once at startup, before any other function here. Loads the CA bundle
 * lazily on the first HTTPS connection, so this itself is cheap. */
void net_init(const char *ca_bundle_path);
void net_shutdown(void);

/* Connection accounting, for checking that keep-alive is doing its job.
 *
 * `reused` counts requests served on a connection the pool already had - those
 * cost no TCP setup and no TLS handshake at all. On an HTTPS HLS station,
 * playlist polls and segment fetches all go to one host, so nearly every request
 * after the first should land here. */
typedef struct {
    unsigned opened;          /* connections dialled */
    unsigned reused;          /* requests served from the pool */
    unsigned stale;           /* pooled connections found dead, retried fresh */
    unsigned handshakes;        /* TLS handshakes performed */
    double   handshake_seconds; /* total time spent in them */
    double   handshake_min;     /* fastest and slowest, to separate a resumed */
    double   handshake_max;     /* handshake from a full one */
} NetStats;

void net_stats(NetStats *out);

/* Drops every pooled connection. Worth calling when playback stops: the sockets
 * are of no further use and the server will drop them anyway. */
void net_close_idle_connections(void);

/* Blocking GET of a whole body. Both append to `out` and return 0 on success, or
 * -1 with a reason written into `error`.
 *
 * get_text stops at 8 MB (a playlist or resolver response is a few kB, so
 * anything near that is a wrong URL) and get_bytes at 32 MB (a runaway guard,
 * not a real limit - a single HLS audio segment is well under a megabyte). */
int net_get_text(const char *url, Buf *out, char *error, size_t error_size);
int net_get_bytes(const char *url, Buf *out, char *error, size_t error_size);

/* Result of opening a progressive stream. `queue` is owned by the caller, who
 * releases it; the worker thread holds its own reference. */
typedef struct {
    ByteQueue *queue;
    char       content_type[128]; /* "" if the server sent none */
    char       final_url[2048];   /* the URL actually streamed from, after redirects */
} NetStream;

/* Starts a streaming GET on a worker thread that writes the response body into
 * `out->queue`. Returns once the response *headers* are in, so the caller can
 * look at the content type before any audio arrives.
 *
 * The worker stops as soon as `*stop` is set or the queue is closed; the caller
 * closing the queue is what makes stopping playback immediate rather than
 * waiting on a socket read. The worker holds its own reference to `stop`. */
int net_open_stream(const char *url, StopFlag *stop, NetStream *out,
                    char *error, size_t error_size);

/* Exposed for the tests. Splits a URL into its parts; returns 0 on success.
 * `port` gets the scheme default when the URL has none. */
int net_parse_url(const char *url, int *is_https, char *host, size_t host_size,
                  int *port, char *path, size_t path_size);

#endif
