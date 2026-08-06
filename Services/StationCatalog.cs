using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Windows.ApplicationModel;
using Windows.Data.Json;
using Windows.Storage;
using RWorldRadio.Models;

namespace RWorldRadio.Services
{
    /// <summary>
    /// Loads the station list that ships inside the package
    /// (Assets/data/countries.json plus Assets/data/countries/&lt;file&gt;.json).
    ///
    /// Two things carried over from the desktop build on purpose:
    ///  * Field access is tolerant. radio-browser occasionally emits a numeric
    ///    field as a string and vice versa, and lat/lon are frequently null. A
    ///    strict deserializer would turn a schema wobble into a whole country
    ///    failing to load.
    ///  * A country file that is missing or corrupt is skipped, not fatal.
    ///
    /// Countries are loaded lazily: the index is small, but the per-country files
    /// total ~17 MB and a phone should not parse all of them at startup.
    /// </summary>
    public sealed class StationCatalog
    {
        private readonly Dictionary<string, CountryEntry> _index =
            new Dictionary<string, CountryEntry>(StringComparer.Ordinal);
        private readonly Dictionary<string, List<Station>> _loaded =
            new Dictionary<string, List<Station>>(StringComparer.Ordinal);

        public IReadOnlyList<string> Countries { get; private set; } = new List<string>();

        /// <summary>Non-fatal problems worth showing in the status line.</summary>
        public string Warning { get; private set; }

        public async Task LoadIndexAsync()
        {
            var folder = await GetDataFolderAsync().ConfigureAwait(false);
            var file = await folder.GetFileAsync("countries.json");
            var text = await FileIO.ReadTextAsync(file);

            var array = JsonArray.Parse(text);
            var names = new List<string>(array.Count);
            foreach (var item in array)
            {
                if (item.ValueType != JsonValueType.Object) continue;
                var obj = item.GetObject();

                var entry = new CountryEntry
                {
                    Name = GetString(obj, "name"),
                    File = GetString(obj, "file"),
                    Count = (int)GetNumber(obj, "count")
                };
                if (string.IsNullOrEmpty(entry.Name) || string.IsNullOrEmpty(entry.File))
                {
                    continue;
                }
                _index[entry.Name] = entry;
                names.Add(entry.Name);
            }

            names.Sort(StringComparer.Ordinal);
            Countries = names;
        }

        /// <summary>
        /// Stations for one country, parsed on first use and cached afterwards.
        /// Returns an empty list rather than throwing if the file is unreadable.
        /// </summary>
        public async Task<IReadOnlyList<Station>> GetStationsAsync(string country)
        {
            if (string.IsNullOrEmpty(country)) return new List<Station>();

            List<Station> cached;
            if (_loaded.TryGetValue(country, out cached)) return cached;

            CountryEntry entry;
            if (!_index.TryGetValue(country, out entry)) return new List<Station>();

            var stations = new List<Station>();
            try
            {
                var folder = await GetDataFolderAsync().ConfigureAwait(false);
                var countries = await folder.GetFolderAsync("countries");
                var file = await countries.GetFileAsync(entry.File);
                var text = await FileIO.ReadTextAsync(file);

                var array = JsonArray.Parse(text);
                foreach (var item in array)
                {
                    if (item.ValueType != JsonValueType.Object) continue;
                    var obj = item.GetObject();

                    var station = new Station
                    {
                        Name = GetString(obj, "name"),
                        Url = GetString(obj, "url"),
                        Codec = GetString(obj, "codec"),
                        Bitrate = (int)GetNumber(obj, "bitrate"),
                        Language = GetString(obj, "language"),
                        Country = country
                    };
                    if (string.IsNullOrEmpty(station.Name) || string.IsNullOrEmpty(station.Url))
                    {
                        continue;
                    }
                    stations.Add(station);
                }
            }
            catch (Exception error)
            {
                Warning = entry.File + ": " + error.Message;
            }

            _loaded[country] = stations;
            return stations;
        }

        private static async Task<StorageFolder> GetDataFolderAsync()
        {
            var assets = await Package.Current.InstalledLocation
                .GetFolderAsync("Assets").AsTask().ConfigureAwait(false);
            return await assets.GetFolderAsync("data").AsTask().ConfigureAwait(false);
        }

        /// <summary>Accepts a string or a number, like the desktop build does.</summary>
        private static string GetString(JsonObject obj, string key)
        {
            IJsonValue value;
            if (!obj.TryGetValue(key, out value)) return string.Empty;
            switch (value.ValueType)
            {
                case JsonValueType.String: return value.GetString();
                case JsonValueType.Number: return value.GetNumber().ToString();
                default: return string.Empty;
            }
        }

        private static double GetNumber(JsonObject obj, string key)
        {
            IJsonValue value;
            if (!obj.TryGetValue(key, out value)) return 0;
            switch (value.ValueType)
            {
                case JsonValueType.Number:
                    return value.GetNumber();
                case JsonValueType.String:
                    double parsed;
                    return double.TryParse(value.GetString(), out parsed) ? parsed : 0;
                default:
                    return 0;
            }
        }
    }
}
