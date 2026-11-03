#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "der.h"
#include "m3u8.h"
#include "stopflag.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#define USER_AGENT       "RWorldRadio/1.0 (Mac OS X PPC)"
#define CONNECT_TIMEOUT  10   /* seconds */
#define TEXT_READ_TIMEOUT 15
#define BYTES_READ_TIMEOUT 20
#define MAX_REDIRECTS    5
#define TEXT_LIMIT       (8u * 1024u * 1024u)
#define BYTES_LIMIT      (32u * 1024u * 1024u)
#define STREAM_CHUNK     (32 * 1024)

/* ------------------------------------------------------------------ TLS --- */

/* One entropy source and DRBG for the whole process. mbedTLS is built here
 * without MBEDTLS_THREADING_C (it would pull in its own pthread wrappers for no
 * benefit at this scale), so the DRBG is serialized by hand: a handshake on the
 * HLS worker can overlap one on the session thread. */
static pthread_mutex_t tls_lock = PTHREAD_MUTEX_INITIALIZER;
static mbedtls_entropy_context  tls_entropy;
static mbedtls_ctr_drbg_context tls_drbg;
static mbedtls_x509_crt         tls_ca_chain;
static int   tls_ready;          /* entropy/DRBG seeded */
static int   tls_ca_loaded;      /* CA chain parsed, or permanently failed */
static int   tls_ca_ok;
static char  tls_ca_path[1024];
static char  tls_ca_error[256];

/* Client-side TLS session cache.
 *
 * The connection pool removes most handshakes, but not all: the pool is emptied
 * when a server hangs up, and an HLS stream whose segments live on a different
 * host than its playlist alternates between two connections. Resuming a session
 * turns the full handshake that would follow - an ECDHE key exchange plus
 * certificate chain verification, the expensive part on a G4 - into one round
 * trip and a hash.
 *
 * Keyed by host, guarded by tls_lock along with the rest of the TLS state. */
#define TLS_SESSION_CACHE 4

static struct {
    char                 host[256];
    mbedtls_ssl_session  session;
    int                  valid;
} tls_session_cache[TLS_SESSION_CACHE];
static unsigned tls_session_next; /* round-robin eviction */

static int drbg_random(void *unused, unsigned char *output, size_t len)
{
    int result;
    (void)unused;
    pthread_mutex_lock(&tls_lock);
    result = mbedtls_ctr_drbg_random(&tls_drbg, output, len);
    pthread_mutex_unlock(&tls_lock);
    return result;
}

void net_init(const char *ca_bundle_path)
{
    static const char personalization[] = "rworldradio";

    mbedtls_entropy_init(&tls_entropy);
    mbedtls_ctr_drbg_init(&tls_drbg);
    mbedtls_x509_crt_init(&tls_ca_chain);
    snprintf(tls_ca_path, sizeof(tls_ca_path), "%s", ca_bundle_path ? ca_bundle_path : "");

    tls_ready = mbedtls_ctr_drbg_seed(&tls_drbg, mbedtls_entropy_func, &tls_entropy,
                                      (const unsigned char *)personalization,
                                      sizeof(personalization) - 1) == 0;
    if (!tls_ready)
        snprintf(tls_ca_error, sizeof(tls_ca_error), "could not seed the random generator");
}

void net_shutdown(void)
{
    unsigned index;

    net_close_idle_connections();

    pthread_mutex_lock(&tls_lock);
    for (index = 0; index < TLS_SESSION_CACHE; index++) {
        if (tls_session_cache[index].valid) {
            mbedtls_ssl_session_free(&tls_session_cache[index].session);
            tls_session_cache[index].valid = 0;
        }
    }
    pthread_mutex_unlock(&tls_lock);

    mbedtls_x509_crt_free(&tls_ca_chain);
    mbedtls_ctr_drbg_free(&tls_drbg);
    mbedtls_entropy_free(&tls_entropy);
    tls_ready = 0;
    tls_ca_loaded = 0;
    tls_ca_ok = 0;
}

/* Loads the CA store on first use rather than at startup: nothing needs it until
 * the first HTTPS connection, and doing it here keeps it off the launch path. */
static int ensure_ca_chain(char *error, size_t error_size)
{
    FILE *file;
    long size;
    unsigned char *data;
    size_t offset = 0;
    int loaded = 0;

    pthread_mutex_lock(&tls_lock);
    if (tls_ca_loaded) {
        int ok = tls_ca_ok;
        if (!ok && error && error_size)
            snprintf(error, error_size, "%s", tls_ca_error);
        pthread_mutex_unlock(&tls_lock);
        return ok;
    }
    tls_ca_loaded = 1;

    file = fopen(tls_ca_path, "rb");
    if (!file) {
        snprintf(tls_ca_error, sizeof(tls_ca_error),
                 "no CA store at %s - HTTPS stations cannot be verified", tls_ca_path);
        goto done;
    }
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0 || size > 8 * 1024 * 1024) {
        snprintf(tls_ca_error, sizeof(tls_ca_error), "%s: implausible CA store size",
                 tls_ca_path);
        fclose(file);
        goto done;
    }
    /* One extra byte: mbedtls_x509_crt_parse requires PEM input to be
     * NUL-terminated and counts that NUL in the length it is given. */
    data = (unsigned char *)malloc((size_t)size + 1);
    if (!data) {
        snprintf(tls_ca_error, sizeof(tls_ca_error), "out of memory reading the CA store");
        fclose(file);
        goto done;
    }
    if (fread(data, 1, (size_t)size, file) != (size_t)size) {
        snprintf(tls_ca_error, sizeof(tls_ca_error), "%s: short read", tls_ca_path);
        free(data);
        fclose(file);
        goto done;
    }
    fclose(file);
    data[size] = '\0';

    if (memcmp(data, "-----BEGIN", 10) == 0) {
        /* A plain PEM bundle, so someone pointed the app at a system file. */
        if (mbedtls_x509_crt_parse(&tls_ca_chain, data, (size_t)size + 1) >= 0)
            loaded = 1;
    } else {
        while (offset < (size_t)size) {
            size_t object = der_object_len(data + offset, (size_t)size - offset);
            if (object == 0)
                break;
            if (mbedtls_x509_crt_parse_der(&tls_ca_chain, data + offset, object) == 0)
                loaded++;
            offset += object;
        }
    }
    free(data);

    if (!loaded)
        snprintf(tls_ca_error, sizeof(tls_ca_error), "%s: no usable certificates",
                 tls_ca_path);

done:
    tls_ca_ok = loaded > 0;
    if (!tls_ca_ok && error && error_size)
        snprintf(error, error_size, "%s", tls_ca_error);
    pthread_mutex_unlock(&tls_lock);
    return tls_ca_ok;
}

/* -------------------------------------------------------------- transport - */

typedef struct {
    int  fd;
    int  tls;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config  conf;

    unsigned char buffer[8192];
    size_t        buffer_pos;
    size_t        buffer_len;
    int           eof;

    /* Where this connection goes, so the pool can match a later request to it. */
    char   host[256];
    int    port;
    double idle_since;
} Conn;

/* ------------------------------------------------------- connection pool - */

/* Every request used to open its own connection and close it again. For a
 * progressive stream that is one connection for the whole session and costs
 * nothing. For HTTPS HLS it is a full TLS handshake for every playlist poll and
 * every segment - several a minute, forever - and a full handshake measures
 * 1.2-2.7 s on the 700 MHz G4 this targets. Keeping the connection instead is
 * the single biggest thing that can be done for HLS on this hardware: 60 s of
 * an HTTPS HLS station went from 22 handshakes and 41 s of handshaking to 2 and
 * 3.3 s.
 *
 * Four slots is more than enough: a session talks to one host for the playlist
 * and usually the same host for segments. */
#define POOL_SIZE 4

/* Servers close idle keep-alive connections on their own schedule, commonly
 * after 5-15 s. Anything older than this is closed rather than gambled on -
 * though a stale one is survivable, since http_request retries once on a fresh
 * connection. */
#define POOL_IDLE_SECONDS 20.0

static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
static Conn *pool[POOL_SIZE];

/* Counters behind net_stats(), so the effect is measurable rather than assumed. */
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;
static NetStats stats;

static double now_seconds(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    return (double)now.tv_sec + (double)now.tv_usec / 1000000.0;
}

/* Cipher suite order. ChaCha20-Poly1305 first is deliberate: it is 32-bit ARX
 * only, so on a G4 it neither uses the FPU nor pushes AES tables through the
 * 256 kB L2, and it measures faster than table-driven AES-GCM there. AES-128
 * suites follow for servers that do not offer ChaCha20. */
static const int PREFERRED_CIPHERSUITES[] = {
    MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,
    MBEDTLS_TLS_RSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_RSA_WITH_AES_128_CBC_SHA256,
    MBEDTLS_TLS_RSA_WITH_AES_128_CBC_SHA,
    0
};

static void set_timeout(int fd, int option, int seconds)
{
    struct timeval timeout;
    timeout.tv_sec = seconds;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, option, &timeout, sizeof(timeout));
}

/* Non-blocking connect plus select, because SO_SNDTIMEO does not bound connect()
 * and a dead host would otherwise hold the session thread for the kernel's full
 * TCP timeout. */
static int tcp_connect(const char *host, int port, char *error, size_t error_size)
{
    struct addrinfo hints, *list = NULL, *entry;
    char service[16];
    int fd = -1, result;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof(service), "%d", port);

    result = getaddrinfo(host, service, &hints, &list);
    if (result != 0 || !list) {
        snprintf(error, error_size, "%s: %s", host, gai_strerror(result));
        return -1;
    }

    for (entry = list; entry; entry = entry->ai_next) {
        int flags;
        fd_set writable;
        struct timeval timeout;
        int error_code = 0;
        socklen_t error_len = sizeof(error_code);

        fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (fd < 0)
            continue;

        flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        if (connect(fd, entry->ai_addr, entry->ai_addrlen) == 0)
            goto connected;
        if (errno != EINPROGRESS) {
            close(fd);
            fd = -1;
            continue;
        }

        FD_ZERO(&writable);
        FD_SET(fd, &writable);
        timeout.tv_sec = CONNECT_TIMEOUT;
        timeout.tv_usec = 0;
        if (select(fd + 1, NULL, &writable, NULL, &timeout) <= 0) {
            close(fd);
            fd = -1;
            continue;
        }
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error_code, &error_len) != 0 ||
            error_code != 0) {
            close(fd);
            fd = -1;
            continue;
        }

    connected:
        fcntl(fd, F_SETFL, flags);
        {
            int one = 1;
            /* Audio arrives as a steady trickle; Nagle would add up to 200 ms of
             * latency to the request itself for no benefit. */
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        }
        break;
    }

    freeaddrinfo(list);
    if (fd < 0)
        snprintf(error, error_size, "could not connect to %s:%d", host, port);
    return fd;
}

static int bio_send(void *context, const unsigned char *data, size_t len)
{
    int fd = *(int *)context;
    ssize_t written = send(fd, data, len, 0);

    if (written >= 0)
        return (int)written;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv(void *context, unsigned char *data, size_t len)
{
    int fd = *(int *)context;
    ssize_t got = recv(fd, data, len, 0);

    if (got >= 0)
        return (int)got;
    if (errno == EINTR)
        return MBEDTLS_ERR_SSL_WANT_READ;
    /* SO_RCVTIMEO expiry surfaces as EAGAIN. For a blocking socket that is a
     * real timeout, not "try again", so it must not loop forever. */
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return MBEDTLS_ERR_SSL_TIMEOUT;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

static void describe_tls_error(int code, char *error, size_t error_size)
{
    char detail[128];
    mbedtls_strerror(code, detail, sizeof(detail));
    snprintf(error, error_size, "TLS: %s", detail);
}

/* Offers this host's cached session to `ssl`, if there is one.
 *
 * mbedtls_ssl_set_session copies what it is given into the context, so this can
 * hand it the cache entry directly under the lock - mbedTLS 2.28 has no public
 * way to copy a session out. It touches no RNG, so holding tls_lock across it
 * cannot deadlock against drbg_random. */
static void tls_session_apply(const char *host, mbedtls_ssl_context *ssl)
{
    unsigned index;

    pthread_mutex_lock(&tls_lock);
    for (index = 0; index < TLS_SESSION_CACHE; index++) {
        if (tls_session_cache[index].valid &&
            strcmp(tls_session_cache[index].host, host) == 0) {
            mbedtls_ssl_set_session(ssl, &tls_session_cache[index].session);
            break;
        }
    }
    pthread_mutex_unlock(&tls_lock);
}

static void tls_session_store(const char *host, mbedtls_ssl_context *ssl)
{
    unsigned index, slot = TLS_SESSION_CACHE;

    pthread_mutex_lock(&tls_lock);
    for (index = 0; index < TLS_SESSION_CACHE; index++) {
        if (tls_session_cache[index].valid &&
            strcmp(tls_session_cache[index].host, host) == 0) {
            slot = index;
            break;
        }
        if (!tls_session_cache[index].valid && slot == TLS_SESSION_CACHE)
            slot = index;
    }
    if (slot == TLS_SESSION_CACHE)
        slot = tls_session_next++ % TLS_SESSION_CACHE;

    if (tls_session_cache[slot].valid)
        mbedtls_ssl_session_free(&tls_session_cache[slot].session);
    mbedtls_ssl_session_init(&tls_session_cache[slot].session);

    if (mbedtls_ssl_get_session(ssl, &tls_session_cache[slot].session) == 0) {
        snprintf(tls_session_cache[slot].host, sizeof(tls_session_cache[slot].host),
                 "%s", host);
        tls_session_cache[slot].valid = 1;
    } else {
        mbedtls_ssl_session_free(&tls_session_cache[slot].session);
        tls_session_cache[slot].valid = 0;
    }
    pthread_mutex_unlock(&tls_lock);
}

static void conn_free(Conn *conn)
{
    if (!conn)
        return;
    if (conn->tls) {
        mbedtls_ssl_close_notify(&conn->ssl);
        mbedtls_ssl_free(&conn->ssl);
        mbedtls_ssl_config_free(&conn->conf);
    }
    if (conn->fd >= 0)
        close(conn->fd);
    free(conn);
}

/* Dials a new connection. Returns NULL with a reason in `error`. */
static Conn *conn_open(const char *host, int port, int use_tls, int read_timeout,
                       char *error, size_t error_size)
{
    Conn *conn;
    int result;

    conn = (Conn *)calloc(1, sizeof(*conn));
    if (!conn) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    conn->fd = -1;
    snprintf(conn->host, sizeof(conn->host), "%s", host);
    conn->port = port;

    conn->fd = tcp_connect(host, port, error, error_size);
    if (conn->fd < 0) {
        free(conn);
        return NULL;
    }
    set_timeout(conn->fd, SO_RCVTIMEO, read_timeout);
    set_timeout(conn->fd, SO_SNDTIMEO, read_timeout);

    pthread_mutex_lock(&stats_lock);
    stats.opened++;
    pthread_mutex_unlock(&stats_lock);

    if (!use_tls)
        return conn;

    if (!tls_ready) {
        snprintf(error, error_size, "TLS is unavailable: %s", tls_ca_error);
        goto plain_failed;
    }
    if (!ensure_ca_chain(error, error_size))
        goto plain_failed;

    conn->tls = 1;
    mbedtls_ssl_init(&conn->ssl);
    mbedtls_ssl_config_init(&conn->conf);

    result = mbedtls_ssl_config_defaults(&conn->conf, MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    if (result != 0)
        goto tls_failed;

    mbedtls_ssl_conf_authmode(&conn->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&conn->conf, &tls_ca_chain, NULL);
    mbedtls_ssl_conf_rng(&conn->conf, drbg_random, NULL);
    mbedtls_ssl_conf_ciphersuites(&conn->conf, PREFERRED_CIPHERSUITES);
    /* TLS 1.2 only: 1.0/1.1 are refused by the same hosts Tiger's own Secure
     * Transport cannot reach, so offering them buys nothing and costs code. */
    mbedtls_ssl_conf_min_version(&conn->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);

    result = mbedtls_ssl_setup(&conn->ssl, &conn->conf);
    if (result != 0)
        goto tls_failed;
    /* SNI. Without it a shared-IP CDN serves the wrong certificate and the
     * handshake fails on almost every HTTPS station. */
    result = mbedtls_ssl_set_hostname(&conn->ssl, host);
    if (result != 0)
        goto tls_failed;

    /* Offer a previous session for this host. If the server takes it the
     * handshake skips the key exchange and the certificate chain entirely. */
    tls_session_apply(host, &conn->ssl);

    mbedtls_ssl_set_bio(&conn->ssl, &conn->fd, bio_send, bio_recv, NULL);

    {
        double began = now_seconds();

        while ((result = mbedtls_ssl_handshake(&conn->ssl)) != 0) {
            if (result != MBEDTLS_ERR_SSL_WANT_READ &&
                result != MBEDTLS_ERR_SSL_WANT_WRITE)
                goto tls_failed;
        }

        {
            double took = now_seconds() - began;
            pthread_mutex_lock(&stats_lock);
            if (stats.handshakes == 0 || took < stats.handshake_min)
                stats.handshake_min = took;
            if (took > stats.handshake_max)
                stats.handshake_max = took;
            stats.handshakes++;
            stats.handshake_seconds += took;
            pthread_mutex_unlock(&stats_lock);
        }
    }

    tls_session_store(host, &conn->ssl);
    return conn;

tls_failed:
    describe_tls_error(result, error, error_size);
    mbedtls_ssl_free(&conn->ssl);
    mbedtls_ssl_config_free(&conn->conf);
    conn->tls = 0;
plain_failed:
    close(conn->fd);
    free(conn);
    return NULL;
}

/* Hands a still-usable connection back for the next request to the same host. */
static void pool_put(Conn *conn)
{
    int index;

    if (!conn)
        return;

    conn->idle_since = now_seconds();
    conn->buffer_pos = 0;
    conn->buffer_len = 0;

    pthread_mutex_lock(&pool_lock);
    for (index = 0; index < POOL_SIZE; index++) {
        if (!pool[index]) {
            pool[index] = conn;
            pthread_mutex_unlock(&pool_lock);
            return;
        }
    }
    /* Pool full: drop the oldest rather than this one, which is the warmest. */
    {
        int oldest = 0;
        for (index = 1; index < POOL_SIZE; index++) {
            if (pool[index]->idle_since < pool[oldest]->idle_since)
                oldest = index;
        }
        {
            Conn *evicted = pool[oldest];
            pool[oldest] = conn;
            pthread_mutex_unlock(&pool_lock);
            conn_free(evicted);
        }
    }
}

/* An idle connection to this host, or NULL. */
static Conn *pool_take(const char *host, int port, int tls)
{
    double now = now_seconds();
    int index;
    Conn *found = NULL;

    pthread_mutex_lock(&pool_lock);
    for (index = 0; index < POOL_SIZE; index++) {
        Conn *candidate = pool[index];
        if (!candidate)
            continue;
        if (now - candidate->idle_since > POOL_IDLE_SECONDS) {
            pool[index] = NULL;
            pthread_mutex_unlock(&pool_lock);
            conn_free(candidate);
            pthread_mutex_lock(&pool_lock);
            continue;
        }
        if (candidate->port == port && candidate->tls == tls &&
            strcmp(candidate->host, host) == 0) {
            pool[index] = NULL;
            found = candidate;
            break;
        }
    }
    pthread_mutex_unlock(&pool_lock);

    if (found) {
        found->eof = 0;
        pthread_mutex_lock(&stats_lock);
        stats.reused++;
        pthread_mutex_unlock(&stats_lock);
    }
    return found;
}

void net_close_idle_connections(void)
{
    Conn *taken[POOL_SIZE];
    int index;

    pthread_mutex_lock(&pool_lock);
    for (index = 0; index < POOL_SIZE; index++) {
        taken[index] = pool[index];
        pool[index] = NULL;
    }
    pthread_mutex_unlock(&pool_lock);

    for (index = 0; index < POOL_SIZE; index++)
        conn_free(taken[index]);
}

void net_stats(NetStats *out)
{
    pthread_mutex_lock(&stats_lock);
    *out = stats;
    pthread_mutex_unlock(&stats_lock);
}

static int conn_write(Conn *conn, const void *data, size_t len)
{
    const unsigned char *cursor = (const unsigned char *)data;

    while (len > 0) {
        int written;
        if (conn->tls) {
            written = mbedtls_ssl_write(&conn->ssl, cursor, len);
            if (written == MBEDTLS_ERR_SSL_WANT_READ ||
                written == MBEDTLS_ERR_SSL_WANT_WRITE)
                continue;
        } else {
            ssize_t sent = send(conn->fd, cursor, len, 0);
            if (sent < 0 && errno == EINTR)
                continue;
            written = (int)sent;
        }
        if (written <= 0)
            return -1;
        cursor += written;
        len -= (size_t)written;
    }
    return 0;
}

/* Refills the read buffer. Returns bytes available, 0 at EOF, -1 on error. */
static int conn_fill(Conn *conn)
{
    int got;

    if (conn->buffer_pos < conn->buffer_len)
        return (int)(conn->buffer_len - conn->buffer_pos);
    if (conn->eof)
        return 0;

    conn->buffer_pos = 0;
    conn->buffer_len = 0;
    for (;;) {
        if (conn->tls) {
            got = mbedtls_ssl_read(&conn->ssl, conn->buffer, sizeof(conn->buffer));
            if (got == MBEDTLS_ERR_SSL_WANT_READ || got == MBEDTLS_ERR_SSL_WANT_WRITE)
                continue;
            if (got == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                conn->eof = 1;
                return 0;
            }
        } else {
            ssize_t received = recv(conn->fd, conn->buffer, sizeof(conn->buffer), 0);
            if (received < 0 && errno == EINTR)
                continue;
            got = (int)received;
        }
        break;
    }

    if (got == 0) {
        conn->eof = 1;
        return 0;
    }
    if (got < 0)
        return -1;
    conn->buffer_len = (size_t)got;
    return got;
}

/* Reads up to `len` bytes. Returns bytes read, 0 at EOF, -1 on error. */
static long conn_read(Conn *conn, void *out, size_t len)
{
    int available = conn_fill(conn);
    size_t taken;

    if (available <= 0)
        return available;
    taken = (size_t)available < len ? (size_t)available : len;
    memcpy(out, conn->buffer + conn->buffer_pos, taken);
    conn->buffer_pos += taken;
    return (long)taken;
}

/* Reads one CRLF- or LF-terminated line into `line` (NUL-terminated, without the
 * terminator). Returns 0 on success, -1 on EOF or error. */
static int conn_read_line(Conn *conn, char *line, size_t size)
{
    size_t length = 0;

    for (;;) {
        int available = conn_fill(conn);
        char c;

        if (available <= 0)
            return length > 0 ? 0 : -1;
        c = (char)conn->buffer[conn->buffer_pos++];
        if (c == '\n')
            break;
        if (c != '\r' && length + 1 < size)
            line[length++] = c;
    }
    line[length] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ HTTP -- */

int net_parse_url(const char *url, int *is_https, char *host, size_t host_size,
                  int *port, char *path, size_t path_size)
{
    const char *cursor, *host_start, *host_end, *path_start;
    size_t host_len;

    if (strncmp(url, "http://", 7) == 0) {
        *is_https = 0;
        *port = 80;
        cursor = url + 7;
    } else if (strncmp(url, "https://", 8) == 0) {
        *is_https = 1;
        *port = 443;
        cursor = url + 8;
    } else {
        return -1;
    }

    /* Skip any userinfo ("user:pass@host"); the app never sends credentials, but
     * a URL carrying them must still resolve to the right host. */
    {
        const char *at = strchr(cursor, '@');
        const char *slash = strchr(cursor, '/');
        if (at && (!slash || at < slash))
            cursor = at + 1;
    }

    host_start = cursor;
    if (*cursor == '[') {
        /* Bracketed IPv6 literal. */
        const char *close_bracket = strchr(cursor, ']');
        if (!close_bracket)
            return -1;
        host_start = cursor + 1;
        host_end = close_bracket;
        cursor = close_bracket + 1;
    } else {
        host_end = cursor;
        while (*host_end && *host_end != ':' && *host_end != '/' && *host_end != '?')
            host_end++;
        cursor = host_end;
    }

    host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len + 1 > host_size)
        return -1;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';

    if (*cursor == ':') {
        *port = (int)strtol(cursor + 1, NULL, 10);
        if (*port <= 0 || *port > 65535)
            return -1;
        while (*cursor && *cursor != '/' && *cursor != '?')
            cursor++;
    }

    path_start = cursor;
    if (*path_start == '\0') {
        snprintf(path, path_size, "/");
    } else if (*path_start == '?') {
        snprintf(path, path_size, "/%s", path_start);
    } else {
        snprintf(path, path_size, "%s", path_start);
    }
    return 0;
}

typedef struct {
    Conn *conn;
    int   status;
    int   chunked;
    long  content_length;   /* -1 when the server did not say */
    long  body_read;
    long  chunk_remaining;
    int   chunk_done;
    char  content_type[128];
    /* Whether this connection may go back in the pool once the body has been
     * read in full: the server must not have said "Connection: close", and the
     * body must be delimited so we know where it ends. A read-until-close body
     * leaves no way to tell the end of the response from the end of the
     * connection, so those are never reused. */
    int   keep_alive;
} HttpResponse;

static int parse_status_line(const char *line)
{
    if (strncmp(line, "HTTP/", 5) != 0)
        return -1;
    line = strchr(line, ' ');
    if (!line)
        return -1;
    return (int)strtol(line + 1, NULL, 10);
}

static int header_is(const char *line, const char *name, const char **value)
{
    size_t len = strlen(name);

    if (strncasecmp(line, name, len) != 0 || line[len] != ':')
        return 0;
    line += len + 1;
    while (*line == ' ' || *line == '\t')
        line++;
    *value = line;
    return 1;
}

/* Sends the request and reads the response headers. `location` receives the
 * redirect target when the status is a 3xx. */
/* Builds and sends the request, then reads the status line and headers.
 * Returns 0 on success, -1 on a transport failure. */
static int send_and_read_headers(HttpResponse *response, const char *host, int port,
                                 int is_https, const char *path,
                                 char *location, size_t location_size)
{
    char line[4096];
    Buf request;
    int ok;

    buf_init(&request);
    buf_appendf(&request, "GET %s HTTP/1.1\r\n", path);
    if ((is_https && port != 443) || (!is_https && port != 80))
        buf_appendf(&request, "Host: %s:%d\r\n", host, port);
    else
        buf_appendf(&request, "Host: %s\r\n", host);
    buf_appendf(&request, "User-Agent: %s\r\n", USER_AGENT);
    /* Icy-MetaData is deliberately not sent: the app has no metadata display, and
     * asking for it would interleave title blocks into the audio bitstream that
     * the decoder would then have to strip. */
    buf_append_str(&request, "Accept: */*\r\n");
    /* Keep-alive is the point of the pool. HTTP/1.1 defaults to it, but saying
     * so explicitly keeps intermediaries from deciding otherwise. */
    buf_append_str(&request, "Connection: keep-alive\r\n\r\n");

    ok = conn_write(response->conn, request.data, request.len) == 0;
    buf_free(&request);
    if (!ok)
        return -1;

    if (conn_read_line(response->conn, line, sizeof(line)) != 0)
        return -1;
    response->status = parse_status_line(line);
    if (response->status < 100)
        return -1;

    /* HTTP/1.0 has no persistent connections unless it opts in. */
    response->keep_alive = strncmp(line, "HTTP/1.0", 8) != 0;

    while (conn_read_line(response->conn, line, sizeof(line)) == 0 && line[0] != '\0') {
        const char *value;

        if (header_is(line, "Content-Type", &value))
            snprintf(response->content_type, sizeof(response->content_type), "%s", value);
        else if (header_is(line, "Content-Length", &value))
            response->content_length = strtol(value, NULL, 10);
        else if (header_is(line, "Transfer-Encoding", &value))
            response->chunked = strcasestr(value, "chunked") != NULL;
        else if (header_is(line, "Location", &value))
            snprintf(location, location_size, "%s", value);
        else if (header_is(line, "Connection", &value)) {
            if (strcasestr(value, "close"))
                response->keep_alive = 0;
            else if (strcasestr(value, "keep-alive"))
                response->keep_alive = 1;
        }
    }

    /* Without a delimiter the body ends when the connection does, so there is
     * nothing to hand back to the pool afterwards. */
    if (!response->chunked && response->content_length < 0)
        response->keep_alive = 0;
    return 0;
}

/* Sends the request and reads the response headers, on a pooled connection when
 * there is one. `location` receives the redirect target for a 3xx.
 *
 * A pooled connection can have been closed by the server while it sat idle, and
 * there is no way to find out except by using it - so a failure on a reused
 * connection is retried once on a fresh one. A failure on a fresh connection is
 * a real failure. */
static int http_request(HttpResponse *response, const char *url, int read_timeout,
                        char *location, size_t location_size,
                        char *error, size_t error_size)
{
    int is_https, port, attempt;
    char host[256], path[2048];

    if (net_parse_url(url, &is_https, host, sizeof(host), &port, path, sizeof(path)) != 0) {
        snprintf(error, error_size, "not an http(s) URL: %s", url);
        return -1;
    }

    for (attempt = 0; attempt < 2; attempt++) {
        int from_pool;

        memset(response, 0, sizeof(*response));
        response->content_length = -1;
        response->chunk_remaining = -1;
        location[0] = '\0';

        response->conn = (attempt == 0) ? pool_take(host, port, is_https) : NULL;
        from_pool = (response->conn != NULL);

        if (!response->conn) {
            response->conn = conn_open(host, port, is_https, read_timeout,
                                       error, error_size);
            if (!response->conn)
                return -1;
        }

        if (send_and_read_headers(response, host, port, is_https, path,
                                  location, location_size) == 0)
            return 0;

        conn_free(response->conn);
        response->conn = NULL;
        if (!from_pool) {
            snprintf(error, error_size, "no usable response from %s", host);
            return -1;
        }
        /* The pooled connection was stale; go around once with a fresh one. */
        pthread_mutex_lock(&stats_lock);
        stats.stale++;
        pthread_mutex_unlock(&stats_lock);
    }
    snprintf(error, error_size, "no usable response from %s", host);
    return -1;
}

/* Reads and discards whatever is left of the body, so the connection is back at
 * a message boundary and can be pooled. Returns 0 if it reached the end. */
static long http_body_read(HttpResponse *response, void *out, size_t len);

static int drain_body(HttpResponse *response)
{
    char scratch[4096];
    long total = 0;

    for (;;) {
        long got = http_body_read(response, scratch, sizeof(scratch));
        if (got < 0)
            return -1;
        if (got == 0)
            return 0;
        /* A redirect or error body should be small; anything big is not worth
         * draining just to save a handshake. */
        total += got;
        if (total > 256 * 1024)
            return -1;
    }
}

/* Ends a response: pools the connection if it can be, closes it otherwise. */
static void response_done(HttpResponse *response, int body_complete)
{
    if (!response->conn)
        return;
    if (response->keep_alive && body_complete)
        pool_put(response->conn);
    else
        conn_free(response->conn);
    response->conn = NULL;
}

/* Reads up to `len` body bytes, unwrapping chunked framing when present.
 * Returns bytes read, 0 at end of body, -1 on error. */
static long http_body_read(HttpResponse *response, void *out, size_t len)
{
    if (!response->chunked) {
        if (response->content_length >= 0) {
            long remaining = response->content_length - response->body_read;
            if (remaining <= 0)
                return 0;
            if ((long)len > remaining)
                len = (size_t)remaining;
        }
        {
            long got = conn_read(response->conn, out, len);
            if (got > 0)
                response->body_read += got;
            return got;
        }
    }

    if (response->chunk_done)
        return 0;

    if (response->chunk_remaining <= 0) {
        char line[64];

        /* Every chunk after the first is preceded by the CRLF that ended the
         * previous one. */
        if (response->chunk_remaining == 0 &&
            conn_read_line(response->conn, line, sizeof(line)) != 0)
            return -1;
        if (conn_read_line(response->conn, line, sizeof(line)) != 0)
            return -1;
        response->chunk_remaining = strtol(line, NULL, 16);
        if (response->chunk_remaining <= 0) {
            response->chunk_done = 1;
            return 0;
        }
    }

    if ((long)len > response->chunk_remaining)
        len = (size_t)response->chunk_remaining;
    {
        long got = conn_read(response->conn, out, len);
        if (got > 0) {
            response->chunk_remaining -= got;
            response->body_read += got;
        }
        return got;
    }
}

static int is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

/* Opens `url`, following redirects. On success the response's body is ready to
 * read and `final_url` holds the URL that actually answered. */
static int http_open(HttpResponse *response, const char *url, int read_timeout,
                     char *final_url, size_t final_url_size,
                     char *error, size_t error_size)
{
    char current[2048];
    char location[2048];
    int hop;

    snprintf(current, sizeof(current), "%s", url);

    for (hop = 0; hop <= MAX_REDIRECTS; hop++) {
        if (http_request(response, current, read_timeout, location, sizeof(location),
                         error, error_size) != 0)
            return -1;

        if (!is_redirect(response->status) || location[0] == '\0') {
            if (response->status < 200 || response->status >= 300) {
                snprintf(error, error_size, "HTTP status %d for %s",
                         response->status, current);
                /* An error body is usually a page of HTML; draining it is worth
                 * it because the next attempt goes to the same host. */
                response_done(response, drain_body(response) == 0);
                return -1;
            }
            snprintf(final_url, final_url_size, "%s", current);
            return 0;
        }

        /* A 3xx normally carries a short body. Reading it leaves the connection
         * at a message boundary so the next hop - very often the same host -
         * can go straight down it. */
        response_done(response, drain_body(response) == 0);
        {
            /* A Location may be relative; the playlist resolver already handles
             * every shape one can take. */
            char *resolved = m3u8_resolve_url(current, location);
            if (!resolved) {
                snprintf(error, error_size, "out of memory following a redirect");
                return -1;
            }
            snprintf(current, sizeof(current), "%s", resolved);
            free(resolved);
        }
    }

    snprintf(error, error_size, "too many redirects starting at %s", url);
    return -1;
}

static int get_body(const char *url, Buf *out, size_t limit, int read_timeout,
                    char *error, size_t error_size)
{
    HttpResponse response;
    char final_url[2048];
    char chunk[16 * 1024];

    if (http_open(&response, url, read_timeout, final_url, sizeof(final_url),
                  error, error_size) != 0)
        return -1;

    for (;;) {
        long got = http_body_read(&response, chunk, sizeof(chunk));
        if (got < 0) {
            snprintf(error, error_size, "%s: read failed", url);
            response_done(&response, 0);
            return -1;
        }
        if (got == 0)
            break;
        if (out->len + (size_t)got > limit) {
            snprintf(error, error_size, "%s: response exceeds %luMB", url,
                     (unsigned long)(limit / (1024 * 1024)));
            response_done(&response, 0);
            return -1;
        }
        if (buf_append(out, chunk, (size_t)got) != 0) {
            snprintf(error, error_size, "out of memory reading %s", url);
            response_done(&response, 0);
            return -1;
        }
    }
    /* Read to the end of a delimited body, so this one goes back in the pool -
     * which is what makes an HLS playlist poll and the segment fetch that
     * follows it share a connection. */
    response_done(&response, 1);

    if (out->len == 0) {
        snprintf(error, error_size, "empty response body from %s", url);
        return -1;
    }
    return 0;
}

int net_get_text(const char *url, Buf *out, char *error, size_t error_size)
{
    return get_body(url, out, TEXT_LIMIT, TEXT_READ_TIMEOUT, error, error_size);
}

int net_get_bytes(const char *url, Buf *out, char *error, size_t error_size)
{
    return get_body(url, out, BYTES_LIMIT, BYTES_READ_TIMEOUT, error, error_size);
}

/* ---------------------------------------------------------- stream worker - */

typedef struct {
    HttpResponse  response;
    ByteQueue    *queue;
    StopFlag     *stop;
} StreamWorker;

static void *stream_worker(void *argument)
{
    StreamWorker *worker = (StreamWorker *)argument;
    char *chunk = (char *)malloc(STREAM_CHUNK);

    if (!chunk) {
        bytequeue_close(worker->queue, "out of memory");
        goto done;
    }

    for (;;) {
        long got;

        if (stopflag_is_set(worker->stop)) {
            bytequeue_close(worker->queue, NULL);
            break;
        }
        got = http_body_read(&worker->response, chunk, STREAM_CHUNK);
        if (got == 0) {
            bytequeue_close(worker->queue, NULL); /* server closed the stream */
            break;
        }
        if (got < 0) {
            /* A stop closes the queue first, so the read error that inevitably
             * follows one must not be reported as a failure. */
            if (stopflag_is_set(worker->stop) || bytequeue_is_closed(worker->queue))
                bytequeue_close(worker->queue, NULL);
            else
                bytequeue_close(worker->queue, "stream read failed");
            break;
        }
        if (!bytequeue_push(worker->queue, chunk, (size_t)got))
            break; /* consumer went away */
    }

done:
    free(chunk);
    /* Never pooled: a progressive stream's body ends only when the connection
     * does, so there is no message boundary to hand back. */
    conn_free(worker->response.conn);
    worker->response.conn = NULL;
    bytequeue_release(worker->queue);
    stopflag_release(worker->stop);
    free(worker);
    return NULL;
}

int net_open_stream(const char *url, StopFlag *stop, NetStream *out,
                    char *error, size_t error_size)
{
    StreamWorker *worker;
    pthread_t thread;
    pthread_attr_t attributes;

    memset(out, 0, sizeof(*out));

    worker = (StreamWorker *)calloc(1, sizeof(*worker));
    if (!worker) {
        snprintf(error, error_size, "out of memory");
        return -1;
    }

    if (http_open(&worker->response, url, BYTES_READ_TIMEOUT,
                  out->final_url, sizeof(out->final_url), error, error_size) != 0) {
        free(worker);
        return -1;
    }
    snprintf(out->content_type, sizeof(out->content_type), "%s",
             worker->response.content_type);

    out->queue = bytequeue_new();
    if (!out->queue) {
        snprintf(error, error_size, "out of memory");
        conn_free(worker->response.conn);
        free(worker);
        return -1;
    }
    worker->queue = bytequeue_retain(out->queue);
    worker->stop = stopflag_retain(stop);

    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    /* 256 kB is well above what this thread needs and well below the 8 MB
     * default, which matters when several sessions have overlapped on a machine
     * with 256 MB of RAM. */
    pthread_attr_setstacksize(&attributes, 256 * 1024);
    if (pthread_create(&thread, &attributes, stream_worker, worker) != 0) {
        pthread_attr_destroy(&attributes);
        snprintf(error, error_size, "could not start the stream thread");
        bytequeue_release(worker->queue);
        stopflag_release(worker->stop);
        bytequeue_release(out->queue);
        out->queue = NULL;
        conn_free(worker->response.conn);
        free(worker);
        return -1;
    }
    pthread_attr_destroy(&attributes);
    return 0;
}
