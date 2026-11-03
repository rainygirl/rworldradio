using System;
using System.Threading;
using Windows.Foundation.Collections;
using Windows.Media.Playback;
using RWorldRadio.Models;

namespace RWorldRadio.Services
{
    /// <summary>
    /// The foreground half of playback: resolves a station's real stream URL and
    /// hands it to the background audio task, then reflects the task's state back
    /// to the UI.
    ///
    /// **Why the audio is not played here.** Two earlier versions got this wrong:
    ///
    ///  1. `MediaPlayer` with `PlaybackSession`/`CommandManager` - those are 1607
    ///     (build 14393) additions, so the app died right after the splash screen on
    ///     a 10586 phone. The compiler does not catch this; see AGENTS.md.
    ///  2. `MediaElement` in the page, plus a `windows.backgroundTasks` manifest
    ///     declaration - this played fine but **stopped when the screen turned
    ///     off**. A MediaElement belongs to the foreground view, so it is suspended
    ///     with the app. The manifest declaration does not change that ownership.
    ///
    /// So the player itself lives in `AudioTask`, in the separate
    /// BackgroundAudioComponent project - a Windows Runtime Component, because the
    /// OS activates a background task by class name over RPC and that needs an
    /// ActivatableClass registration, which only a .winmd produces. A task class
    /// compiled into the app's own exe fails with
    /// "The RPC server is unavailable (0x800706BA)". That is the arrangement Windows 10
    /// Mobile expects for background audio.
    ///
    /// <see cref="StreamResolver"/> still runs here: nothing in the media stack
    /// follows a `.m3u`/`.pls` pointer or notices that an extensionless URL
    /// redirects to a signed `.m3u8`.
    ///
    /// The generation counter is the desktop design kept as-is: play/stop bump it,
    /// and a superseded resolve unwinds quietly rather than overwriting the newer
    /// station's status.
    /// </summary>
    public sealed class RadioPlayer
    {
        public enum State { Idle, Connecting, Playing, Failed }

        public sealed class StatusEventArgs : EventArgs
        {
            public State State { get; set; }
            public string StationName { get; set; }
            public string Detail { get; set; }
        }

        public event EventHandler<StatusEventArgs> StatusChanged;

        private readonly StreamResolver _resolver = new StreamResolver();
        private CancellationTokenSource _cancel;
        private long _generation;

        /// <summary>
        /// True between sending a play request and the task confirming it started.
        /// The player passes through a Closed/Paused state while switching sources,
        /// which the task reports as "stopped" - without this the label would flash
        /// "Stopped" on every station change.
        private bool _awaitingStart;

        /// Name of whatever is playing, as last reported by the background task.
        /// Kept separately from <see cref="Current"/> because after the app has been
        /// suspended and resumed the audio is still going but this process has lost
        /// its Station object - the task is then the only source of truth.
        /// </summary>
        private string _playingName;

        public RadioPlayer()
        {
            // Guarded: anything that throws while the page is being constructed takes
            // the app down right after the splash screen, with nothing visible on a
            // sideloaded phone to say why.
            try
            {
                BackgroundMediaPlayer.MessageReceivedFromBackground += OnMessageFromBackground;
            }
            catch (Exception error)
            {
                App.Log("background player unavailable: " + error.Message);
            }
        }

        public Station Current { get; private set; }

        /// <summary>
        /// Re-syncs the UI with the background task. Call this when the app resumes:
        /// audio may have kept playing while this process was suspended or even
        /// terminated, in which case everything the UI knew is gone and only the task
        /// can say what is on.
        /// </summary>
        public void RefreshFromBackground()
        {
            try
            {
                var message = new ValueSet();
                message.Add(AudioMessages.QueryState, true);
                BackgroundMediaPlayer.SendMessageToBackground(message);
            }
            catch (Exception error)
            {
                App.Log("state query failed: " + error.Message);
            }
        }

        public async void Play(Station station)
        {
            if (station == null) return;

            var generation = Interlocked.Increment(ref _generation);
            if (_cancel != null) _cancel.Cancel();
            var cancel = new CancellationTokenSource();
            _cancel = cancel;

            Current = station;
            _playingName = station.Name;
            Emit(State.Connecting, station.Name, null);

            try
            {
                var resolved = await _resolver
                    .ResolveAsync(station.Url, cancel.Token)
                    .ConfigureAwait(true);

                if (Interlocked.Read(ref _generation) != generation) return;

                var message = new ValueSet();
                message.Add(AudioMessages.PlayUri, resolved.Url);
                message.Add(AudioMessages.Title, station.Name);
                message.Add(AudioMessages.Subtitle, station.Country ?? string.Empty);
                _awaitingStart = true;
                BackgroundMediaPlayer.SendMessageToBackground(message);
            }
            catch (OperationCanceledException)
            {
                // Superseded by a newer Play/Stop; that call owns the status now.
            }
            catch (Exception error)
            {
                if (Interlocked.Read(ref _generation) == generation)
                {
                    Emit(State.Failed, station.Name, error.Message);
                }
            }
        }

        public void Stop()
        {
            Interlocked.Increment(ref _generation);
            if (_cancel != null) { _cancel.Cancel(); _cancel = null; }
            Current = null;
            _playingName = null;
            _awaitingStart = false;

            try
            {
                var message = new ValueSet();
                message.Add(AudioMessages.Stop, true);
                BackgroundMediaPlayer.SendMessageToBackground(message);
            }
            catch (Exception error)
            {
                App.Log("stop message failed: " + error.Message);
            }
            Emit(State.Idle, null, null);
        }

        /// <summary>
        /// 0.0-1.0 for the level indicator. There is no metering in this media stack,
        /// so this only reflects whether audio is playing - unlike the desktop
        /// builds, which compute a true peak from the decoded buffer.
        /// </summary>
        public double Level
        {
            get
            {
                try
                {
                    return BackgroundMediaPlayer.Current.CurrentState == MediaPlayerState.Playing
                        ? 1.0 : 0.0;
                }
                catch (Exception)
                {
                    return 0.0;
                }
            }
        }

        /// <summary>
        /// State pushed up from the background task. Arrives on a background thread;
        /// the page marshals to the dispatcher in its own handler.
        /// </summary>
        private void OnMessageFromBackground(object sender, MediaPlayerDataReceivedEventArgs e)
        {
            object value;

            // The task sends the station name alongside the state, which is what
            // makes a resumed UI able to label what it is hearing.
            object nowPlaying;
            if (e.Data.TryGetValue(AudioMessages.NowPlaying, out nowPlaying) && nowPlaying != null)
            {
                var name = nowPlaying.ToString();
                if (!string.IsNullOrEmpty(name)) _playingName = name;
            }

            if (e.Data.TryGetValue(AudioMessages.Error, out value))
            {
                _awaitingStart = false;
                Emit(State.Failed, _playingName, value != null ? value.ToString() : null);
                return;
            }

            if (e.Data.TryGetValue(AudioMessages.State, out value) && value != null)
            {
                switch (value.ToString())
                {
                    case "playing":
                        _awaitingStart = false;
                        Emit(State.Playing, _playingName, null);
                        break;
                    case "connecting":
                        Emit(State.Connecting, _playingName, null);
                        break;
                    case "stopped":
                        // Ignore the transient stop that a source switch produces;
                        // otherwise the label flashes "Stopped" on every change.
                        if (_awaitingStart) break;
                        _playingName = null;
                        Emit(State.Idle, null, null);
                        break;
                }
            }
        }

        private void Emit(State state, string station, string detail)
        {
            var handler = StatusChanged;
            if (handler != null)
            {
                handler(this, new StatusEventArgs
                {
                    State = state,
                    StationName = station,
                    Detail = detail
                });
            }
        }
    }
}
