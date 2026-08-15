/* The Dock icon, for the case where there is no bundle to read one from.
 *
 * An installed copy gets its icon from the .app's Info.plist
 * (CFBundleIconFile -> Contents/Resources/rworldradio.icns, built by
 * packaging/make-icns.py). Running the binary straight out of build/ there is no
 * bundle, and the Dock falls back to the generic executable icon - so the same
 * artwork is compiled in and set at startup. This is what src/icon.rs does in
 * the Rust ports. */
#ifndef RWR_ICON_H
#define RWR_ICON_H

/* Safe to call unconditionally: when running from a bundle this replaces the
 * Dock tile with the identical image, and any failure leaves the tile alone. */
void icon_apply_to_dock(void);

#endif
