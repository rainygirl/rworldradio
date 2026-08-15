/* A small horizontal LED-style peak-level bar: ten glossy rounded segments,
 * green/yellow/red by position - a classic vertical studio LED meter laid on its
 * side to fit one status row. Port of the Rust ports' level_meter.rs, which was
 * the Haiku build's LevelMeterView.
 *
 * The level is pushed in by the caller (0.0-1.0); this has no timer of its own.
 * Setting the same value twice does not repaint, which is what keeps a silent
 * passage from costing anything. */
#ifndef RWR_LEVELMETER_H
#define RWR_LEVELMETER_H

#include <Carbon/Carbon.h>

typedef struct LevelMeter LevelMeter;

LevelMeter *levelmeter_create(HIViewRef parent, HIViewRef *out_view);
void        levelmeter_dispose(LevelMeter *meter);
void        levelmeter_set_level(LevelMeter *meter, float level);

/* The size the meter wants, matching the Rust port's 80x18. */
#define LEVELMETER_WIDTH  80.0f
#define LEVELMETER_HEIGHT 18.0f

#endif
