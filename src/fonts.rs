//! macOS system fonts for the non-Latin station names.
//!
//! Unlike Linux, macOS has no fontconfig to ask and no single font with full CJK
//! coverage: Hiragino covers Japanese, Apple SD Gothic Neo covers Korean, PingFang
//! and Hiragino Sans GB cover Chinese. The shared loader installs several
//! fallbacks in order, so the list below is layered by script rather than being a
//! first-match-wins search like the Linux one.
//!
//! Paths (not font names) are used deliberately: reading a file needs no
//! CoreText/AppKit binding, which keeps this port from pulling in an Objective-C
//! bridge just to find a font.

use std::path::PathBuf;

/// In priority order. The shared loader skips anything missing or unparseable and
/// stops once it has enough, so listing per-OS-version alternatives is free.
const CANDIDATES: &[&str] = &[
    // Japanese (kana + kanji). The localized filename is what ships in
    // /System/Library/Fonts; the ASCII-named W3 variant exists on some versions.
    "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
    "/System/Library/Fonts/Hiragino Sans W3.ttc",
    // Korean.
    "/System/Library/Fonts/AppleSDGothicNeo.ttc",
    // Chinese (Simplified/Traditional).
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/Supplemental/PingFang.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    // Broad catch-all: Cyrillic, Greek, Arabic, Hebrew, Thai and more in one
    // file. Present on essentially every macOS install.
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
    "/Library/Fonts/Arial Unicode.ttf",
];

pub fn install(ctx: &egui::Context) {
    let candidates: Vec<PathBuf> = CANDIDATES.iter().map(PathBuf::from).collect();
    let installed = rworldradio::fonts::install_fallbacks(ctx, &candidates);
    if installed.is_empty() {
        eprintln!(
            "rworldradio: no usable system font found; non-Latin station names may \
             not render."
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Guards the hardcoded list: if macOS moves these files, the app silently
    /// loses CJK rendering, and this is where that should be noticed.
    #[test]
    fn at_least_one_candidate_exists_on_this_machine() {
        let found: Vec<&str> = CANDIDATES
            .iter()
            .copied()
            .filter(|path| std::path::Path::new(path).is_file())
            .collect();
        assert!(
            !found.is_empty(),
            "none of the macOS font candidates exist: {CANDIDATES:?}"
        );
        eprintln!("font candidates present: {found:?}");
    }

    #[test]
    fn installs_and_reports_what_it_used() {
        let ctx = egui::Context::default();
        let candidates: Vec<PathBuf> = CANDIDATES.iter().map(PathBuf::from).collect();
        let installed = rworldradio::fonts::install_fallbacks(&ctx, &candidates);
        assert!(!installed.is_empty(), "no macOS fallback font could be loaded");
        eprintln!("installed fallbacks: {installed:?}");
    }
}
