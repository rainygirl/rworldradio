/* The decoded-audio side of playback.
 *
 * AudioSink is the ring buffer between the decoder thread (producer) and the
 * CoreAudio render callback (consumer), plus the peak level the status row
 * reads. Port of the Rust port's audio.rs, minus its Converter: see
 * audio_ca.c for why the resampler is gone.
 *
 * AudioOutput is the CoreAudio side and lives in audio_ca.c. */
#ifndef RWR_AUDIO_H
#define RWR_AUDIO_H

#include <stddef.h>

typedef struct AudioSink   AudioSink;
typedef struct AudioOutput AudioOutput;

/* ------------------------------------------------------------------ sink -- */

/* Refcounted: the decoder thread, the render callback and the player's session
 * record all hold one. */
AudioSink *audiosink_new(int channels);
AudioSink *audiosink_retain(AudioSink *sink);
void       audiosink_release(AudioSink *sink);

/* Appends interleaved samples, blocking while the ring is full. This is what
 * paces the decoder to real time. Returns 0 once muted, which is the decoder's
 * signal to stop. */
int audiosink_push(AudioSink *sink, const float *samples, size_t count);

/* Fills `out` from the ring, padding with silence on underrun, and updates the
 * level meter. Called from the render callback: no allocation, short critical
 * section, never blocks. */
void audiosink_fill(AudioSink *sink, float *out, size_t count);

/* Silences output immediately and releases any blocked producer. */
void audiosink_mute(AudioSink *sink);

/* Peak amplitude (0.0-1.0) of the most recently played buffer. Cheap and safe to
 * poll from the UI thread every 100ms. */
float audiosink_level(AudioSink *sink);

/* ---------------------------------------------------------------- output -- */

/* Opens the default output device for `sample_rate`/`channels` and starts
 * pulling from `sink`. Returns NULL with a reason in `error` on failure.
 *
 * The unit is opened at the *stream's* rate rather than the device's: the
 * DefaultOutput unit contains Apple's own AltiVec-optimized converter, so
 * letting it do the rate change is both faster and better than the Rust port's
 * linear interpolation. */
AudioOutput *audiooutput_open(AudioSink *sink, int sample_rate, int channels,
                              char *error, size_t error_size);
void         audiooutput_close(AudioOutput *output);

/* First error the render path reported, or NULL. The decode loop polls this so a
 * dead output device ends the session instead of leaving the UI saying "Now
 * Playing" into silence. */
const char *audiooutput_failure(AudioOutput *output);

#endif
