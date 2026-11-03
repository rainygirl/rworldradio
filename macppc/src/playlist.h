/* Deciding what a URL or a response body actually is, before handing it to a
 * decoder. Ported from the free functions at the top of the Rust port's
 * player.rs.
 *
 * Kept out of player.c so `make check` can test it: this is where the "the URL
 * said one thing and the server did another" handling lives, and it is the part
 * of the session path most likely to be wrong for a station nobody tried yet. */
#ifndef RWR_PLAYLIST_H
#define RWR_PLAYLIST_H

#include <stddef.h>

/* True for a URL whose path ends in .m3u8, ignoring any query string. Those go
 * through the HLS pipeline rather than being read as a progressive body. */
int playlist_is_hls_url(const char *url);

/* True if this response body is a playlist rather than audio.
 *
 * The URL is not a reliable signal: stations very commonly use an extensionless
 * URL that 302s to "...m3u8?Policy=..." (CDN signing), so the only dependable
 * evidence is what came back. `content_type` may be NULL. */
int playlist_looks_like_playlist(const unsigned char *head, size_t len,
                                 const char *content_type);

/* First playable URL in a legacy .m3u (bare URL lines) or .pls (FileN=<url>)
 * playlist. Returns 1 and fills `out` if one was found. */
int playlist_first_stream_url(const char *text, char *out, size_t size);

/* First non-empty line, trimmed - a TuneIn Tune.ashx response is just the
 * resolved stream URL on a line of its own. */
int playlist_first_line(const char *text, char *out, size_t size);

#endif
