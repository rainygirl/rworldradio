/* CoreAudio output for Mac OS X 10.4 on PowerPC.
 *
 * This replaces the Rust ports' cpal path, and it deliberately does less than
 * they do. The Rust ports open the device's *default* config and convert into it
 * with a linear-interpolating resampler written in audio.rs. Here the
 * DefaultOutput audio unit is opened at the decoded stream's own rate and
 * channel count instead, and Apple's converter inside that unit does the rate
 * change and channel mapping. On a G4 that is the better trade twice over: the
 * converter is AltiVec-optimized where our C loop would not be, and it is a
 * proper polyphase resampler rather than linear interpolation.
 *
 * The cost is that the unit can only be opened once the first frame has been
 * decoded, since that is when the rate is known - which is exactly when the Rust
 * port opens its cpal stream too, so nothing is lost.
 *
 * The Component Manager calls (FindNextComponent / OpenAComponent) are the
 * 10.4 spelling; AudioComponentFindNext and friends did not arrive until 10.6.
 */

#include "audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>

/* How long the render callback may go without running before the device is
 * treated as dead. Two seconds is many times the largest buffer CoreAudio hands
 * out, so this cannot fire on a busy machine, only on a device that has stopped
 * pulling - headphones unplugged, the default output switched away. */
#define STALL_SECONDS 2.0

struct AudioOutput {
    AudioUnit  unit;
    int        opened;
    int        started;
    AudioSink *sink;

    /* Written by the render thread, read by the decode thread. A 32-bit counter
     * that only ever increments; a torn read would at worst delay the stall
     * check by one poll. */
    volatile unsigned render_ticks;

    /* Decode-thread-only state for the stall check. */
    unsigned last_seen_ticks;
    double   last_progress_time;

    char failure[256];
    int  have_failure;
};

static double now_seconds(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    return (double)now.tv_sec + (double)now.tv_usec / 1000000.0;
}

static OSStatus render(void *context,
                       AudioUnitRenderActionFlags *action_flags,
                       const AudioTimeStamp *timestamp,
                       UInt32 bus,
                       UInt32 frames,
                       AudioBufferList *data)
{
    AudioOutput *output = (AudioOutput *)context;
    UInt32 index;

    (void)action_flags;
    (void)timestamp;
    (void)bus;
    (void)frames;

    output->render_ticks++;

    /* The format set below is interleaved, so there is exactly one buffer
     * holding every channel. Fill any others with silence rather than assuming. */
    for (index = 0; index < data->mNumberBuffers; index++) {
        AudioBuffer *buffer = &data->mBuffers[index];
        size_t samples = buffer->mDataByteSize / sizeof(float);

        if (index == 0)
            audiosink_fill(output->sink, (float *)buffer->mData, samples);
        else
            memset(buffer->mData, 0, buffer->mDataByteSize);
    }
    return noErr;
}

AudioOutput *audiooutput_open(AudioSink *sink, int sample_rate, int channels,
                              char *error, size_t error_size)
{
    ComponentDescription description;
    Component component;
    AudioOutput *output;
    AudioStreamBasicDescription format;
    AURenderCallbackStruct callback;
    OSStatus status;

    if (sample_rate <= 0 || channels <= 0) {
        snprintf(error, error_size, "stream reported an impossible format (%d Hz, %d ch)",
                 sample_rate, channels);
        return NULL;
    }

    output = (AudioOutput *)calloc(1, sizeof(*output));
    if (!output) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    output->sink = audiosink_retain(sink);

    description.componentType = kAudioUnitType_Output;
    description.componentSubType = kAudioUnitSubType_DefaultOutput;
    description.componentManufacturer = kAudioUnitManufacturer_Apple;
    description.componentFlags = 0;
    description.componentFlagsMask = 0;

    component = FindNextComponent(NULL, &description);
    if (!component) {
        snprintf(error, error_size, "no default audio output unit");
        goto failed;
    }
    status = OpenAComponent(component, &output->unit);
    if (status != noErr) {
        snprintf(error, error_size, "could not open the output unit (OSStatus %ld)",
                 (long)status);
        goto failed;
    }
    output->opened = 1;

    callback.inputProc = render;
    callback.inputProcRefCon = output;
    status = AudioUnitSetProperty(output->unit, kAudioUnitProperty_SetRenderCallback,
                                  kAudioUnitScope_Input, 0, &callback, sizeof(callback));
    if (status != noErr) {
        snprintf(error, error_size, "could not set the render callback (OSStatus %ld)",
                 (long)status);
        goto failed;
    }

    /* Float32, packed, interleaved, native (big) endian on PPC. Matching the
     * unit's canonical format is what keeps our side of the callback a straight
     * memcpy with no conversion at all. */
    memset(&format, 0, sizeof(format));
    format.mSampleRate = (Float64)sample_rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    format.mChannelsPerFrame = (UInt32)channels;
    format.mBitsPerChannel = 32;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = (UInt32)(sizeof(float) * (size_t)channels);
    format.mBytesPerPacket = format.mBytesPerFrame;

    status = AudioUnitSetProperty(output->unit, kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Input, 0, &format, sizeof(format));
    if (status != noErr) {
        snprintf(error, error_size,
                 "the output unit rejected %d Hz / %d channel audio (OSStatus %ld)",
                 sample_rate, channels, (long)status);
        goto failed;
    }

    status = AudioUnitInitialize(output->unit);
    if (status != noErr) {
        snprintf(error, error_size, "could not initialize the output unit (OSStatus %ld)",
                 (long)status);
        goto failed;
    }

    status = AudioOutputUnitStart(output->unit);
    if (status != noErr) {
        AudioUnitUninitialize(output->unit);
        snprintf(error, error_size, "could not start audio output (OSStatus %ld)",
                 (long)status);
        goto failed;
    }
    output->started = 1;
    output->last_seen_ticks = output->render_ticks;
    output->last_progress_time = now_seconds();
    return output;

failed:
    if (output->opened)
        CloseComponent(output->unit);
    audiosink_release(output->sink);
    free(output);
    return NULL;
}

void audiooutput_close(AudioOutput *output)
{
    if (!output)
        return;
    if (output->started) {
        /* Mute first: the callback may be running right now, and this makes it
         * emit silence rather than whatever is still buffered. */
        audiosink_mute(output->sink);
        AudioOutputUnitStop(output->unit);
        AudioUnitUninitialize(output->unit);
    }
    if (output->opened)
        CloseComponent(output->unit);
    audiosink_release(output->sink);
    free(output);
}

const char *audiooutput_failure(AudioOutput *output)
{
    unsigned ticks;
    double now;

    if (!output || !output->started)
        return NULL;
    if (output->have_failure)
        return output->failure;

    ticks = output->render_ticks;
    now = now_seconds();
    if (ticks != output->last_seen_ticks) {
        output->last_seen_ticks = ticks;
        output->last_progress_time = now;
        return NULL;
    }
    if (now - output->last_progress_time > STALL_SECONDS) {
        snprintf(output->failure, sizeof(output->failure),
                 "the audio device stopped requesting samples");
        output->have_failure = 1;
        return output->failure;
    }
    return NULL;
}
