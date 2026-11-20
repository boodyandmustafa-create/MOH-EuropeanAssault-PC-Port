/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Falls back gracefully if XAudio2 is unavailable.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apu_xaudio2.h"

/* The XAudio2 backend is Windows-only. On POSIX hosts apu_sdl2.c provides
 * the same xa2_* functions over SDL2. */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
/* Device buffer size and count. Defaults 512 x 4 (10.7 ms each, 43 ms
 * worst case queued); RECOMP_AUDIO_BUF_SAMPLES and RECOMP_AUDIO_BUF_COUNT
 * override them at xa2_init, clamped to the static storage below. */
#define XA2_BUF_SAMPLES_DEFAULT 512
#define XA2_NUM_BUFS_DEFAULT    4
#define XA2_BUF_SAMPLES_MIN     128
#define XA2_BUF_SAMPLES_MAX     4096
#define XA2_NUM_BUFS_MIN        2
#define XA2_NUM_BUFS_MAX        8

static int g_xa2_buf_samples = XA2_BUF_SAMPLES_DEFAULT;
static int g_xa2_num_bufs    = XA2_NUM_BUFS_DEFAULT;
#define XA2_BUF_SAMPLES g_xa2_buf_samples
#define XA2_NUM_BUFS    g_xa2_num_bufs

static int xa2_env_int(const char *name, int def, int lo, int hi)
{
    const char *e = getenv(name);
    int v;
    if (!e || !*e) return def;
    v = atoi(e);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
/* One slot more than can be queued: with every buffer on the voice, the
 * next one is staged in a slot the voice does not hold. With only
 * XA2_NUM_BUFS slots the staging buffer was the oldest queued one, and
 * filling it overwrote audio the device had not played yet. */
#define XA2_RING (XA2_NUM_BUFS + 1)
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS_MAX + 1][XA2_BUF_SAMPLES_MAX][2];
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_fill = 0;  /* samples staged in bufs[next_buf] */
static unsigned  g_xa2_underruns;
static int       g_xa2_dry;
static ULONGLONG g_xa2_underrun_logged;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;

int xa2_init(void)
{
    HRESULT hr;
    int com_initialized;
    WAVEFORMATEX wfx = { 0 };

    if (g_xa2_initialized) return 1;

    g_xa2_buf_samples = xa2_env_int("RECOMP_AUDIO_BUF_SAMPLES",
                                    XA2_BUF_SAMPLES_DEFAULT,
                                    XA2_BUF_SAMPLES_MIN, XA2_BUF_SAMPLES_MAX);
    g_xa2_num_bufs = xa2_env_int("RECOMP_AUDIO_BUF_COUNT", XA2_NUM_BUFS_DEFAULT,
                                 XA2_NUM_BUFS_MIN, XA2_NUM_BUFS_MAX);

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }
    com_initialized = SUCCEEDED(hr);

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        XA2_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = XA2_CHANNELS;
    wfx.nSamplesPerSec  = XA2_SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = XA2_CHANNELS * 2;
    wfx.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.nBlockAlign;

    hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
        &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] Start failed: 0x%08lX\n", hr);
        goto fail;
    }

    g_xa2_next_buf = 0;
    g_xa2_fill = 0;
    g_xa2_dry = 0;
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz stereo 16-bit, %d x %d-sample buffers, %.1f ms queued max)\n",
            XA2_SAMPLE_RATE, XA2_NUM_BUFS, XA2_BUF_SAMPLES,
            1000.0 * XA2_NUM_BUFS * XA2_BUF_SAMPLES / XA2_SAMPLE_RATE);
    return 1;

fail:
    xa2_shutdown();
    /* Failed initialization still runs on the COM-initializing thread. */
    if (com_initialized) CoUninitialize();
    return 0;
}

static void xa2_sfx_shutdown(void);

void xa2_shutdown(void)
{
    xa2_sfx_shutdown();
    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }

    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* Hand mixed samples to XAudio2. Called from the APU frame thread with one
 * EP period (256 samples) at a time. Samples accumulate in the current device
 * buffer, which is submitted when full, so a call shorter than a buffer is
 * kept, not padded or dropped. Returns 1 if every sample was accepted; 0 if
 * the voice already had every buffer queued when one filled, in which case
 * that buffer is discarded (pacing by queue depth is what prevents it). */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx, n, ok = 1;
    HRESULT hr;

    if (!g_xa2_initialized || !g_xa2_source) return 0;

    while (num_samples > 0) {
        idx = g_xa2_next_buf;
        n = XA2_BUF_SAMPLES - g_xa2_fill;
        if (n > num_samples) n = num_samples;
        memcpy(g_xa2_bufs[idx][g_xa2_fill], samples,
               n * XA2_CHANNELS * sizeof(int16_t));
        g_xa2_fill += n;
        samples += n * XA2_CHANNELS;
        num_samples -= n;
        if (g_xa2_fill < XA2_BUF_SAMPLES)
            break;

        g_xa2_fill = 0;
        IXAudio2SourceVoice_GetState(g_xa2_source, &state,
                                     XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if ((int)state.BuffersQueued >= XA2_NUM_BUFS) { ok = 0; continue; }

        memset(&xbuf, 0, sizeof(xbuf));
        xbuf.AudioBytes = XA2_BUF_SAMPLES * XA2_CHANNELS * sizeof(int16_t);
        xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];
        hr = IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);
        if (FAILED(hr)) { ok = 0; continue; }

        g_xa2_next_buf = (idx + 1) % XA2_RING;
        g_xa2_frames_written++;
    }
    return ok;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

int xa2_get_buffer_count(void)
{
    return XA2_NUM_BUFS;
}

/* Samples handed over and not yet played: the buffers the voice still
 * holds plus what is staged in the next one. Also the underrun detector:
 * the voice running dry after audio has started is one underrun, counted
 * once per dry spell and reported at most once a second. A drain announced
 * by xa2_expect_drain (the frame thread pausing) is not one. */
int xa2_queued_samples(void)
{
    XAUDIO2_VOICE_STATE state;

    if (!g_xa2_initialized || !g_xa2_source) return 0;
    IXAudio2SourceVoice_GetState(g_xa2_source, &state,
                                 XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (state.BuffersQueued == 0 && g_xa2_frames_written > 0) {
        if (!g_xa2_dry) {
            ULONGLONG now = GetTickCount64();
            g_xa2_dry = 1;
            g_xa2_underruns++;
            if (now - g_xa2_underrun_logged >= 1000) {
                g_xa2_underrun_logged = now;
                fprintf(stderr, "[XA2] underrun x%u\n", g_xa2_underruns);
            }
        }
    } else {
        g_xa2_dry = 0;
    }
    return (int)state.BuffersQueued * XA2_BUF_SAMPLES + g_xa2_fill;
}

/* Mark the coming dry spell as already seen; the first call that finds
 * buffers queued again clears it, so later dry spells count as before. */
void xa2_expect_drain(void)
{
    g_xa2_dry = 1;
}

unsigned xa2_underruns(void)
{
    return g_xa2_underruns;
}

uint64_t xa2_samples_played(void)
{
    XAUDIO2_VOICE_STATE state;

    if (!g_xa2_initialized || !g_xa2_source) return 0;
    IXAudio2SourceVoice_GetState(g_xa2_source, &state, 0);
    return state.SamplesPlayed;
}

/* ---- Effect voices (game sounds of our own: pain, breathing) ----
 * A few extra source voices on the same mastering voice, apart from the emulated APU's mix. XAudio2 mixes them on its own
 * thread, so playing one costs the game thread a few calls. The caller keeps the sample buffer alive while it plays
 * (the effects are loaded once at start-up and never freed). */
#define XA2_SFX_GENERAL 6                      /* voices 0..5 serve xa2_sfx_play; the last one is kept for the breathing loop */
#define XA2_SFX_VOICES  (XA2_SFX_GENERAL + 1)
static IXAudio2SourceVoice *g_sfx_v[XA2_SFX_VOICES];
static int                  g_sfx_loop[XA2_SFX_VOICES];
static int                  g_sfx_tried = 0;

static int xa2_sfx_ready(void)
{
    WAVEFORMATEX wfx = { 0 };
    int i;

    if (g_sfx_v[0]) return 1;
    if (g_sfx_tried || !g_xa2_initialized || !g_xa2) return 0;
    g_sfx_tried = 1;
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = XA2_SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = 4;
    wfx.nAvgBytesPerSec = XA2_SAMPLE_RATE * 4;
    for (i = 0; i < XA2_SFX_VOICES; i++) {
        if (FAILED(IXAudio2_CreateSourceVoice(g_xa2, &g_sfx_v[i], &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL))) {
            fprintf(stderr, "[XA2] effect voice %d failed\n", i);
            while (i-- > 0) { g_sfx_v[i]->lpVtbl->DestroyVoice(g_sfx_v[i]); g_sfx_v[i] = NULL; }
            return 0;
        }
        IXAudio2SourceVoice_Start(g_sfx_v[i], 0, XAUDIO2_COMMIT_NOW);
    }
    return 1;
}

/* Play interleaved stereo 48 kHz 16-bit frames on a free effect voice. vol 0..1+, pan -1 (left) .. 1 (right), loop != 0 plays
 * until xa2_sfx_stop. Returns a handle (voice + 1) or 0 when every voice is busy. */
int xa2_sfx_play(const int16_t *stereo48k, int frames, float vol, float pan, int loop)
{
    XAUDIO2_VOICE_STATE st;
    XAUDIO2_BUFFER xb;
    float m[4], l, r;
    int i;

    if (!stereo48k || frames <= 0 || !xa2_sfx_ready()) return 0;
    for (i = 0; i < XA2_SFX_GENERAL; i++) {
        IXAudio2SourceVoice_GetState(g_sfx_v[i], &st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (st.BuffersQueued == 0) break;
    }
    if (i == XA2_SFX_GENERAL) return 0;
    if (pan < -1.0f) pan = -1.0f;
    if (pan > 1.0f) pan = 1.0f;
    l = pan > 0.0f ? 1.0f - pan : 1.0f;             /* equal-gain balance: only the far side is turned down */
    r = pan < 0.0f ? 1.0f + pan : 1.0f;
    m[0] = l; m[1] = 0.0f; m[2] = 0.0f; m[3] = r;   /* L->L, R->L, L->R, R->R */
    g_sfx_v[i]->lpVtbl->SetVolume(g_sfx_v[i], vol, XAUDIO2_COMMIT_NOW);
    g_sfx_v[i]->lpVtbl->SetOutputMatrix(g_sfx_v[i], NULL, 2, 2, m, XAUDIO2_COMMIT_NOW);
    memset(&xb, 0, sizeof xb);
    xb.AudioBytes = (UINT32)frames * 4u;
    xb.pAudioData = (const BYTE *)stereo48k;
    if (loop) xb.LoopCount = XAUDIO2_LOOP_INFINITE;
    if (FAILED(IXAudio2SourceVoice_SubmitSourceBuffer(g_sfx_v[i], &xb, NULL))) return 0;
    g_sfx_loop[i] = loop ? 1 : 0;
    return i + 1;
}

/* The breathing voice: one clip at a time, volume changed while it plays (fade in / out). */
int xa2_sfx_breath_busy(void)
{
    XAUDIO2_VOICE_STATE st;
    if (!xa2_sfx_ready()) return 0;
    IXAudio2SourceVoice_GetState(g_sfx_v[XA2_SFX_GENERAL], &st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    return st.BuffersQueued != 0;
}

int xa2_sfx_breath_play(const int16_t *stereo48k, int frames, float vol)
{
    XAUDIO2_BUFFER xb;
    IXAudio2SourceVoice *v;
    if (!stereo48k || frames <= 0 || !xa2_sfx_ready() || xa2_sfx_breath_busy()) return 0;
    v = g_sfx_v[XA2_SFX_GENERAL];
    v->lpVtbl->SetVolume(v, vol, XAUDIO2_COMMIT_NOW);
    memset(&xb, 0, sizeof xb);
    xb.AudioBytes = (UINT32)frames * 4u;
    xb.pAudioData = (const BYTE *)stereo48k;
    return SUCCEEDED(IXAudio2SourceVoice_SubmitSourceBuffer(v, &xb, NULL));
}

void xa2_sfx_breath_volume(float vol)
{
    IXAudio2SourceVoice *v = g_sfx_v[XA2_SFX_GENERAL];
    if (v) v->lpVtbl->SetVolume(v, vol, XAUDIO2_COMMIT_NOW);
}

void xa2_sfx_breath_stop(void)
{
    IXAudio2SourceVoice *v = g_sfx_v[XA2_SFX_GENERAL];
    if (!v) return;
    IXAudio2SourceVoice_Stop(v, 0, XAUDIO2_COMMIT_NOW);
    IXAudio2SourceVoice_FlushSourceBuffers(v);
    IXAudio2SourceVoice_Start(v, 0, XAUDIO2_COMMIT_NOW);
}

static void xa2_sfx_shutdown(void)
{
    int i;
    for (i = 0; i < XA2_SFX_VOICES; i++) {
        if (g_sfx_v[i]) {
            IXAudio2SourceVoice_Stop(g_sfx_v[i], 0, XAUDIO2_COMMIT_NOW);
            g_sfx_v[i]->lpVtbl->DestroyVoice(g_sfx_v[i]);
            g_sfx_v[i] = NULL;
        }
    }
    g_sfx_tried = 0;
}

void xa2_sfx_stop(int handle)
{
    int i = handle - 1;
    if (i < 0 || i >= XA2_SFX_VOICES || !g_sfx_v[i]) return;
    IXAudio2SourceVoice_Stop(g_sfx_v[i], 0, XAUDIO2_COMMIT_NOW);
    IXAudio2SourceVoice_FlushSourceBuffers(g_sfx_v[i]);
    IXAudio2SourceVoice_Start(g_sfx_v[i], 0, XAUDIO2_COMMIT_NOW);
    g_sfx_loop[i] = 0;
}

#else /* !_WIN32: apu_sdl2.c implements the same contract over SDL2 */

/* ISO C wants at least one declaration in a translation unit. */
typedef int apu_xaudio2_posix_unused;

#endif /* _WIN32 */
