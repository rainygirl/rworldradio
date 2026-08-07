//! R World Radio - macOS port.
//!
//! Everything substantial is shared with the Linux port (`../rust`): the dataset
//! parser, HTTP/TLS, the HLS pipeline, the MPEG-TS demuxer, the ADTS normalizer,
//! symphonia decoding, the cpal output path and the whole egui UI. cpal picks
//! CoreAudio instead of ALSA on its own, so audio needs no code here at all.
//!
//! What this crate adds is only what really is macOS-specific:
//!
//! * `paths` - the dataset search order (app-bundle `Contents/Resources`,
//!   `~/Library/Application Support`) instead of the XDG one.
//! * `fonts` - Hiragino/Apple SD Gothic Neo/PingFang instead of the Noto paths,
//!   layered because no single macOS font covers all of CJK.
//! * `mediakeys` - the F8 play/pause key, which arrives through MediaPlayer's
//!   Now Playing system rather than as a key event.
//! * `MacApp` - Cmd+Q, which AppKit does not handle for a window that has no
//!   main menu.

mod fonts;
mod mediakeys;
mod paths;

use mediakeys::{Command, MediaKeys};
use rworldradio::ui::RadioApp;

/// Wraps the shared app to add the macOS keyboard conventions.
struct MacApp {
    inner: RadioApp,
    media_keys: MediaKeys,
}

impl eframe::App for MacApp {
    fn update(&mut self, ctx: &egui::Context, frame: &mut eframe::Frame) {
        // winit creates no NSMenu, so without this Cmd+Q does nothing - which on
        // macOS reads as the app being broken. Cmd+W closes the only window,
        // which for a single-window app means quitting too.
        let quit_requested = ctx.input(|input| {
            input.modifiers.mac_cmd
                && (input.key_pressed(egui::Key::Q) || input.key_pressed(egui::Key::W))
        });
        if quit_requested {
            ctx.send_viewport_cmd(egui::ViewportCommand::Close);
        }

        // The media key normally goes to the Now Playing system, but with "Use F1,
        // F2, etc. keys as standard function keys" on - or with fn held - F8 is an
        // ordinary key event and has to be handled here as well.
        let f8_pressed = ctx.input(|input| input.key_pressed(egui::Key::F8));
        match self.media_keys.take() {
            Some(Command::Play) if !self.inner.is_active() => self.inner.toggle_playback(),
            Some(Command::Stop) if self.inner.is_active() => self.inner.toggle_playback(),
            Some(Command::Toggle) => self.inner.toggle_playback(),
            _ if f8_pressed => self.inner.toggle_playback(),
            _ => {}
        }

        self.inner.update(ctx, frame);

        // After update(), so a station started this frame is published straight
        // away rather than a frame late.
        self.media_keys
            .publish(self.inner.is_active(), self.inner.current_station_name());
    }

    fn on_exit(&mut self, gl: Option<&eframe::glow::Context>) {
        self.inner.on_exit(gl);
    }
}

fn main() -> eframe::Result<()> {
    // Same content area as the other two ports.
    let options = eframe::NativeOptions {
        // The icon is set at runtime as well as in the bundle: run outside an
        // .app (`cargo run`) there is no bundle for macOS to read it from, and
        // the Dock falls back to the generic executable icon.
        viewport: rworldradio::icon::apply(
            egui::ViewportBuilder::default()
                .with_title("R World Radio")
                .with_inner_size([760.0, 560.0])
                .with_min_inner_size([520.0, 340.0]),
        ),
        ..Default::default()
    };

    eframe::run_native(
        "R World Radio",
        options,
        Box::new(|cc| {
            fonts::install(&cc.egui_ctx);
            Ok(Box::new(MacApp {
                inner: RadioApp::with_loader(cc, Box::new(paths::load_dataset)),
                media_keys: MediaKeys::install(&cc.egui_ctx),
            }))
        }),
    )
}
