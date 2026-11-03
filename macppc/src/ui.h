/* The window: country list, station list, now-playing/status row. Port of the
 * Rust ports' ui.rs, which was the Haiku build's MainWindow (BWindow + two
 * BListViews + a status group).
 *
 * Both lists are virtualized and both have a filter box, for the same reasons
 * the Rust port gave: some countries have thousands of stations, and scrolling
 * 229 countries with no keyboard type-ahead is painful. Here the lists are
 * native Carbon views (see listview.h), so unlike egui they only repaint the
 * rows that changed. */
#ifndef RWR_UI_H
#define RWR_UI_H

typedef struct RadioUI RadioUI;

/* Opens the window and starts loading the dataset. Returns NULL if the window
 * could not be created; a missing dataset is reported in the status row rather
 * than being a failure to start. */
RadioUI *ui_create(void);
void     ui_dispose(RadioUI *ui);

#endif
