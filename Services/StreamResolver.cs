using System;
using System.IO;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace RWorldRadio.Services
{
    /// <summary>
    /// Works out what a station's URL actually serves before handing it to
    /// MediaPlayer: follows HTTP redirects and playlist indirection to the URL
    /// that really carries audio.
    ///
    /// MediaPlayer handles MP3, AAC and HLS on its own, but it will not follow a
    /// .m3u/.pls pointer, and it cannot tell that an extensionless URL 302s to a
    /// CDN-signed .m3u8. Both are common: one station in the bundled list redirects
    /// to "...m3u8?Policy=...", and several .m3u entries return a playlist that
    /// points at the real stream.
    ///
    /// Ported from the Linux/macOS build's stream resolver. The invariants below
    /// were each found by a real station failing, so keep them:
    ///
    ///  * Decide from the response BODY, not the URL. Plenty of .m3u/.pls URLs in
    ///    the list serve audio directly despite the extension, and must be left
    ///    alone rather than followed.
    ///  * A body starting with "http" is a playlist too - one station returns a
    ///    bare URL with content type application/octet-stream, and that is the only
    ///    tell.
    ///  * Read only the first bytes. A live radio stream never ends, so reading the
    ///    whole body would hang forever.
    /// </summary>
    public sealed class StreamResolver
    {
        // A playlist is a few hundred bytes; this is only ever a prefix of a
        // stream, never a whole one.
        private const int PeekBytes = 1024;

        // More than two hops means something is looping.
        private const int MaxHops = 2;

        private readonly HttpClient _http;

        public StreamResolver()
        {
            // No automatic redirect following: where a URL redirects TO is part of
            // what this class reports, and the final URL is needed to resolve a
            // playlist's relative entries.
            var handler = new HttpClientHandler { AllowAutoRedirect = false };
            _http = new HttpClient(handler);
            _http.DefaultRequestHeaders.UserAgent.ParseAdd("RWorldRadio/1.0");
        }

        public sealed class Result
        {
            /// <summary>The URL to hand to MediaPlayer.</summary>
            public string Url { get; set; }

            /// <summary>
            /// True when that URL is an HLS playlist. MediaPlayer needs
            /// AdaptiveMediaSource for those rather than a plain MediaSource.
            /// </summary>
            public bool IsHls { get; set; }
        }

        /// <summary>
        /// Follows redirects and playlists. Fails safe: on any error the input URL
        /// is returned unchanged, so a resolver problem is never worse than not
        /// having one.
        /// </summary>
        public async Task<Result> ResolveAsync(string startUrl, CancellationToken cancel)
        {
            var url = startUrl;

            for (var hop = 0; hop <= MaxHops; hop++)
            {
                if (IsHlsUrl(url))
                {
                    return new Result { Url = url, IsHls = true };
                }

                HttpResponseMessage response;
                try
                {
                    var request = new HttpRequestMessage(HttpMethod.Get, url);
                    response = await _http.SendAsync(
                        request, HttpCompletionOption.ResponseHeadersRead, cancel)
                        .ConfigureAwait(false);
                }
                catch (Exception)
                {
                    return new Result { Url = url, IsHls = false };
                }

                using (response)
                {
                    var status = (int)response.StatusCode;
                    if (status >= 300 && status < 400)
                    {
                        var location = response.Headers.Location;
                        if (location == null)
                        {
                            return new Result { Url = url, IsHls = false };
                        }
                        // Relative Location headers are legal.
                        url = location.IsAbsoluteUri
                            ? location.ToString()
                            : new Uri(new Uri(url), location).ToString();
                        continue;
                    }

                    var contentType = response.Content.Headers.ContentType?.MediaType;
                    var head = await ReadPrefixAsync(response, cancel).ConfigureAwait(false);

                    if (!LooksLikePlaylist(head, contentType))
                    {
                        // This URL serves audio.
                        return new Result { Url = url, IsHls = false };
                    }

                    if (hop == MaxHops)
                    {
                        return new Result { Url = url, IsHls = false };
                    }

                    // A playlist. HLS goes straight to MediaPlayer's adaptive
                    // source; a legacy .m3u/.pls is a pointer to follow.
                    var text = await ReadTextAsync(url, cancel).ConfigureAwait(false);
                    if (text == null)
                    {
                        return new Result { Url = url, IsHls = false };
                    }

                    if (text.IndexOf("#EXT-X-", StringComparison.Ordinal) >= 0)
                    {
                        return new Result { Url = url, IsHls = true };
                    }

                    var next = FirstStreamUrl(text);
                    if (string.IsNullOrEmpty(next))
                    {
                        return new Result { Url = url, IsHls = false };
                    }
                    url = next;
                }
            }

            return new Result { Url = url, IsHls = false };
        }

        /// <summary>True if the URL's path ends in ".m3u8" (query ignored).</summary>
        public static bool IsHlsUrl(string url)
        {
            if (string.IsNullOrEmpty(url)) return false;
            var path = url;
            var query = path.IndexOf('?');
            if (query >= 0) path = path.Substring(0, query);
            return path.EndsWith(".m3u8", StringComparison.OrdinalIgnoreCase);
        }

        /// <summary>
        /// True if this response body is a playlist rather than audio.
        /// </summary>
        public static bool LooksLikePlaylist(byte[] head, string contentType)
        {
            if (head != null && head.Length > 0)
            {
                var prefix = Encoding.ASCII.GetString(
                    head, 0, Math.Min(head.Length, 16));

                if (prefix.StartsWith("#EXTM3U", StringComparison.Ordinal) ||
                    prefix.StartsWith("#EXT-X", StringComparison.Ordinal))
                {
                    return true;
                }
                if (prefix.StartsWith("[playlist]", StringComparison.OrdinalIgnoreCase))
                {
                    return true;
                }
                // A bare URL list with no header at all, served as
                // application/octet-stream. No audio bitstream starts with "http".
                if (prefix.StartsWith("http://", StringComparison.OrdinalIgnoreCase) ||
                    prefix.StartsWith("https://", StringComparison.OrdinalIgnoreCase))
                {
                    return true;
                }
            }

            if (string.IsNullOrEmpty(contentType)) return false;
            switch (contentType.ToLowerInvariant())
            {
                case "application/vnd.apple.mpegurl":
                case "application/x-mpegurl":
                case "audio/x-mpegurl":
                case "audio/mpegurl":
                case "audio/x-scpls":
                case "audio/scpls":
                    return true;
                default:
                    return false;
            }
        }

        /// <summary>
        /// First playable URL in a legacy .m3u (bare URL lines) or .pls
        /// ("FileN=&lt;url&gt;") playlist.
        /// </summary>
        public static string FirstStreamUrl(string text)
        {
            if (string.IsNullOrEmpty(text)) return null;

            foreach (var rawLine in text.Split('\n'))
            {
                var line = rawLine.Trim();
                if (line.Length == 0) continue;

                var candidate = line;
                var equals = line.IndexOf('=');
                if (equals >= 0)
                {
                    // .pls: only FileN= lines carry a URL. Title/Length do not.
                    var key = line.Substring(0, equals).Trim();
                    if (key.StartsWith("file", StringComparison.OrdinalIgnoreCase))
                    {
                        candidate = line.Substring(equals + 1).Trim();
                    }
                }
                else if (line[0] == '#')
                {
                    continue; // an .m3u comment
                }

                if (candidate.StartsWith("http://", StringComparison.OrdinalIgnoreCase) ||
                    candidate.StartsWith("https://", StringComparison.OrdinalIgnoreCase))
                {
                    return candidate;
                }
            }
            return null;
        }

        /// <summary>
        /// Reads at most PeekBytes of the body. Never reads to the end - the body
        /// may be an endless stream.
        /// </summary>
        private static async Task<byte[]> ReadPrefixAsync(
            HttpResponseMessage response, CancellationToken cancel)
        {
            try
            {
                using (var stream = await response.Content.ReadAsStreamAsync()
                    .ConfigureAwait(false))
                {
                    var buffer = new byte[PeekBytes];
                    var filled = 0;
                    while (filled < buffer.Length)
                    {
                        var read = await stream
                            .ReadAsync(buffer, filled, buffer.Length - filled, cancel)
                            .ConfigureAwait(false);
                        if (read <= 0) break;
                        filled += read;
                    }
                    var result = new byte[filled];
                    Array.Copy(buffer, result, filled);
                    return result;
                }
            }
            catch (Exception)
            {
                return new byte[0];
            }
        }

        /// <summary>Fetches a whole playlist; they are small.</summary>
        private async Task<string> ReadTextAsync(string url, CancellationToken cancel)
        {
            try
            {
                var request = new HttpRequestMessage(HttpMethod.Get, url);
                using (var response = await _http
                    .SendAsync(request, HttpCompletionOption.ResponseContentRead, cancel)
                    .ConfigureAwait(false))
                {
                    if (!response.IsSuccessStatusCode) return null;
                    return await response.Content.ReadAsStringAsync().ConfigureAwait(false);
                }
            }
            catch (Exception)
            {
                return null;
            }
        }
    }
}
