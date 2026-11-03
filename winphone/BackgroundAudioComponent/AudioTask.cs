using System;
using Windows.ApplicationModel.Background;
using Windows.Media;
using Windows.Media.Playback;
using Windows.Foundation.Collections;

namespace RWorldRadio.BackgroundAudio
{
    /// <summary>
    /// The background audio task. This is what actually keeps playing when the
    /// screen turns off.
    ///
    /// **Why this class exists.** The first attempt used `MediaElement` in the page
    /// plus a `windows.backgroundTasks` manifest declaration. That was not enough:
    /// a `MediaElement` belongs to the foreground app's view, so when the phone
    /// suspends the app the audio stops with it. The declaration alone changes
    /// nothing about that ownership. On Windows 10 Mobile the audio has to be owned
    /// by a *separate background process*, and that is what
    /// `BackgroundMediaPlayer` gives you: the player lives in this task, outlives
    /// the foreground UI, and the two halves talk over
    /// `SendMessageToBackground`/`SendMessageToForeground`.
    ///
    /// This task is declared in Package.appxmanifest as the audio background task's
    /// EntryPoint. It must not do anything slow in Run(): the OS gives it a short
    /// window before it is considered hung.
    /// </summary>
    public sealed class AudioTask : IBackgroundTask
    {
        // Message keys shared with the foreground app. These are internal to this
        // component: a WinRT component may not expose const fields across the ABI,
        // and the app has its own copy in Services/AudioMessages.cs. They cross the
        // process boundary as ValueSet keys, so the two copies must agree.
        internal const string MessagePlayUri = "play-uri";
        internal const string MessageTitle = "title";
        internal const string MessageSubtitle = "subtitle";
        internal const string MessageStop = "stop";
        internal const string MessageState = "state";
        internal const string MessageError = "error";
        internal const string MessageQueryState = "query-state";
        internal const string MessageNowPlaying = "now-playing";

        private BackgroundTaskDeferral _deferral;
        private SystemMediaTransportControls _transport;
        private string _title = "R World Radio";
        private string _subtitle = string.Empty;

        public void Run(IBackgroundTaskInstance taskInstance)
        {
            // Holding a deferral is what keeps this task alive between messages;
            // without it the OS tears it down as soon as Run() returns and the
            // audio dies with it.
            _deferral = taskInstance.GetDeferral();
            taskInstance.Canceled += OnCanceled;
            taskInstance.Task.Completed += OnTaskCompleted;

            // The lock screen / headset controls must be answered by whichever
            // process owns the audio - here, not the UI.
            _transport = BackgroundMediaPlayer.Current.SystemMediaTransportControls;
            _transport.IsEnabled = true;
            _transport.IsPlayEnabled = true;
            _transport.IsPauseEnabled = true;
            _transport.IsStopEnabled = true;
            _transport.ButtonPressed += OnButtonPressed;

            BackgroundMediaPlayer.Current.CurrentStateChanged += OnCurrentStateChanged;
            BackgroundMediaPlayer.Current.MediaFailed += OnMediaFailed;
            BackgroundMediaPlayer.MessageReceivedFromForeground += OnMessageFromForeground;
        }

        // ---- messages from the UI ----

        private void OnMessageFromForeground(object sender, MediaPlayerDataReceivedEventArgs e)
        {
            object value;

            if (e.Data.TryGetValue(MessageQueryState, out value))
            {
                // The UI has just resumed and knows nothing. Report what is actually
                // playing, including the station name - the UI cannot supply that
                // itself after being suspended.
                ReportState();
                return;
            }

            if (e.Data.TryGetValue(MessageStop, out value))
            {
                BackgroundMediaPlayer.Current.Pause();
                _title = "R World Radio";
                _subtitle = string.Empty;
                UpdateTransport(MediaPlaybackStatus.Stopped);
                ReportState();
                return;
            }

            if (e.Data.TryGetValue(MessagePlayUri, out value))
            {
                object t, s;
                _title = e.Data.TryGetValue(MessageTitle, out t) && t != null
                    ? t.ToString() : "R World Radio";
                _subtitle = e.Data.TryGetValue(MessageSubtitle, out s) && s != null
                    ? s.ToString() : string.Empty;

                try
                {
                    // AutoPlay ensures playback starts without a further round trip.
                    BackgroundMediaPlayer.Current.AutoPlay = true;
                    BackgroundMediaPlayer.Current.SetUriSource(new Uri(value.ToString()));
                    UpdateTransport(MediaPlaybackStatus.Changing);
                }
                catch (Exception error)
                {
                    SendToForeground(MessageError, error.Message);
                }
            }
        }

        // ---- player and transport events ----

        private void OnCurrentStateChanged(MediaPlayer sender, object args)
        {
            switch (sender.CurrentState)
            {
                case MediaPlayerState.Playing:
                    UpdateTransport(MediaPlaybackStatus.Playing);
                    break;
                case MediaPlayerState.Buffering:
                case MediaPlayerState.Opening:
                    UpdateTransport(MediaPlaybackStatus.Changing);
                    break;
                case MediaPlayerState.Paused:
                case MediaPlayerState.Closed:
                    UpdateTransport(MediaPlaybackStatus.Stopped);
                    break;
            }
            ReportState();
        }

        /// <summary>
        /// Tells the UI both the state and which station it belongs to. Sending them
        /// together is what lets a freshly resumed UI show the right label without
        /// having remembered anything.
        /// </summary>
        private void ReportState()
        {
            string state;
            try
            {
                switch (BackgroundMediaPlayer.Current.CurrentState)
                {
                    case MediaPlayerState.Playing:   state = "playing"; break;
                    case MediaPlayerState.Buffering:
                    case MediaPlayerState.Opening:   state = "connecting"; break;
                    default:                         state = "stopped"; break;
                }
            }
            catch (Exception)
            {
                state = "stopped";
            }

            try
            {
                var message = new ValueSet();
                message.Add(MessageState, state);
                message.Add(MessageNowPlaying, state == "stopped" ? string.Empty : _title);
                BackgroundMediaPlayer.SendMessageToForeground(message);
            }
            catch (Exception)
            {
                // The UI may not be running; normal, not an error.
            }
        }

        private void OnMediaFailed(MediaPlayer sender, MediaPlayerFailedEventArgs args)
        {
            UpdateTransport(MediaPlaybackStatus.Stopped);
            SendToForeground(MessageError, args.ErrorMessage ?? args.Error.ToString());
        }

        private void OnButtonPressed(
            SystemMediaTransportControls sender,
            SystemMediaTransportControlsButtonPressedEventArgs args)
        {
            switch (args.Button)
            {
                case SystemMediaTransportControlsButton.Play:
                    BackgroundMediaPlayer.Current.Play();
                    break;
                case SystemMediaTransportControlsButton.Pause:
                case SystemMediaTransportControlsButton.Stop:
                    BackgroundMediaPlayer.Current.Pause();
                    break;
            }
        }

        private void UpdateTransport(MediaPlaybackStatus status)
        {
            try
            {
                _transport.PlaybackStatus = status;
                var display = _transport.DisplayUpdater;
                display.Type = MediaPlaybackType.Music;
                display.MusicProperties.Title = _title;
                display.MusicProperties.Artist = _subtitle;
                display.Update();
            }
            catch (Exception)
            {
                // Cosmetic only - never worth taking the audio down for.
            }
        }

        private static void SendToForeground(string key, string value)
        {
            try
            {
                var message = new ValueSet();
                message.Add(key, value);
                BackgroundMediaPlayer.SendMessageToForeground(message);
            }
            catch (Exception)
            {
                // The UI may not be running; that is normal and not an error.
            }
        }

        // ---- teardown ----

        private void OnCanceled(IBackgroundTaskInstance sender, BackgroundTaskCancellationReason reason)
        {
            Shutdown();
        }

        private void OnTaskCompleted(BackgroundTaskRegistration sender, BackgroundTaskCompletedEventArgs args)
        {
            Shutdown();
        }

        private void Shutdown()
        {
            try
            {
                BackgroundMediaPlayer.MessageReceivedFromForeground -= OnMessageFromForeground;
                BackgroundMediaPlayer.Current.CurrentStateChanged -= OnCurrentStateChanged;
                BackgroundMediaPlayer.Current.MediaFailed -= OnMediaFailed;
                if (_transport != null) _transport.ButtonPressed -= OnButtonPressed;
                BackgroundMediaPlayer.Shutdown();
            }
            catch (Exception)
            {
            }
            if (_deferral != null)
            {
                _deferral.Complete();
                _deferral = null;
            }
        }
    }
}
