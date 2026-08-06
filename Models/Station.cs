using System;

namespace RWorldRadio.Models
{
    /// <summary>One station record, matching the bundled dataset's JSON shape.</summary>
    public sealed class Station
    {
        public string Name { get; set; }
        public string Url { get; set; }
        public string Codec { get; set; }
        public int Bitrate { get; set; }
        public string Language { get; set; }
        public string Country { get; set; }

        /// <summary>e.g. "MP3 128kbps", "AAC", "128kbps", or "".</summary>
        public string CodecBitrate
        {
            get
            {
                // radio-browser writes the literal "UNKNOWN" when it could not
                // determine the codec; that is not worth showing.
                var text = string.Equals(Codec, "UNKNOWN", StringComparison.OrdinalIgnoreCase)
                    ? string.Empty
                    : (Codec ?? string.Empty);
                if (Bitrate > 0)
                {
                    if (text.Length > 0) text += " ";
                    text += Bitrate + "kbps";
                }
                return text;
            }
        }
    }

    /// <summary>One entry of the dataset index (countries.json).</summary>
    public sealed class CountryEntry
    {
        public string Name { get; set; }
        public string File { get; set; }
        public int Count { get; set; }
    }
}
