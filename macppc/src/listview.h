/* A virtualized single-column list, as an HIView inside an HIScrollView.
 *
 * DataBrowser is the obvious Carbon control for this and is the wrong one here.
 * It owns its item list, so filtering as the user types means removing every
 * item and adding the survivors on each keystroke - and the largest country in
 * the dataset has 6,557 stations, which on a G4 makes typing visibly stutter.
 * This view holds no items at all: the delegate answers "how many rows" and
 * "what is row N", filtering is a rebuild of an int array in the caller, and
 * drawing touches only the ~30 rows actually on screen. That is the same
 * virtualization the Rust port gets from egui's ScrollArea::show_rows.
 *
 * Text is drawn with HIThemeDrawTextBox, so ATSUI does script-aware font
 * fallback for free - which is what this dataset needs, with station names in
 * Latin, Cyrillic, Greek, Arabic, Thai and all of CJK. The Rust ports need a
 * whole module (fonts.rs) to do the same job by hand. */
#ifndef RWR_LISTVIEW_H
#define RWR_LISTVIEW_H

#include <Carbon/Carbon.h>

typedef struct ListView ListView;

typedef struct {
    void *context;

    int (*row_count)(void *context);

    /* The row's text. The returned CFStringRef is BORROWED: the view neither
     * retains nor releases it, so the delegate must keep it alive until the next
     * listview_reload. Callers cache one CFStringRef per row rather than
     * creating them per draw, which is what keeps scrolling allocation-free. */
    CFStringRef (*row_text)(void *context, int row);

    /* -1 when the selection was cleared. */
    void (*selection_changed)(void *context, int row);

    /* Double-click or Return. */
    void (*row_activated)(void *context, int row);
} ListViewDelegate;

/* Creates the scroll view and its content, embedded in `parent`. Returns NULL on
 * failure. `out_scroll` receives the HIScrollView, which is what the caller
 * positions. */
ListView *listview_create(WindowRef window, HIViewRef parent,
                          const ListViewDelegate *delegate, HIViewRef *out_scroll);
void      listview_dispose(ListView *list);

/* Call after the row count or any row's text changed. Keeps the selection only
 * if it is still in range. */
void listview_reload(ListView *list);

int  listview_selection(const ListView *list);
void listview_set_selection(ListView *list, int row, int scroll_into_view);

/* Height of one row, for sizing calculations in the caller. */
float listview_row_height(const ListView *list);

#endif
