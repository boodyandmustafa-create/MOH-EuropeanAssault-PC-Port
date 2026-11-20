/**
 * XAudio2 Audio Output Backend
 */
#ifndef APU_XAUDIO2_H
#define APU_XAUDIO2_H

#include <stdint.h>

/* The device backend behind these functions: XAudio2 on Windows
 * (apu_xaudio2.c), SDL2 on POSIX hosts (apu_sdl2.c). Same contract. */
#if defined(_WIN32)
#define XA2_BACKEND_NAME "XAudio2"
#else
#define XA2_BACKEND_NAME "SDL2"
#endif

/* Initialize the device backend. Returns 1 on success, 0 on failure. */
int xa2_init(void);

/* Shut down XAudio2 and release all resources. */
void xa2_shutdown(void);

/* Returns 1 if XAudio2 is active. */
int xa2_is_active(void);

/* Submit interleaved stereo 16-bit samples. Returns 1 if accepted. */
int xa2_submit_samples(const int16_t *samples, int num_samples);

/* Get the preferred buffer size in samples. */
int xa2_get_buffer_size(void);

/* Number of device buffers the voice may hold. */
int xa2_get_buffer_count(void);

/* Samples handed over and not yet played (queued buffers plus the partly
 * filled one). Also counts underruns: see xa2_underruns. */
int xa2_queued_samples(void);

/* The queue is about to drain on purpose (the frame thread is pausing):
 * the dry spell that follows is not an underrun. */
void xa2_expect_drain(void);

/* Pacing marks in samples for count buffers of buf_samples, fed one period
 * (period samples) at a time. A period is submitted only while queued <
 * high, so high is capped where that period could fill a buffer the voice
 * has no slot for: queued + period must stay below (count + 1) buffers
 * (count on the voice, one staging). Exact in samples, so buffer sizes that
 * are not multiples of the period work too. low is half of high. */
static inline void xa2_pacing_marks(int buf_samples, int count, int period,
                                    int *low, int *high)
{
    int h = buf_samples * count;
    int cap = buf_samples * (count + 1) - period;
    if (h > cap) h = cap;
    if (h < 1) h = 1;
    *high = h;
    *low = h / 2;
}

/* Times the voice ran dry after audio started ([XA2] underrun x%u). */
unsigned xa2_underruns(void);

/* Samples the device has consumed since init: XAudio2's SamplesPlayed, or
 * on SDL2 everything queued minus what is still queued. Read against the
 * wall clock this is the host-side starvation measure: while the device
 * plays, it advances 48000 a second; whatever it falls behind is silence
 * the device inserted (an underrun the WAV tap, which records submissions,
 * cannot see). */
uint64_t xa2_samples_played(void);

/* Effect voices apart from the emulated APU's mix (Windows / XAudio2 only; elsewhere they do nothing). frames of interleaved
 * stereo 48 kHz 16-bit samples that stay valid while they play; returns a handle (> 0) or 0 when no voice is free. */
#if defined(_WIN32)
int  xa2_sfx_play(const int16_t *stereo48k, int frames, float vol, float pan, int loop);
void xa2_sfx_stop(int handle);
/* the breathing voice (kept apart from the others): one clip at a time, volume adjustable while it plays */
int  xa2_sfx_breath_busy(void);
int  xa2_sfx_breath_play(const int16_t *stereo48k, int frames, float vol);
void xa2_sfx_breath_volume(float vol);
void xa2_sfx_breath_stop(void);
#else
static inline int  xa2_sfx_breath_busy(void) { return 0; }
static inline int  xa2_sfx_breath_play(const int16_t *s, int f, float v) { (void)s; (void)f; (void)v; return 0; }
static inline void xa2_sfx_breath_volume(float v) { (void)v; }
static inline void xa2_sfx_breath_stop(void) { }
static inline int  xa2_sfx_play(const int16_t *s, int f, float v, float p, int l) { (void)s; (void)f; (void)v; (void)p; (void)l; return 0; }
static inline void xa2_sfx_stop(int h) { (void)h; }
#endif

#endif /* APU_XAUDIO2_H */
