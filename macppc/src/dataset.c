#include "dataset.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define RWR_MAGIC        0x52575244u /* 'RWRD' */
#define RWR_VERSION      1u
#define HEADER_SIZE      64u
#define COUNTRY_SIZE     12u
#define STATION_SIZE     24u
#define NO_SMALL_STRING  0xFFFFu

struct Dataset {
    const unsigned char *base;
    size_t               size;

    const unsigned char *countries;
    const unsigned char *stations;
    const unsigned char *smalltab;
    const char          *strings;
    size_t               strings_len;

    unsigned country_count;
    unsigned station_count;
    unsigned small_count;

    char path[1024];
};

static unsigned be32(const unsigned char *p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
           ((unsigned)p[2] << 8)  |  (unsigned)p[3];
}

static unsigned be16(const unsigned char *p)
{
    return ((unsigned)p[0] << 8) | (unsigned)p[1];
}

/* IEEE-754 single from four big-endian bytes. Done through a union rather than
 * a pointer cast so it stays defined under -O3's strict aliasing. */
static float be_float(const unsigned char *p)
{
    union { unsigned bits; float value; } cast;
    cast.bits = be32(p);
    return cast.value;
}

/* Every string offset out of the file is bounds-checked before use: the app
 * mmap's a file it did not write in this process, and a truncated or corrupt
 * one must not turn into an out-of-bounds read. */
static const char *string_at(const Dataset *dataset, unsigned offset)
{
    if (offset >= dataset->strings_len)
        return "";
    return dataset->strings + offset;
}

static void fail(char *error, size_t error_size, const char *format, ...)
{
    va_list args;
    if (!error || error_size == 0)
        return;
    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
}

Dataset *dataset_open(const char *path, char *error, size_t error_size)
{
    int fd;
    struct stat info;
    void *mapping;
    const unsigned char *base;
    Dataset *dataset;
    unsigned countries_off, stations_off, smalltab_off, strings_off, total;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        fail(error, error_size, "%s: %s", path, strerror(errno));
        return NULL;
    }
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        fail(error, error_size, "%s: not a regular file", path);
        close(fd);
        return NULL;
    }
    if ((size_t)info.st_size < HEADER_SIZE) {
        fail(error, error_size, "%s: too small to be a dataset", path);
        close(fd);
        return NULL;
    }

    mapping = mmap(NULL, (size_t)info.st_size, PROT_READ, MAP_FILE | MAP_PRIVATE, fd, 0);
    /* The mapping keeps the file alive; the descriptor is not needed after this. */
    close(fd);
    if (mapping == MAP_FAILED) {
        fail(error, error_size, "%s: could not map: %s", path, strerror(errno));
        return NULL;
    }

    base = (const unsigned char *)mapping;
    if (be32(base) != RWR_MAGIC) {
        fail(error, error_size, "%s: not a stations.dat (bad magic)", path);
        munmap(mapping, (size_t)info.st_size);
        return NULL;
    }
    if (be32(base + 4) != RWR_VERSION) {
        fail(error, error_size, "%s: dataset version %u, this build reads %u",
             path, be32(base + 4), RWR_VERSION);
        munmap(mapping, (size_t)info.st_size);
        return NULL;
    }

    dataset = (Dataset *)calloc(1, sizeof(*dataset));
    if (!dataset) {
        fail(error, error_size, "out of memory");
        munmap(mapping, (size_t)info.st_size);
        return NULL;
    }

    dataset->base = base;
    dataset->size = (size_t)info.st_size;
    dataset->country_count = be32(base + 8);
    dataset->station_count = be32(base + 12);
    dataset->small_count   = be32(base + 16);
    countries_off = be32(base + 20);
    stations_off  = be32(base + 24);
    smalltab_off  = be32(base + 28);
    strings_off   = be32(base + 32);
    dataset->strings_len = be32(base + 36);
    total = be32(base + 40);

    /* One consistency pass so nothing downstream has to re-check: every section
     * must lie inside the mapping, and the header's own total must match the
     * file. A file that fails this is truncated or from a different build. */
    if (total != dataset->size ||
        countries_off < HEADER_SIZE ||
        (size_t)countries_off + (size_t)COUNTRY_SIZE * dataset->country_count > stations_off ||
        (size_t)stations_off  + (size_t)STATION_SIZE * dataset->station_count > smalltab_off ||
        (size_t)smalltab_off  + (size_t)4 * dataset->small_count > strings_off ||
        (size_t)strings_off   + dataset->strings_len != dataset->size ||
        dataset->strings_len == 0 ||
        base[strings_off + dataset->strings_len - 1] != '\0') {
        fail(error, error_size, "%s: dataset is truncated or malformed", path);
        free(dataset);
        munmap(mapping, (size_t)info.st_size);
        return NULL;
    }

    dataset->countries   = base + countries_off;
    dataset->stations    = base + stations_off;
    dataset->smalltab    = base + smalltab_off;
    dataset->strings     = (const char *)(base + strings_off);
    snprintf(dataset->path, sizeof(dataset->path), "%s", path);
    return dataset;
}

void dataset_close(Dataset *dataset)
{
    if (!dataset)
        return;
    munmap((void *)dataset->base, dataset->size);
    free(dataset);
}

/* Every accessor tolerates a NULL dataset. dataset_open failing is a normal
 * outcome - a missing or unreadable stations.dat is reported in the status row
 * rather than stopping the app - so the UI keeps running with no dataset at all,
 * and the alternative would be a NULL check at every call site. */
const char *dataset_path(const Dataset *dataset)
{
    return dataset ? dataset->path : "";
}

int dataset_country_count(const Dataset *dataset)
{
    return dataset ? (int)dataset->country_count : 0;
}

int dataset_total_station_count(const Dataset *dataset)
{
    return dataset ? (int)dataset->station_count : 0;
}

const char *dataset_country_name(const Dataset *dataset, int index)
{
    if (!dataset || index < 0 || (unsigned)index >= dataset->country_count)
        return "";
    return string_at(dataset, be32(dataset->countries + (size_t)index * COUNTRY_SIZE));
}

int dataset_station_count(const Dataset *dataset, int country_index)
{
    if (!dataset || country_index < 0 || (unsigned)country_index >= dataset->country_count)
        return 0;
    return (int)be32(dataset->countries + (size_t)country_index * COUNTRY_SIZE + 8);
}

/* Interned codec/language value, or "" for the no-value sentinel. */
static const char *small_string(const Dataset *dataset, unsigned id)
{
    if (id == NO_SMALL_STRING || id >= dataset->small_count)
        return "";
    return string_at(dataset, be32(dataset->smalltab + (size_t)id * 4));
}

int dataset_station(const Dataset *dataset, int country_index, int station_index,
                    Station *out)
{
    const unsigned char *country, *record;
    unsigned first, count;

    if (!dataset || country_index < 0 || (unsigned)country_index >= dataset->country_count)
        return -1;
    country = dataset->countries + (size_t)country_index * COUNTRY_SIZE;
    first = be32(country + 4);
    count = be32(country + 8);
    if (station_index < 0 || (unsigned)station_index >= count)
        return -1;
    if (first + (unsigned)station_index >= dataset->station_count)
        return -1;

    record = dataset->stations + ((size_t)first + (size_t)station_index) * STATION_SIZE;
    out->name     = string_at(dataset, be32(record));
    out->url      = string_at(dataset, be32(record + 4));
    out->language = small_string(dataset, be16(record + 8));
    out->codec    = small_string(dataset, be16(record + 10));
    out->bitrate  = (int)be16(record + 12);
    out->flags    = be16(record + 14);
    out->lat      = be_float(record + 16);
    out->lon      = be_float(record + 20);
    out->country  = string_at(dataset, be32(country));
    return 0;
}

char *station_codec_bitrate_label(const Station *station, char *out, size_t size)
{
    /* radio-browser writes the literal "UNKNOWN" for stations whose codec it
     * could not determine; showing that placeholder to the user is worse than
     * showing nothing. */
    int have_codec = station->codec[0] != '\0' && strcmp(station->codec, "UNKNOWN") != 0;

    if (have_codec && station->bitrate > 0)
        snprintf(out, size, "%s %dkbps", station->codec, station->bitrate);
    else if (have_codec)
        snprintf(out, size, "%s", station->codec);
    else if (station->bitrate > 0)
        snprintf(out, size, "%dkbps", station->bitrate);
    else if (size > 0)
        out[0] = '\0';
    return out;
}

char *station_details(const Station *station, char *out, size_t size)
{
    char label[128];
    char second[256];

    station_codec_bitrate_label(station, label, sizeof(label));

    second[0] = '\0';
    if (label[0] && station->language[0])
        snprintf(second, sizeof(second), "%s - %s", label, station->language);
    else if (label[0])
        snprintf(second, sizeof(second), "%s", label);
    else if (station->language[0])
        snprintf(second, sizeof(second), "%s", station->language);

    if (station->flags & RWR_STATION_HAS_LOCATION) {
        snprintf(out, size, "%s%s%s\n%s (%.3f, %.3f)\n%s",
                 station->name, second[0] ? "\n" : "", second,
                 station->country, (double)station->lat, (double)station->lon,
                 station->url);
    } else if (station->country[0]) {
        snprintf(out, size, "%s%s%s\n%s\n%s",
                 station->name, second[0] ? "\n" : "", second,
                 station->country, station->url);
    } else {
        snprintf(out, size, "%s%s%s\n%s",
                 station->name, second[0] ? "\n" : "", second, station->url);
    }
    return out;
}
