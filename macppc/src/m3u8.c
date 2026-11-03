#include "m3u8.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"

static char *dup_range(const char *start, size_t len)
{
    char *out = (char *)malloc(len + 1);
    if (!out)
        return NULL;
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

static char *dup_str(const char *text)
{
    return dup_range(text, strlen(text));
}

char *m3u8_resolve_url(const char *base_url, const char *uri)
{
    const char *scheme_end, *host_start, *path_start, *query, *last_slash;
    Buf out;

    if (strstr(uri, "://"))
        return dup_str(uri);

    scheme_end = strstr(base_url, "://");
    if (!scheme_end)
        return dup_str(uri); /* base isn't a URL we can resolve against */

    buf_init(&out);

    /* Protocol-relative has to be checked before the host-absolute case below:
     * otherwise "//host/path" would be pasted onto the base's own origin and
     * produce a bogus double-slash URL. (The Haiku original got this wrong;
     * qingting.fm's playlists use this form.) */
    if (uri[0] == '/' && uri[1] == '/') {
        buf_append(&out, base_url, (size_t)(scheme_end - base_url));
        buf_append_str(&out, "://");
        buf_append_str(&out, uri + 2);
        return buf_release(&out);
    }

    host_start = scheme_end + 3;
    path_start = strchr(host_start, '/');

    if (uri[0] == '/') {
        size_t origin_len = path_start ? (size_t)(path_start - base_url) : strlen(base_url);
        buf_append(&out, base_url, origin_len);
        buf_append_str(&out, uri);
        return buf_release(&out);
    }

    if (!path_start) {
        /* Base is bare origin ("https://host.example"): the URI is its first
         * path component. */
        buf_append_str(&out, base_url);
        buf_append_str(&out, "/");
        buf_append_str(&out, uri);
        return buf_release(&out);
    }

    /* Strip any query string before taking the directory, otherwise a
     * "?token=a/b" containing a slash would be treated as part of the path. */
    query = strchr(base_url, '?');
    last_slash = NULL;
    {
        const char *scan;
        const char *end = query ? query : base_url + strlen(base_url);
        for (scan = base_url; scan < end; scan++) {
            if (*scan == '/')
                last_slash = scan;
        }
    }
    buf_append(&out, base_url, (size_t)(last_slash - base_url) + 1);
    buf_append_str(&out, uri);
    return buf_release(&out);
}

int m3u8_is_master_playlist(const char *text)
{
    return strstr(text, "#EXT-X-STREAM-INF") != NULL;
}

/* atoi/atof semantics: take as much of a leading number as there is and ignore
 * the rest, which is what these playlist fields need ("#EXTINF:6.4, no desc",
 * "BANDWIDTH=374000,CODECS=..."). */
static long parse_leading_long(const char *text)
{
    while (*text == ' ' || *text == '\t')
        text++;
    return strtol(text, NULL, 10);
}

static double parse_leading_double(const char *text)
{
    while (*text == ' ' || *text == '\t')
        text++;
    return strtod(text, NULL);
}

/* Reads an integer attribute out of an "#EXT-X-..." line, matching either the
 * first attribute (":KEY=") or a later one (",KEY="). */
static long extract_int_attribute(const char *line, const char *key)
{
    char pattern[64];
    const char *found;

    snprintf(pattern, sizeof(pattern), ":%s=", key);
    found = strstr(line, pattern);
    if (!found) {
        snprintf(pattern, sizeof(pattern), ",%s=", key);
        found = strstr(line, pattern);
        if (!found)
            return 0;
    }
    return parse_leading_long(found + strlen(pattern));
}

/* Splits `text` into lines with trailing whitespace trimmed, the way Rust's
 * `str::lines()` plus `trim_end()` does. The returned array and its strings are
 * one allocation each; free with free_lines(). */
static char **split_lines(const char *text, size_t *out_count)
{
    size_t count = 0, capacity = 64, index;
    char **lines = (char **)malloc(capacity * sizeof(*lines));
    const char *cursor = text;

    if (!lines)
        return NULL;

    for (;;) {
        const char *newline = strchr(cursor, '\n');
        size_t len = newline ? (size_t)(newline - cursor) : strlen(cursor);
        char *line;

        while (len > 0 && (cursor[len - 1] == '\r' || cursor[len - 1] == ' ' ||
                           cursor[len - 1] == '\t'))
            len--;

        if (count == capacity) {
            char **grown = (char **)realloc(lines, capacity * 2 * sizeof(*lines));
            if (!grown)
                goto out_of_memory;
            lines = grown;
            capacity *= 2;
        }
        line = dup_range(cursor, len);
        if (!line)
            goto out_of_memory;
        lines[count++] = line;

        if (!newline)
            break;
        cursor = newline + 1;
    }

    *out_count = count;
    return lines;

out_of_memory:
    for (index = 0; index < count; index++)
        free(lines[index]);
    free(lines);
    return NULL;
}

static void free_lines(char **lines, size_t count)
{
    size_t index;
    for (index = 0; index < count; index++)
        free(lines[index]);
    free(lines);
}

static int has_prefix(const char *text, const char *prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

int m3u8_parse_master(const char *text, const char *base_url,
                      M3u8Variant **out, size_t *out_count)
{
    size_t line_count = 0, index, count = 0, capacity = 8;
    char **lines = split_lines(text, &line_count);
    M3u8Variant *variants;

    *out = NULL;
    *out_count = 0;
    if (!lines)
        return -1;

    variants = (M3u8Variant *)malloc(capacity * sizeof(*variants));
    if (!variants) {
        free_lines(lines, line_count);
        return -1;
    }

    index = 0;
    while (index < line_count) {
        size_t next;
        char *uri;

        if (!has_prefix(lines[index], "#EXT-X-STREAM-INF:")) {
            index++;
            continue;
        }

        /* The URI is the next line that is neither blank nor a tag. */
        next = index + 1;
        while (next < line_count && (lines[next][0] == '\0' || lines[next][0] == '#'))
            next++;
        if (next >= line_count)
            break;

        uri = m3u8_resolve_url(base_url, lines[next]);
        if (!uri)
            break;

        if (count == capacity) {
            M3u8Variant *grown =
                (M3u8Variant *)realloc(variants, capacity * 2 * sizeof(*variants));
            if (!grown) {
                free(uri);
                break;
            }
            variants = grown;
            capacity *= 2;
        }
        variants[count].uri = uri;
        variants[count].bandwidth = extract_int_attribute(lines[index], "BANDWIDTH");
        count++;
        index = next + 1;
    }

    free_lines(lines, line_count);
    *out = variants;
    *out_count = count;
    return 0;
}

void m3u8_free_variants(M3u8Variant *variants, size_t count)
{
    size_t index;
    if (!variants)
        return;
    for (index = 0; index < count; index++)
        free(variants[index].uri);
    free(variants);
}

int m3u8_parse_media(const char *text, const char *base_url, M3u8MediaPlaylist *out)
{
    size_t line_count = 0, index, capacity = 32;
    char **lines;
    double next_duration = 0.0;

    memset(out, 0, sizeof(*out));
    out->target_duration = 6.0; /* MediaPlaylist::default() in the Rust port */

    lines = split_lines(text, &line_count);
    if (!lines)
        return -1;

    out->segments = (M3u8Segment *)malloc(capacity * sizeof(*out->segments));
    if (!out->segments) {
        free_lines(lines, line_count);
        return -1;
    }

    for (index = 0; index < line_count; index++) {
        const char *line = lines[index];

        if (has_prefix(line, "#EXT-X-MEDIA-SEQUENCE:")) {
            out->media_sequence = parse_leading_long(line + 22);
        } else if (has_prefix(line, "#EXT-X-TARGETDURATION:")) {
            out->target_duration = parse_leading_double(line + 22);
        } else if (has_prefix(line, "#EXT-X-ENDLIST")) {
            out->is_end_list = 1;
        } else if (has_prefix(line, "#EXTINF:")) {
            next_duration = parse_leading_double(line + 8);
        } else if (line[0] != '\0' && line[0] != '#') {
            char *uri = m3u8_resolve_url(base_url, line);
            if (!uri)
                break;
            if (out->segment_count == capacity) {
                M3u8Segment *grown = (M3u8Segment *)realloc(
                    out->segments, capacity * 2 * sizeof(*out->segments));
                if (!grown) {
                    free(uri);
                    break;
                }
                out->segments = grown;
                capacity *= 2;
            }
            out->segments[out->segment_count].uri = uri;
            out->segments[out->segment_count].duration = next_duration;
            out->segment_count++;
            next_duration = 0.0;
        }
    }

    free_lines(lines, line_count);
    return 0;
}

void m3u8_free_media(M3u8MediaPlaylist *playlist)
{
    size_t index;
    if (!playlist->segments)
        return;
    for (index = 0; index < playlist->segment_count; index++)
        free(playlist->segments[index].uri);
    free(playlist->segments);
    playlist->segments = NULL;
    playlist->segment_count = 0;
}
