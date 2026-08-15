#include "der.h"

size_t der_object_len(const unsigned char *data, size_t len)
{
    size_t header, length, index;

    /* Every certificate in the store is an ASN.1 SEQUENCE. Anything else means
     * the store is corrupt or we have lost our place in it, and the caller stops
     * rather than guessing. */
    if (len < 2 || data[0] != 0x30)
        return 0;

    /* Short form: the length is the low seven bits of the second byte. */
    if ((data[1] & 0x80) == 0)
        return 2 + (size_t)data[1];

    /* Long form: the low seven bits say how many bytes of length follow. Four is
     * the most a certificate could plausibly need and the most that fits in a
     * 32-bit size_t, which is what PPC has. */
    header = (size_t)(data[1] & 0x7F);
    if (header == 0 || header > 4 || len < 2 + header)
        return 0;

    length = 0;
    for (index = 0; index < header; index++)
        length = (length << 8) | data[2 + index];

    if (length > len - 2 - header)
        return 0;
    return 2 + header + length;
}
