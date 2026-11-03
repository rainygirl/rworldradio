namespace RWorldRadio.Services
{
    /// <summary>
    /// Keys for the ValueSet messages exchanged with the background audio task.
    ///
    /// Duplicated deliberately: the task lives in a separate Windows Runtime
    /// Component, and a WinRT component cannot expose `const` fields across the ABI.
    /// The copy in BackgroundAudioComponent/AudioTask.cs must stay in step with this
    /// one - they are the wire format between the two processes.
    /// </summary>
    internal static class AudioMessages
    {
        public const string PlayUri = "play-uri";
        public const string Title = "title";
        public const string Subtitle = "subtitle";
        public const string Stop = "stop";
        public const string State = "state";
        public const string Error = "error";

        /// <summary>
        /// UI -> task: "what are you playing?". Sent when the app resumes, because
        /// the foreground process may have been terminated while the audio kept
        /// going, losing everything the UI knew.
        /// </summary>
        public const string QueryState = "query-state";

        /// <summary>
        /// task -> UI: the station name that goes with a State reply. The UI cannot
        /// supply this itself after being suspended.
        /// </summary>
        public const string NowPlaying = "now-playing";
    }
}
