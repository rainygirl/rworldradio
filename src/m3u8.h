/* Minimal HLS playlist parser - port of the Rust port's m3u8.rs, which was
 * itself a port of the Haiku build's M3u8Parser. Enough to follow a master
 * playlist to one media playlist and to read a live media playlist's segment
 * list; no byte-range segments, no encryption, no discontinuities. */
#ifndef RWR_M3U8_H
#define RWR_M3U8_H

#include <stddef.h>

typedef struct {
    char *uri;       /* malloc'd, already resolved against the playlist URL */
    long  bandwidth;
} M3u8Variant;

typedef struct {
    char  *uri;      /* malloc'd */
    double duration;
} M3u8Segment;

typedef struct {
    M3u8Segment *segments;
    size_t       segment_count;
    long         media_sequence;
    double       target_duration;
    int          is_end_list;
} M3u8MediaPlaylist;

/* Resolves a possibly-relative URI against a base URL, handling the four shapes
 * HLS playlists actually produce: absolute, protocol-relative ("//host/path"),
 * host-absolute ("/path") and playlist-relative ("segment.ts"). Returns a
 * malloc'd string, or NULL if allocation failed. */
char *m3u8_resolve_url(const char *base_url, const char *uri);

int m3u8_is_master_playlist(const char *text);

/* Both parsers return 0 on success. The caller frees with the matching _free. */
int  m3u8_parse_master(const char *text, const char *base_url,
                       M3u8Variant **out, size_t *out_count);
void m3u8_free_variants(M3u8Variant *variants, size_t count);

int  m3u8_parse_media(const char *text, const char *base_url,
                      M3u8MediaPlaylist *out);
void m3u8_free_media(M3u8MediaPlaylist *playlist);

#endif
