/* mbedTLS build configuration for this port.
 *
 * Passed with -DMBEDTLS_CONFIG_FILE. Everything below is either required to talk
 * to a modern streaming host or removed to keep the static library small - see
 * vendor/README.md.
 *
 * Targets mbedTLS 2.28 (the last branch that builds with GCC 4.0.1).
 */
#ifndef RWR_MBEDTLS_CONFIG_H
#define RWR_MBEDTLS_CONFIG_H

/* ------------------------------------------------------------- platform --- */

#define MBEDTLS_HAVE_ASM
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_C
/* MBEDTLS_NO_PLATFORM_ENTROPY is left undefined so the entropy source is
 * /dev/urandom, which Darwin has. */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_ERROR_C

/* MBEDTLS_THREADING_C is deliberately off. The only shared object across threads
 * is the CTR-DRBG, which src/net.c serializes with its own mutex; enabling
 * mbedTLS's threading layer would add a mutex to every context for no benefit. */

/* ---------------------------------------------------------------- crypto --- */

#define MBEDTLS_AES_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_C
#define MBEDTLS_GCM_C
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C          /* still needed to parse older certificates */
#define MBEDTLS_OID_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_RSA_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C

/* Only the two curves anything in the wild negotiates. Each additional curve
 * costs code and a precomputed table; dropping the rest is most of why this
 * build is under 400 kB. */
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECDSA_DETERMINISTIC
#define MBEDTLS_HMAC_DRBG_C

/* Speed knobs. A window of 6 and the fixed-point optimization roughly halve the
 * cost of a P-256 scalar multiplication, which is the single most expensive
 * thing in a handshake on a G4. They cost about 12 kB of tables. */
#define MBEDTLS_ECP_WINDOW_SIZE          6
#define MBEDTLS_ECP_FIXED_POINT_OPTIM    1

/* ------------------------------------------------------------------ x509 --- */

#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_PEM_PARSE_C    /* so a plain PEM bundle also works, see net.c */

/* ------------------------------------------------------------------- TLS --- */

#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION      /* SNI - mandatory on any CDN */
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH
/* Client-side session tickets. Session-ID resumption alone is not much use in
 * practice - most CDNs are stateless and only offer tickets - and resumption is
 * what turns the handshake that a dropped pooled connection forces back into
 * one cheap round trip. See the session cache in src/net.c. */
#define MBEDTLS_SSL_SESSION_TICKETS
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED

/* Not defined, on purpose: MBEDTLS_SSL_PROTO_DTLS, MBEDTLS_SSL_SRV_C,
 * MBEDTLS_SSL_TICKET_C (that is the server side), MBEDTLS_SSL_RENEGOTIATION,
 * every PSK and DHE key
 * exchange, MBEDTLS_DES_C, MBEDTLS_ARC4_C, MBEDTLS_SSL_PROTO_TLS1 and
 * MBEDTLS_SSL_PROTO_TLS1_1. The old protocol versions in particular would only
 * let the client fall back to exactly what Tiger's own Secure Transport can
 * already do, which is what this library exists to get past. */

/* The default of 16 kB in and 16 kB out per connection is more than a radio
 * stream needs, and the app may have three connections open at once (stream,
 * playlist poll, segment fetch) on a machine with 256 MB. */
#define MBEDTLS_SSL_IN_CONTENT_LEN   16384   /* a server may send full records */
#define MBEDTLS_SSL_OUT_CONTENT_LEN  4096    /* our requests are a few hundred bytes */

#include "mbedtls/check_config.h"

#endif
