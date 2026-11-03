//! Where the station dataset lives on macOS.
//!
//! The Linux port's list is XDG-shaped (`~/.local/share`, `/usr/share`). macOS
//! has two different conventions that matter here: an app bundle keeps its data
//! inside itself under `Contents/Resources`, and anything installed outside a
//! bundle goes in `~/Library/Application Support`. Everything else - actually
//! reading and parsing the files - is shared with the Linux port.

use std::env;
use std::path::PathBuf;

use rworldradio::station_cache;

/// `data/` inside the enclosing `.app` bundle, if the binary is running from one.
///
/// A bundled binary lives at `RWorldRadio.app/Contents/MacOS/rworldradio`, and
/// the convention is for its data to sit beside it in
/// `RWorldRadio.app/Contents/Resources/`. `install.sh` copies the dataset there,
/// which is what makes the installed app self-contained - drag it to the Trash
/// and nothing is left behind.
pub fn bundle_resources_data() -> Option<PathBuf> {
    let exe = env::current_exe().ok()?;
    let macos_dir = exe.parent()?;
    if macos_dir.file_name()? != "MacOS" {
        return None; // not running from a bundle
    }
    let contents = macos_dir.parent()?;
    Some(contents.join("Resources/data"))
}

/// `~/Library/Application Support/RWorldRadio/data`.
pub fn application_support_data() -> Option<PathBuf> {
    let home = env::var_os("HOME")?;
    Some(
        PathBuf::from(home)
            .join("Library/Application Support/RWorldRadio/data"),
    )
}

/// The full macOS search order, highest priority first.
pub fn candidate_data_dirs() -> Vec<PathBuf> {
    let mut candidates = Vec::new();

    // A bundle's own Resources wins: if the app was installed, that copy is the
    // one meant to be used, even if a checkout happens to be lying around.
    candidates.extend(bundle_resources_data());
    // Then the checkout layouts (`cargo run`, `target/release/…`).
    candidates.extend(station_cache::exe_relative_candidates());
    candidates.extend(application_support_data());
    candidates.push(PathBuf::from("/Library/Application Support/RWorldRadio/data"));
    candidates.extend(station_cache::relative_candidates());

    candidates
}

/// Loads the dataset from the first candidate that has one.
pub fn load_dataset() -> Result<station_cache::LoadResult, String> {
    station_cache::load_from(&candidate_data_dirs())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn search_order_includes_the_macos_conventions() {
        let candidates = candidate_data_dirs();
        let rendered: Vec<String> = candidates
            .iter()
            .map(|p| p.display().to_string())
            .collect();

        assert!(
            rendered.iter().any(|p| p.contains("Library/Application Support/RWorldRadio/data")),
            "missing Application Support path in {rendered:?}"
        );
        assert!(
            rendered.iter().any(|p| p.ends_with("../shared/data") || p.ends_with("data")),
            "missing relative fallback in {rendered:?}"
        );
    }

    #[test]
    fn bundle_path_is_none_outside_a_bundle() {
        // The test binary lives in target/debug/deps, not Contents/MacOS.
        assert!(bundle_resources_data().is_none());
    }

    /// The dataset must be findable when running out of the checkout, which is
    /// what `cargo run` does. Skipped if `data/` isn't present.
    #[test]
    fn finds_the_checkout_dataset() {
        let repo_data = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../shared/data");
        if !repo_data.join("countries.json").is_file() {
            eprintln!("skipping: shared/data/ not present");
            return;
        }
        let loaded = load_dataset().expect("dataset should be found from the checkout");
        assert!(loaded.by_country.len() > 100);
    }
}
