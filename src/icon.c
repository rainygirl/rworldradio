#include "icon.h"

#include <Carbon/Carbon.h>

/* Generated into build/ by tools/mkicondata.py from
 * packaging/rworldradio-128.png; see the Makefile. */
#include "icon_png.h"

void icon_apply_to_dock(void)
{
    CFDataRef data;
    CGImageSourceRef source;
    CGImageRef image;

    data = CFDataCreateWithBytesNoCopy(NULL, RWR_ICON_PNG, sizeof(RWR_ICON_PNG),
                                       kCFAllocatorNull);
    if (!data)
        return;

    /* ImageIO is a sub-framework of ApplicationServices on 10.4, which is where
     * CGImageSource arrived - it is exactly at this port's floor, so there is no
     * older decoder to fall back to. Failing here just leaves the generic Dock
     * icon, which is cosmetic. */
    source = CGImageSourceCreateWithData(data, NULL);
    if (!source) {
        CFRelease(data);
        return;
    }

    image = CGImageSourceCreateImageAtIndex(source, 0, NULL);
    if (image) {
        SetApplicationDockTileImage(image);
        CGImageRelease(image);
    }

    CFRelease(source);
    CFRelease(data);
}
