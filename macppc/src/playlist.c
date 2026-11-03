#include "playlist.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

static int ends_with_ci(const char *text, const char *suffix)
{
    size_t text_len = strlen(text), suffix_len = strlen(suffix), index;

    if (suffix_len > text_len)
        return 0;
    text += text_len - suffix_len;
    for (index = 0; index < suffix_len; index++) {
        if (tolower((unsigned char)text[index]) != (unsigned char)suffix[index])
            return 0;
    }
    return 1;
}

/* BMediaFile(BUrl) on Haiku only handled a plain progressive HTTP body, not
 * HLS's playlist-of-segments scheme. Stations whose stream is an .m3u8 (common
 * for broadcasters like the BBC, who moved their public endpoints to HLS-only
 * some years ago) go through hls.c instead. Same split here, same detection. */
int playlist_is_hls_url(const char *url)
{
    char path[2048];
    const char *query = strchr(url, '?');
    size_t len = query ? (size_t)(query - url) : strlen(url);

    if (len >= sizeof(path))
        len = sizeof(path) - 1;
    memcpy(path, url, len);
    path[len] = '\0';
    return ends_with_ci(path, ".m3u8");
}

/* True if this response body is a playlist rather than audio.
 *
 * The URL is not a reliable signal: stations very commonly use an extensionless
 * URL that 302s to "...m3u8?Policy=..." (CDN signing), so the only dependable
 * evidence is what came back. */
int playlist_looks_like_playlist(const unsigned char *head, size_t len,
                                 const char *content_type)
{
    char mime[128];
    size_t index = 0;

    if (len >= 7 && memcmp(head, "#EXTM3U", 7) == 0)
        return 1;
    if (len >= 6 && memcmp(head, "#EXT-X", 6) == 0)
        return 1;
    /* SHOUTcast/Winamp .pls */
    if (len >= 10 && strncasecmp((const char *)head, "[playlist]", 10) == 0)
        return 1;
    /* A bare URL list with no header at all: some .m3u endpoints return just the
     * stream URL, with a content type of application/octet-stream (confirmed:
     * streams.br.de/br-klassik_2.m3u). No audio bitstream begins with the ASCII
     * "http", so this is unambiguous. */
    if (len >= 7 && memcmp(head, "http://", 7) == 0)
        return 1;
    if (len >= 8 && memcmp(head, "https://", 8) == 0)
        return 1;

    if (!content_type || !content_type[0])
        return 0;
    while (*content_type == ' ')
        content_type++;
    while (*content_type && *content_type != ';' && index + 1 < sizeof(mime))
        mime[index++] = (char)tolower((unsigned char)*content_type++);
    while (index > 0 && mime[index - 1] == ' ')
        index--;
    mime[index] = '\0';

    return strcmp(mime, "application/vnd.apple.mpegurl") == 0 ||
           strcmp(mime, "application/x-mpegurl") == 0 ||
           strcmp(mime, "audio/x-mpegurl") == 0 ||
           strcmp(mime, "audio/mpegurl") == 0 ||
           strcmp(mime, "audio/x-scpls") == 0 ||
           strcmp(mime, "audio/scpls") == 0;
}

/* First playable URL in a legacy .m3u (bare URL lines) or .pls (FileN=<url>)
 * playlist. Writes into `out` and returns 1 if one was found. */
int playlist_first_stream_url(const char *text, char *out, size_t size)
{
    const char *cursor = text;

    while (*cursor) {
        const char *newline = strchr(cursor, '\n');
        size_t len = newline ? (size_t)(newline - cursor) : strlen(cursor);
        const char *line = cursor, *candidate, *equals;
        size_t line_len = len;

        while (line_len > 0 && isspace((unsigned char)line[0])) { line++; line_len--; }
        while (line_len > 0 && isspace((unsigned char)line[line_len - 1])) line_len--;

        if (line_len == 0)
            goto next_line;

        candidate = line;
        equals = (const char *)memchr(line, '=', line_len);
        if (equals && line_len >= 4 && strncasecmp(line, "file", 4) == 0) {
            /* .pls entries are "File1=http://..."; Title1= and Length1= are
             * skipped by the http check below. */
            size_t offset = (size_t)(equals - line) + 1;
            candidate = line + offset;
            line_len -= offset;
            while (line_len > 0 && isspace((unsigned char)candidate[0])) {
                candidate++;
                line_len--;
            }
        } else if (line[0] == '#') {
            goto next_line; /* a comment in a .m3u */
        }

        if ((line_len > 7 && strncmp(candidate, "http://", 7) == 0) ||
            (line_len > 8 && strncmp(candidate, "https://", 8) == 0)) {
            if (line_len >= size)
                line_len = size - 1;
            memcpy(out, candidate, line_len);
            out[line_len] = '\0';
            return 1;
        }

    next_line:
        if (!newline)
            break;
        cursor = newline + 1;
    }
    return 0;
}

/* Trims a Tune.ashx text response to its first non-empty line, which is just the
 * resolved stream URL. */
int playlist_first_line(const char *text, char *out, size_t size)
{
    const char *cursor = text;

    while (*cursor) {
        const char *newline = strchr(cursor, '\n');
        size_t len = newline ? (size_t)(newline - cursor) : strlen(cursor);
        const char *line = cursor;

        while (len > 0 && isspace((unsigned char)line[0])) { line++; len--; }
        while (len > 0 && isspace((unsigned char)line[len - 1])) len--;
        if (len > 0) {
            if (len >= size)
                len = size - 1;
            memcpy(out, line, len);
            out[len] = '\0';
            return 1;
        }
        if (!newline)
            break;
        cursor = newline + 1;
    }
    return 0;
}
