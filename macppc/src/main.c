/* R World Radio - Mac OS X 10.4 PowerPC port.
 *
 * The Rust ports (Linux, and the 10.15+ macOS one this was derived from) share a
 * single crate and differ in four small files. This port shares no code with
 * them at all, because none of what they are built on exists here: rustc has
 * never had a powerpc-apple-darwin target, and eframe/egui, winit, cpal and
 * rustls all require APIs that arrived long after Tiger. So this is a rewrite in
 * C against Carbon, CoreAudio, libmad, faad2 and a bundled mbedTLS - but a
 * rewrite that follows the Rust port module for module, because the parts that
 * matter (the HLS pipeline, the TS demuxer, the format detection, the session
 * lifecycle) are protocol handling rather than platform code.
 */

#include <Carbon/Carbon.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "icon.h"
#include "net.h"
#include "paths.h"
#include "ui.h"

/* No menu bar is installed here on purpose. The bold application menu - Services,
 * Hide, Show All and Quit - is built by the system for every GUI process. A menu
 * the app inserts itself does not become that menu, it lands beside it, which is
 * why an earlier version showed "R World Radio" twice with a File menu after it.
 * Nothing here needs a menu of its own: the window has a close box, and
 * Quit (Cmd+Q) arrives as kHICommandQuit, which RunApplicationEventLoop
 * already handles. */

int main(int argc, char **argv)
{
    RadioUI *ui;
    char ca_store[2048];

    (void)argc;
    (void)argv;

    /* The CA store is only opened on the first HTTPS connection - see
     * ensure_ca_chain in net.c - so a missing one costs nothing at launch and
     * surfaces as a per-station error rather than a startup failure. */
    paths_find_data_file("cacert.der", ca_store, sizeof(ca_store));
    net_init(ca_store);

    /* Only matters when running outside a bundle - an installed .app already has
     * its icon from Info.plist. See src/icon.h. */
    icon_apply_to_dock();

    ui = ui_create();
    if (!ui) {
        StandardAlert(kAlertStopAlert, "\pR World Radio",
                      "\pThe main window could not be created.", NULL, NULL);
        net_shutdown();
        return 1;
    }

    RunApplicationEventLoop();

    ui_dispose(ui);
    net_shutdown();
    return 0;
}
