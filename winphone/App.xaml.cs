using System;
using Windows.ApplicationModel.Activation;
using Windows.Storage;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using RWorldRadio.Views;

namespace RWorldRadio
{
    public sealed partial class App : Application
    {
        public App()
        {
            InitializeComponent();

            // A sideloaded app on a phone gives no console and no debugger, so an
            // unhandled exception is invisible - the splash screen just disappears.
            // Write it to LocalState instead, which is readable over MTP at
            // Phone\Documents or via Device Portal.
            UnhandledException += (s, e) =>
            {
                e.Handled = true;
                Log("UnhandledException: " + e.Message + "\n" + e.Exception);
            };
        }

        /// <summary>
        /// Appends a line to LocalState\crash.log. Best-effort: never throws, since
        /// it runs from an exception handler.
        /// </summary>
        internal static void Log(string message)
        {
            try
            {
                var path = System.IO.Path.Combine(
                    ApplicationData.Current.LocalFolder.Path, "crash.log");
                System.IO.File.AppendAllText(
                    path, DateTime.Now.ToString("HH:mm:ss") + "  " + message + "\n");
            }
            catch { }
        }

        protected override void OnLaunched(LaunchActivatedEventArgs e)
        {
            var rootFrame = Window.Current.Content as Frame;
            if (rootFrame == null)
            {
                rootFrame = new Frame();
                Window.Current.Content = rootFrame;
            }

            if (rootFrame.Content == null)
            {
                // Navigation failures are the other way a phone app dies silently
                // at startup, so record them too.
                rootFrame.NavigationFailed += (s, args) =>
                    Log("NavigationFailed: " + args.SourcePageType + " " + args.Exception);
                try
                {
                    rootFrame.Navigate(typeof(MainPage), e.Arguments);
                }
                catch (Exception error)
                {
                    Log("Navigate threw: " + error);
                    throw;
                }
            }
            Window.Current.Activate();
            Log("activated");
        }
    }
}
