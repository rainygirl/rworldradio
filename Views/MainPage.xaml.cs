using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using Windows.UI.Core;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using RWorldRadio.Models;
using RWorldRadio.Services;

namespace RWorldRadio.Views
{
    public sealed partial class MainPage : Page
    {
        private readonly StationCatalog _catalog = new StationCatalog();
        // Constructed after InitializeComponent so a failure inside it is logged
        // rather than taking down the whole page silently.
        private readonly RadioPlayer _player;

        private readonly ObservableCollection<string> _visibleCountries =
            new ObservableCollection<string>();
        private readonly ObservableCollection<Station> _visibleStations =
            new ObservableCollection<Station>();

        private IReadOnlyList<string> _allCountries = new List<string>();
        private IReadOnlyList<Station> _allStations = new List<Station>();

        public MainPage()
        {
            InitializeComponent();

            _player = new RadioPlayer();

            CountryList.ItemsSource = _visibleCountries;
            StationList.ItemsSource = _visibleStations;

            _player.StatusChanged += OnPlayerStatus;

            // Hardware back button: from the station list, go back to countries
            // rather than leaving the app. Guarded because anything that throws in
            // this constructor kills the app right after the splash screen, with no
            // visible error on a sideloaded phone.
            try
            {
                SystemNavigationManager.GetForCurrentView().BackRequested += OnBackRequested;
            }
            catch (Exception error)
            {
                App.Log("BackRequested hookup failed: " + error.Message);
            }

            // Re-sync with the background audio task whenever the app comes back.
            // Audio survives suspension but this process may not, so on resume the
            // UI can be showing "Stopped" while a station is still playing.
            Application.Current.Resuming += OnAppResuming;

            Loaded += OnLoaded;
        }

        private async void OnAppResuming(object sender, object e)
        {
            // Resuming is raised on a background thread.
            await Dispatcher.RunAsync(CoreDispatcherPriority.Normal,
                () => _player.RefreshFromBackground());
        }

        private async void OnLoaded(object sender, RoutedEventArgs e)
        {
            StatusText.Text = "Loading station list...";
            // Also ask on first load: the app may have been terminated while the
            // audio kept playing, so this launch is effectively a resume.
            _player.RefreshFromBackground();
            try
            {
                await _catalog.LoadIndexAsync();
                _allCountries = _catalog.Countries;
                ApplyCountryFilter(SearchBox.Text);
                StatusText.Text = _allCountries.Count + " countries";
            }
            catch (Exception error)
            {
                StatusText.Text = "Could not load the station list: " + error.Message;
                App.Log("LoadIndexAsync failed: " + error);
            }
        }

        // ---- country pane ----

        private void ApplyCountryFilter(string needle)
        {
            _visibleCountries.Clear();
            var lowered = (needle ?? string.Empty).ToLowerInvariant();
            foreach (var name in _allCountries)
            {
                if (lowered.Length == 0 ||
                    name.ToLowerInvariant().Contains(lowered))
                {
                    _visibleCountries.Add(name);
                }
            }
        }

        private async void OnCountryClick(object sender, ItemClickEventArgs e)
        {
            var country = e.ClickedItem as string;
            if (string.IsNullOrEmpty(country)) return;

            _allStations = await _catalog.GetStationsAsync(country);

            // A new country starts unfiltered, matching the desktop builds.
            SearchBox.Text = string.Empty;
            SearchBox.PlaceholderText = "Search stations";
            ApplyStationFilter(string.Empty);

            CountryList.Visibility = Visibility.Collapsed;
            StationList.Visibility = Visibility.Visible;
            UpdateBackButton();
            StatusText.Text = country + " - " + _allStations.Count + " stations";
        }

        // ---- station pane ----

        private void ApplyStationFilter(string needle)
        {
            _visibleStations.Clear();
            var lowered = (needle ?? string.Empty).ToLowerInvariant();
            foreach (var station in _allStations)
            {
                if (lowered.Length == 0 ||
                    station.Name.ToLowerInvariant().Contains(lowered))
                {
                    _visibleStations.Add(station);
                }
            }
        }

        private void OnStationClick(object sender, ItemClickEventArgs e)
        {
            // Single tap, not the desktop's double-click: on a phone there is no
            // separate "select" gesture to distinguish it from.
            var station = e.ClickedItem as Station;
            if (station != null) _player.Play(station);
        }

        private void OnSearchTextChanged(object sender, TextChangedEventArgs e)
        {
            if (StationList.Visibility == Visibility.Visible)
            {
                ApplyStationFilter(SearchBox.Text);
            }
            else
            {
                ApplyCountryFilter(SearchBox.Text);
            }
        }

        // ---- navigation ----

        private void OnBackRequested(object sender, BackRequestedEventArgs e)
        {
            if (StationList.Visibility != Visibility.Visible) return;

            StationList.Visibility = Visibility.Collapsed;
            CountryList.Visibility = Visibility.Visible;
            SearchBox.Text = string.Empty;
            SearchBox.PlaceholderText = "Search countries";
            ApplyCountryFilter(string.Empty);
            UpdateBackButton();
            e.Handled = true; // do not let the shell close the app
        }

        private void UpdateBackButton()
        {
            // On a phone the back button is hardware, so this is cosmetic - never
            // worth taking the app down for.
            try
            {
                SystemNavigationManager.GetForCurrentView().AppViewBackButtonVisibility =
                    StationList.Visibility == Visibility.Visible
                        ? AppViewBackButtonVisibility.Visible
                        : AppViewBackButtonVisibility.Collapsed;
            }
            catch (Exception error)
            {
                App.Log("back button visibility failed: " + error.Message);
            }
        }

        // ---- playback ----

        private void OnStopClick(object sender, RoutedEventArgs e)
        {
            _player.Stop();
        }

        private async void OnPlayerStatus(object sender, RadioPlayer.StatusEventArgs e)
        {
            // MediaPlayer raises its events on a background thread.
            await Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () =>
            {
                switch (e.State)
                {
                    case RadioPlayer.State.Connecting:
                        NowPlayingText.Text = "Connecting: " + e.StationName;
                        StopButton.Visibility = Visibility.Collapsed;
                        break;
                    case RadioPlayer.State.Playing:
                        NowPlayingText.Text = "Now Playing: " + e.StationName;
                        StopButton.Visibility = Visibility.Visible;
                        break;
                    case RadioPlayer.State.Idle:
                        NowPlayingText.Text = "Stopped";
                        StopButton.Visibility = Visibility.Collapsed;
                        break;
                    case RadioPlayer.State.Failed:
                        NowPlayingText.Text = "Error (" + e.StationName + ")";
                        StatusText.Text = e.Detail ?? string.Empty;
                        StopButton.Visibility = Visibility.Collapsed;
                        break;
                }
            });
        }
    }
}
