#!/bin/sh
# Builds RWorldRadio.app and installs it into an Applications folder. Run from
# anywhere:
#
#   ./install.sh                 # build + install into ~/Applications (no sudo)
#   ./install.sh --system        # install into /Applications (may need sudo)
#   ./install.sh --build-only    # just assemble build/RWorldRadio.app
#   ./install.sh --universal     # build an arm64 + x86_64 universal binary
#   ./install.sh --uninstall [--system]
#
# The bundle is self-contained: the dataset is copied into
# Contents/Resources/data, so the installed app doesn't depend on this checkout
# still existing, and dragging it to the Trash removes everything. That's the
# macOS convention, and simpler than the Linux port's XDG split.
set -e

cd "$(dirname "$0")"
PROJECT_DIR=$PWD
DATA_SRC="$PROJECT_DIR/data"
BUILD_DIR="$PROJECT_DIR/build"
APP_NAME="RWorldRadio.app"

MODE=install
SCOPE=user
UNIVERSAL=no
for arg in "$@"; do
	case "$arg" in
		--system) SCOPE=system ;;
		--build-only) MODE=build ;;
		--uninstall) MODE=uninstall ;;
		--universal) UNIVERSAL=yes ;;
		--help|-h)
			awk 'NR>1 && /^#/ { sub(/^# ?/, ""); print; next } NR>1 { exit }' "$0"
			exit 0 ;;
		*)
			echo "install.sh: unknown option: $arg" >&2
			exit 2 ;;
	esac
done

if [ "$SCOPE" = system ]; then
	APPS_DIR=/Applications
else
	APPS_DIR="$HOME/Applications"
fi

if [ "$MODE" = uninstall ]; then
	rm -rf "$APPS_DIR/$APP_NAME"
	echo "Removed $APPS_DIR/$APP_NAME"
	exit 0
fi

if [ "$(uname -s)" != Darwin ]; then
	echo "install.sh: this is the macOS port; for Linux see the rworldradio-linux repo" >&2
	exit 1
fi

if ! command -v cargo >/dev/null 2>&1; then
	echo "install.sh: cargo not found. Install Rust from https://rustup.rs" >&2
	exit 1
fi

if [ ! -f "$DATA_SRC/countries.json" ]; then
	echo "install.sh: no dataset at $DATA_SRC/countries.json" >&2
	echo "  (run tools/update_stations_db.py first)" >&2
	exit 1
fi

if [ "$UNIVERSAL" = yes ]; then
	echo "Building (release, universal)..."
	for target in aarch64-apple-darwin x86_64-apple-darwin; do
		if ! rustup target list --installed 2>/dev/null | grep -qx "$target"; then
			echo "  adding target $target"
			rustup target add "$target"
		fi
		cargo build --release --target "$target"
	done
	BINARY="$PROJECT_DIR/target/universal-rworldradio"
	lipo -create -output "$BINARY" \
		"$PROJECT_DIR/target/aarch64-apple-darwin/release/rworldradio" \
		"$PROJECT_DIR/target/x86_64-apple-darwin/release/rworldradio"
else
	echo "Building (release)..."
	cargo build --release
	BINARY="$PROJECT_DIR/target/release/rworldradio"
fi

[ -x "$BINARY" ] || { echo "install.sh: $BINARY missing after build" >&2; exit 1; }

# Assemble the bundle from scratch so a rebuild can't leave stale files behind.
APP="$BUILD_DIR/$APP_NAME"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

cp "$BINARY" "$APP/Contents/MacOS/rworldradio"
chmod 755 "$APP/Contents/MacOS/rworldradio"
cp packaging/Info.plist "$APP/Contents/Info.plist"
cp packaging/rworldradio.icns "$APP/Contents/Resources/rworldradio.icns"
printf 'APPL????' > "$APP/Contents/PkgInfo"

mkdir -p "$APP/Contents/Resources/data"
cp "$DATA_SRC/countries.json" "$APP/Contents/Resources/data/"
cp -R "$DATA_SRC/countries" "$APP/Contents/Resources/data/"

# Ad-hoc signature. The linker already ad-hoc signs the raw binary, but `lipo`
# and copying into a bundle invalidate it, and on Apple silicon an unsigned
# binary will not execute at all.
if command -v codesign >/dev/null 2>&1; then
	codesign --force --sign - --timestamp=none "$APP" >/dev/null 2>&1 \
		&& echo "Signed (ad-hoc)." \
		|| echo "NOTE: ad-hoc codesign failed; the app may refuse to launch."
fi

echo "Built $APP"

if [ "$MODE" = build ]; then
	exit 0
fi

mkdir -p "$APPS_DIR"
rm -rf "$APPS_DIR/$APP_NAME"
cp -R "$APP" "$APPS_DIR/$APP_NAME"

LSREGISTER=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
[ -x "$LSREGISTER" ] && "$LSREGISTER" -f "$APPS_DIR/$APP_NAME" 2>/dev/null || true

echo
echo "Installed:"
echo "  $APPS_DIR/$APP_NAME"
echo "  $(du -sh "$APPS_DIR/$APP_NAME" | cut -f1) including the station dataset"
echo
echo "Open it from Launchpad/Spotlight, or: open '$APPS_DIR/$APP_NAME'"
