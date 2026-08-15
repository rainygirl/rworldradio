/* Where the app's two data files live. Port of the Rust macOS port's paths.rs,
 * with stations.dat in place of the data/ tree of JSON.
 *
 * An .app bundle keeps its data inside itself under Contents/Resources, and
 * anything installed outside a bundle goes in ~/Library/Application Support.
 * Both are searched, plus the checkout layouts so the binary runs from build/
 * without being installed. */
#ifndef RWR_PATHS_H
#define RWR_PATHS_H

#include <stddef.h>

/* Writes the first existing candidate for `filename` into `out`. Returns 1 on
 * success, 0 if none of the candidates exist (in which case `out` holds the
 * highest-priority candidate, so an error message can name something useful). */
int paths_find_data_file(const char *filename, char *out, size_t size);

/* The search order as a human-readable list, for the status row's tooltip. */
void paths_describe_search_order(const char *filename, char *out, size_t size);

#endif
