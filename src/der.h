/* Just enough ASN.1 to walk the concatenated-DER CA store that
 * tools/mkcastore.py produces.
 *
 * mbedTLS's own mbedtls_x509_crt_parse() handles a PEM bundle in one call, but
 * on non-PEM input it parses exactly one DER certificate. The store is 151 of
 * them back to back, so net.c has to find each one's boundary itself. That is
 * this file - kept out of net.c so the tests, which cannot link mbedTLS on a
 * modern host, can still check it against the real data/cacert.der. */
#ifndef RWR_DER_H
#define RWR_DER_H

#include <stddef.h>

/* Total length of the DER object at `data`, its header included, or 0 if it does
 * not parse as one that fits inside `len`. */
size_t der_object_len(const unsigned char *data, size_t len);

#endif
