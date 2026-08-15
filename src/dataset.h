/* Read side of data/stations.dat - see tools/mkdataset.py for the layout.
 *
 * The whole file is mmap'd read-only and never copied. A Station is a set of
 * pointers into that mapping, so building one is a few loads and no allocation;
 * the strings live as long as the Dataset does.
 *
 * All multi-byte fields are decoded explicitly from big-endian rather than
 * cast-in-place. On the PPC target that compiles to plain loads, and it is what
 * lets tests/test_core.c read the same file on a little-endian host. */
#ifndef RWR_DATASET_H
#define RWR_DATASET_H

#include <stddef.h>

typedef struct Dataset Dataset;

enum {
    RWR_STATION_HAS_LOCATION  = 1 << 0,
    RWR_STATION_NEEDS_RESOLVE = 1 << 1
};

typedef struct {
    const char *name;
    const char *url;      /* already playback_url(): resolved if the dataset had one */
    const char *country;
    const char *codec;    /* "" when the dataset had none */
    const char *language; /* "" when the dataset had none */
    int         bitrate;  /* 0 when unknown */
    unsigned    flags;
    float       lat;
    float       lon;
} Station;

/* Opens and validates the file. Returns NULL and writes a reason into
 * `error` (a buffer of `error_size` bytes) on failure. */
Dataset *dataset_open(const char *path, char *error, size_t error_size);
void     dataset_close(Dataset *dataset);

/* Path the dataset was opened from, for the status line. */
const char *dataset_path(const Dataset *dataset);

int         dataset_country_count(const Dataset *dataset);
const char *dataset_country_name(const Dataset *dataset, int index);
int         dataset_station_count(const Dataset *dataset, int country_index);
int         dataset_total_station_count(const Dataset *dataset);

/* Fills `out` with station `station_index` of `country_index`. Returns 0 on
 * success, -1 if either index is out of range. */
int dataset_station(const Dataset *dataset, int country_index, int station_index,
                    Station *out);

/* "MP3 128kbps", "AAC", "128kbps", or "" - codec_bitrate_label() from the Rust
 * port, including its treatment of the literal "UNKNOWN" as no codec at all.
 * Writes into `out` and returns it. */
char *station_codec_bitrate_label(const Station *station, char *out, size_t size);

/* Multi-line tooltip text - details() from the Rust port. */
char *station_details(const Station *station, char *out, size_t size);

#endif
