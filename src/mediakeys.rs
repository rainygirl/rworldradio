//! The play/pause media key (F8), and the Now Playing state it needs to work.
//!
//! F8 never reaches the window as a key event. macOS intercepts it in the WindowServer
//! and routes it to whichever app most recently registered as the Now Playing app -
//! which is also what fills in Control Centre's Now Playing tile and the Touch Bar.
//! Registering means two things, and doing only the first is not enough:
//!
//! 1. Handlers on `MPRemoteCommandCenter`, so there is something to route *to*.
//! 2. `MPNowPlayingInfoCenter`'s `playbackState` and info dictionary, so the system
//!    considers this app a plausible target. An app that never publishes a playing
//!    state is skipped and the key goes to Music.app instead.
//!
//! Handlers fire on the main thread from AppKit's run loop, i.e. between egui frames,
//! so they only record what was pressed; `take` drains it on the next frame. The
//! `request_repaint` is what makes "next frame" happen promptly - the app is otherwise
//! idle when stopped, and the key press would sit unhandled until something else woke
//! the UI.

use std::ptr::NonNull;
use std::sync::atomic::{AtomicU8, Ordering};
use std::sync::Arc;

use block2::RcBlock;
use objc2::rc::Retained;
use objc2::runtime::AnyObject;
use objc2_foundation::{NSDictionary, NSNumber, NSString};
use objc2_media_player::{
    MPMediaItemPropertyTitle, MPNowPlayingInfoCenter, MPNowPlayingInfoPropertyIsLiveStream,
    MPNowPlayingPlaybackState, MPRemoteCommandCenter, MPRemoteCommandEvent,
    MPRemoteCommandHandlerStatus,
};

/// What the last media-key press asked for.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Command {
    Play,
    Stop,
    Toggle,
}

const NONE: u8 = 0;
const PLAY: u8 = 1;
const STOP: u8 = 2;
const TOGGLE: u8 = 3;

pub struct MediaKeys {
    pending: Arc<AtomicU8>,
    now_playing: Retained<MPNowPlayingInfoCenter>,
    /// What was last published, so the info dictionary is only rebuilt when it
    /// actually changed rather than on every frame.
    published: Option<(bool, String)>,
    /// `addTargetWithHandler:` returns a token that owns the block; dropping it
    /// unregisters the handler, so these have to outlive the app.
    _targets: Vec<Retained<AnyObject>>,
}

impl MediaKeys {
    /// Registers the transport commands. `ctx` is only used to wake the UI when a
    /// key arrives while the app is idle.
    pub fn install(ctx: &egui::Context) -> Self {
        let pending = Arc::new(AtomicU8::new(NONE));

        // SAFETY: MediaPlayer's shared singletons, called on the main thread before
        // the event loop starts. The handler blocks capture only an Arc and an
        // egui::Context, both of which outlive the registration (they are kept in
        // the returned MediaKeys, which main.rs holds for the life of the app).
        let (now_playing, _targets) = unsafe {
            let center = MPRemoteCommandCenter::sharedCommandCenter();

            // Every transport command has to be answered. A command left at its
            // default disabled state makes the system look for another app, so the
            // ones this app cannot honour (skip, seek, next/previous - there is no
            // timeline in a live stream) are explicitly disabled rather than ignored.
            for unsupported in [
                center.nextTrackCommand(),
                center.previousTrackCommand(),
                center.seekForwardCommand(),
                center.seekBackwardCommand(),
                center.changePlaybackPositionCommand().into_super(),
            ] {
                unsupported.setEnabled(false);
            }

            let mut targets = Vec::new();
            for (command, request) in [
                (center.togglePlayPauseCommand(), TOGGLE),
                (center.playCommand(), PLAY),
                (center.pauseCommand(), STOP),
                (center.stopCommand(), STOP),
            ] {
                command.setEnabled(true);
                targets.push(command.addTargetWithHandler(&handler(&pending, ctx, request)));
            }

            (MPNowPlayingInfoCenter::defaultCenter(), targets)
        };

        MediaKeys {
            pending,
            now_playing,
            published: None,
            _targets,
        }
    }

    /// The media-key press since the last call, if any.
    pub fn take(&self) -> Option<Command> {
        match self.pending.swap(NONE, Ordering::AcqRel) {
            PLAY => Some(Command::Play),
            STOP => Some(Command::Stop),
            TOGGLE => Some(Command::Toggle),
            _ => None,
        }
    }

    /// Publishes what is playing. Call every frame - it is a no-op unless the state
    /// changed, and the system needs this to keep routing the key here.
    pub fn publish(&mut self, active: bool, station: Option<&str>) {
        let station = station.unwrap_or("R World Radio");
        if self.published.as_ref().is_some_and(|(was_active, was_station)| {
            *was_active == active && was_station == station
        }) {
            return;
        }
        self.published = Some((active, station.to_string()));

        // SAFETY: main thread, and the dictionary is copied by the setter.
        unsafe {
            let title = NSString::from_str(station);
            let live = NSNumber::new_bool(true);
            let info = NSDictionary::from_slices(
                &[MPMediaItemPropertyTitle, MPNowPlayingInfoPropertyIsLiveStream],
                &[&*title as &AnyObject, &*live as &AnyObject],
            );
            self.now_playing.setNowPlayingInfo(Some(&info));
            // Paused, not Stopped, when idle: Stopped hands the Now Playing role
            // back to the system, and the next F8 would go to Music.app instead of
            // starting the selected station here.
            self.now_playing.setPlaybackState(if active {
                MPNowPlayingPlaybackState::Playing
            } else {
                MPNowPlayingPlaybackState::Paused
            });
        }
    }
}

fn handler(
    pending: &Arc<AtomicU8>,
    ctx: &egui::Context,
    request: u8,
) -> RcBlock<dyn Fn(NonNull<MPRemoteCommandEvent>) -> MPRemoteCommandHandlerStatus> {
    let pending = Arc::clone(pending);
    let ctx = ctx.clone();
    RcBlock::new(move |_event: NonNull<MPRemoteCommandEvent>| {
        pending.store(request, Ordering::Release);
        ctx.request_repaint();
        MPRemoteCommandHandlerStatus::Success
    })
}
