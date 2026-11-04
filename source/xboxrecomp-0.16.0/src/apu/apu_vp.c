/*
 * MCPX APU Voice Processor - Standalone extraction from xemu
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "apu_state.h"
#include "apu.h"
#include "fpconv.h"

#ifdef _WIN32
#include <windows.h>
extern volatile LONG g_audio_mark;      /* video/fb_present.c, F8 */
#ifndef RECOMP_TLS
#define RECOMP_TLS __declspec(thread)
#endif
#define VEV_NOW() ((unsigned)GetTickCount())
#else
static volatile long g_audio_mark;
#define LONG long
#endif

extern volatile long g_apu_dbg_voice_off, g_apu_dbg_notify, g_apu_dbg_voice_on;

/* #define DEBUG_MCPX */

#ifdef DEBUG_MCPX
#define DPRINTF(fmt, ...) fprintf(stderr, fmt, ## __VA_ARGS__)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

/* ============================================================
 * Voice list register table
 * ============================================================ */

static const struct {
    hwaddr top, current, next;
} voice_list_regs[] = {
    { NV_PAPU_TVL2D, NV_PAPU_CVL2D, NV_PAPU_NVL2D }, /* 2D */
    { NV_PAPU_TVL3D, NV_PAPU_CVL3D, NV_PAPU_NVL3D }, /* 3D */
    { NV_PAPU_TVLMP, NV_PAPU_CVLMP, NV_PAPU_NVLMP }, /* MP */
};

/* ============================================================
 * Notify status helper
 * ============================================================ */

static void set_notify_status(MCPXAPUState *d, uint32_t v, int notifier,
                              int status)
{
    hwaddr notify_offset = d->regs[NV_PAPU_FENADDR];
    notify_offset += 16 * (MCPX_HW_NOTIFIER_BASE_OFFSET +
                           v * MCPX_HW_NOTIFIER_COUNT + notifier);
    notify_offset += 15;

    stb_phys(address_space_memory, notify_offset, (uint8_t)status);
    stb_phys(address_space_memory, notify_offset - 1, 1);
    InterlockedIncrement(&g_apu_dbg_notify);

    qatomic_or(&d->regs[NV_PAPU_ISTS],
               NV_PAPU_ISTS_FEVINTSTS | NV_PAPU_ISTS_FENINTSTS);
    d->set_irq = true;
}

/* ============================================================
 * Filter helpers
 * ============================================================ */

static void voice_reset_filters(MCPXAPUState *d, uint16_t v)
{
    assert(v < MCPX_HW_MAX_VOICES);
    memset(&d->vp.filters[v].svf, 0, sizeof(d->vp.filters[v].svf));
    d->vp.filters[v].gain_valid = false;     /* a new voice starts at its level */
    hrtf_filter_clear_history(&d->vp.filters[v].hrtf);
    if (d->vp.filters[v].resampler) {
        src_reset(d->vp.filters[v].resampler);
    }
    d->vp.filters[v].rs_have = 0;
    d->vp.filters[v].rs_pos = 0.0f;
}

static bool voice_should_mute(uint16_t v)
{
    bool m = (g_dbg_voice_monitor >= 0) && (v != g_dbg_voice_monitor);
    return m || mcpx_apu_debug_is_muted(v);
}

/* ============================================================
 * Utility functions
 * ============================================================ */

static float clampf(float v, float mn, float mx)
{
    if (v < mn) return mn;
    if (v > mx) return mx;
    return v;
}

static float attenuate(uint16_t vol)
{
    vol &= 0xFFF;
    return (vol == 0xFFF) ? 0.0f : powf(10.0f, vol / (64.0f * -20.0f));
}

/* ============================================================
 * Voice register accessors (read/write voice struct in RAM)
 * ============================================================ */

static uint32_t voice_get_mask(MCPXAPUState *d, uint16_t voice_handle,
                               hwaddr offset, uint32_t mask)
{
    hwaddr voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
    return (ldl_le_phys(address_space_memory, voice + offset) & mask) >>
           ctz32(mask);
}

static void voice_set_mask(MCPXAPUState *d, uint16_t voice_handle,
                           hwaddr offset, uint32_t mask, uint32_t val)
{
    hwaddr voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
    uint32_t v = ldl_le_phys(address_space_memory, voice + offset) & ~mask;
    stl_le_phys(address_space_memory, voice + offset,
                v | ((val << ctz32(mask)) & mask));
}

/* How many voices are active right now (the SNDWATCH line). -1 until the voice table is set up. */
int mcpx_apu_active_voices(MCPXAPUState *d)
{
    int n = 0;
    uint16_t v;

    if (!d->regs[NV_PAPU_VPVADDR])
        return -1;
    for (v = 0; v < MCPX_HW_MAX_VOICES; v++)
        if (voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE))
            n++;
    return n;
}

/* ============================================================
 * Voice off / lock
 * ============================================================ */

/* RECOMP_APU_EV=1 (or RECOMP_APU_3D): every voice start, release and stop
 * with the tick time, so a sound that vanishes can be told from one that
 * keeps playing. A voice that was started and never stopped while the
 * player hears nothing is the mixer's; one the game stops is the game's. */
extern ptrdiff_t xbox_GetMemoryOffset(void);
void pan_dump(const char *why);

/* The last 8 start / release / stop events of every voice, always kept (a few KB), so a voice that is stuck
 * playing can be read from the live process: apudump.py --hist shows when it was started and whether the
 * game ever released or stopped it. kind: 1 ON, 2 OFF, 3 RELEASE. */
typedef struct { uint32_t t, kind, fmt, ba, state, cbo; } ApuVEv;
volatile ApuVEv g_apu_vlog[256][8];
volatile uint32_t g_apu_vlog_n[256];

static void vev(const char *what, unsigned v, MCPXAPUState *d)
{
    static int on = -1;
    static int dumped;
    if (v < 256) {
        uint32_t n = g_apu_vlog_n[v]++ & 7u;
        g_apu_vlog[v][n].t = (uint32_t)GetTickCount();
        g_apu_vlog[v][n].kind = what[1] == 'N' ? 1u : what[1] == 'F' ? 2u : 3u;
        g_apu_vlog[v][n].fmt = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFF);
        g_apu_vlog[v][n].ba = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CUR_PSL_START, NV_PAVS_VOICE_CUR_PSL_START_BA);
        g_apu_vlog[v][n].state = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE, 0xFFFFFFFF);
        g_apu_vlog[v][n].cbo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO);
    }
    if (!dumped && v >= 68 && v < 224 && what[0] == 'O' && what[1] == 'N') {
        dumped = 1;
        pan_dump("first-effect-voice");
    }
    if (on < 0) on = apu_env_on("RECOMP_APU_EV") || apu_env_on("RECOMP_APU_3D");
    if (!on) return;
    unsigned fmt = (unsigned)voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFF);
    fprintf(stderr, "[VEV] t=%u %s v=%u fmt=%08X vol=%X,%X,%X,%X,%X,%X\n", VEV_NOW(), what, v, fmt,
            (unsigned)voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME0),
            (unsigned)voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME1),
            (unsigned)voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME2),
            (unsigned)voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME3),
            (unsigned)voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME4),
            (unsigned)voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME5));
}

/* ============================================================
 * Ring guard (2026-10-30)
 *
 * A title can mix its own sounds in software into one looping buffer and play that through a few
 * voices (MoH: 6 channels = 3 voices, music + dialogue + ambience). Seen live: a one-shot ADPCM format
 * (A000E18B) landed on such a voice while it played, the voice then ran off its end and stopped, the
 * title retired the buffer, and every software-mixed sound was gone until the game was restarted.
 * The game registers its ring voices (apu_ring_watch); while they play, config writes and VOICE_ON
 * aimed at them are dropped and logged with the last front-end commands. If they still end inside
 * the APU (not by a game stop), apu_ring_rearm restarts them and the mixer plays them even though
 * the title's DSound has unlinked them from its voice list. RECOMP_RING_GUARD=0 turns it off.
 * ============================================================ */
static void voice_spin_lock(MCPXAPUState *d, uint16_t v);
static void voice_spin_unlock(MCPXAPUState *d, uint16_t v);

static volatile LONG s_ring_watch;          /* ring voices registered and playing */
static uint16_t s_ring_v[3];
static uint32_t s_ring_fmt[3];
static volatile LONG s_ring_guest_stop;     /* the game sent RELEASE/OFF to a ring voice itself */
static volatile LONG s_ring_apu_end;        /* a ring voice ended inside the APU (not a game command) */
static volatile uint8_t s_ring_pin[MCPX_HW_MAX_VOICES];   /* mixed even when no voice list reaches it */
static uint32_t s_ring_visit[MCPX_HW_MAX_VOICES];
static RECOMP_TLS int t_fe_voice_off;      /* voice_off called from the front end (a game command) */
static volatile LONG s_ring_blocked;

typedef struct { uint32_t t, tid, meth, arg, cv; } FeTrace;
static FeTrace s_fe_trace[64];
static volatile LONG s_fe_trace_i;

static int ring_guard_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_RING_GUARD");
        on = !(e && *e == '0');
    }
    return on;
}

static int is_ring_voice(uint16_t v)
{
    return s_ring_watch && (v == s_ring_v[0] || v == s_ring_v[1] || v == s_ring_v[2]);
}

static void fe_trace_dump(const char *why)
{
    LONG end = s_fe_trace_i;
    uint32_t now = (uint32_t)GetTickCount();
    fprintf(stderr, "[RINGGUARD] last front-end commands before %s (oldest first: ms ago, thread, method, arg, current voice):\n", why);
    for (LONG k = end - 24; k < end; k++) {
        const FeTrace *f = &s_fe_trace[(k + 1) & 63];
        if (f->meth || f->arg)
            fprintf(stderr, "[RINGGUARD]   %4u ms tid %5u meth %03X arg %08X cv %u\n",
                    (unsigned)(now - f->t), (unsigned)f->tid, (unsigned)f->meth, (unsigned)f->arg, (unsigned)f->cv);
    }
}

/* Returns 1 when the command must not reach the voice. */
static int ring_guard(MCPXAPUState *d, uint32_t method, uint32_t argument)
{
    uint16_t v;
    switch (method) {
    case NV1BA0_PIO_VOICE_OFF:
    case NV1BA0_PIO_VOICE_RELEASE:
        v = (uint16_t)(argument & 0xFFFF);
        if (is_ring_voice(v) && !s_ring_apu_end) {
            /* the game stops its ring (level change, flush): trust it, stop guarding */
            InterlockedExchange(&s_ring_guest_stop, 1);
            InterlockedExchange(&s_ring_watch, 0);
            fprintf(stderr, "[RINGGUARD] t=%u the game stopped ring voice %u (%s): guard off until it plays again\n",
                    (unsigned)(GetTickCount() / 1000), v, method == NV1BA0_PIO_VOICE_OFF ? "OFF" : "RELEASE");
        }
        return 0;
    case NV1BA0_PIO_VOICE_ON:
        v = (uint16_t)(argument & NV1BA0_PIO_VOICE_ON_HANDLE);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_VBIN: case NV1BA0_PIO_SET_VOICE_CFG_FMT:
    case NV1BA0_PIO_SET_VOICE_CFG_ENV0: case NV1BA0_PIO_SET_VOICE_CFG_ENVA:
    case NV1BA0_PIO_SET_VOICE_CFG_ENV1: case NV1BA0_PIO_SET_VOICE_CFG_ENVF:
    case NV1BA0_PIO_SET_VOICE_CFG_MISC: case NV1BA0_PIO_SET_VOICE_SSL_A:
    case NV1BA0_PIO_SET_VOICE_SSL_B: case NV1BA0_PIO_SET_VOICE_CFG_BUF_BASE:
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_LBO: case NV1BA0_PIO_SET_VOICE_BUF_CBO:
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_EBO:
        v = (uint16_t)d->regs[NV_PAPU_FECV];
        break;
    default:
        return 0;
    }
    if (!is_ring_voice(v) || !voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE))
        return 0;
    {
        LONG n = InterlockedIncrement(&s_ring_blocked);
        if (n <= 40)
            fprintf(stderr, "[RINGGUARD] t=%u BLOCKED method %03X arg %08X aimed at playing ring voice %u (fmt now %08X) from thread %u\n",
                    (unsigned)(GetTickCount() / 1000), (unsigned)method, (unsigned)argument, v,
                    voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFF), (unsigned)GetCurrentThreadId());
        if (n <= 3)
            fe_trace_dump("the blocked command");
    }
    return 1;
}

/* Called by the game while its ring buffer plays (state started): remember its voices and their looping format. */
void apu_ring_watch(uint16_t v0, uint16_t v1, uint16_t v2)
{
    extern MCPXAPUState *g_state;
    MCPXAPUState *d = g_state;
    uint16_t vs[3] = { v0, v1, v2 };
    if (!d || !ring_guard_on())
        return;
    if (s_ring_watch && !s_ring_apu_end && s_ring_v[0] == v0 && s_ring_v[1] == v1 && s_ring_v[2] == v2)
        return;
    for (int i = 0; i < 3; i++) {
        if (vs[i] >= MCPX_HW_MAX_VOICES)
            return;
        if (!voice_get_mask(d, vs[i], NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE))
            return;                                  /* not playing (yet): look again on the next call */
        if (voice_get_mask(d, vs[i], NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_EACUR)
                >= NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE)
            return;                                  /* releasing: the game is stopping it, do not guard it again */
        if (!(voice_get_mask(d, vs[i], NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFF) & NV_PAVS_VOICE_CFG_FMT_LOOP))
            return;
    }
    for (int i = 0; i < 3; i++) {
        s_ring_v[i] = vs[i];
        s_ring_fmt[i] = voice_get_mask(d, vs[i], NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFF);
    }
    InterlockedExchange(&s_ring_guest_stop, 0);
    InterlockedExchange(&s_ring_apu_end, 0);
    InterlockedExchange(&s_ring_watch, 1);
    fprintf(stderr, "[RINGGUARD] t=%u guarding ring voices %u %u %u (fmt %08X %08X %08X)\n", (unsigned)(GetTickCount() / 1000),
            v0, v1, v2, s_ring_fmt[0], s_ring_fmt[1], s_ring_fmt[2]);
}

/* Called by the game when its ring lost its started state. Restarts the voices when they ended inside the APU (or the game
 * only stopped them after that); returns 0 when the game stopped them on its own (leave a real stop alone). */
int apu_ring_rearm(uint16_t v0, uint16_t v1, uint16_t v2)
{
    extern MCPXAPUState *g_state;
    MCPXAPUState *d = g_state;
    if (!d || !ring_guard_on())
        return 0;
    if (s_ring_v[0] != v0 || s_ring_v[1] != v1 || s_ring_v[2] != v2 || !s_ring_fmt[0])
        return 0;
    if (s_ring_guest_stop && !s_ring_apu_end)
        return 0;
    fprintf(stderr, "[RING] t=%u restarting ring voices %u %u %u (fmt was %08X state %08X cbo %u)\n",
            (unsigned)(GetTickCount() / 1000), v0, v1, v2, voice_get_mask(d, v0, NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFF),
            voice_get_mask(d, v0, NV_PAVS_VOICE_PAR_STATE, 0xFFFFFFFF),
            voice_get_mask(d, v0, NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO));
    for (int i = 0; i < 3; i++) {
        uint16_t v = s_ring_v[i];
        voice_spin_lock(d, v);
        voice_set_mask(d, v, NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFF, s_ring_fmt[i]);
        voice_set_mask(d, v, NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO, 0);
        voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, 0xFFFFFFFF, NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
        s_ring_pin[v] = 1;
        voice_spin_unlock(d, v);
    }
    InterlockedExchange(&s_ring_guest_stop, 0);
    InterlockedExchange(&s_ring_apu_end, 0);
    InterlockedExchange(&s_ring_watch, 1);
    return 1;
}

static void voice_off(MCPXAPUState *d, uint16_t v)
{
    if (!t_fe_voice_off && is_ring_voice(v) && !s_ring_guest_stop) {
        if (!InterlockedExchange(&s_ring_apu_end, 1)) {
            fprintf(stderr, "[RINGGUARD] t=%u ring voice %u ended inside the APU (fmt %08X, cbo %u): not a game stop\n",
                    (unsigned)(GetTickCount() / 1000), v, voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFF),
                    voice_get_mask(d, v, NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO));
            fe_trace_dump("the ring voice ended");
        }
    }
    vev("OFF", v, d);
    if (InterlockedIncrement(&g_apu_dbg_voice_off) <= 40)
        fprintf(stderr, "[APU] VOICE_OFF voice %u (notifier base 0x%08X)\n",
                v, d->regs[NV_PAPU_FENADDR]);
    voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                   NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 0);

    bool stream = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0;
    int notifier = MCPX_HW_NOTIFIER_SSLA_DONE;
    if (stream) {
        assert(v < MCPX_HW_MAX_VOICES);
        assert(d->vp.ssl[v].ssl_index <= 1);
        notifier += d->vp.ssl[v].ssl_index;
    }
    set_notify_status(d, v, notifier, NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
    /* And the voice-off slot, which nothing wrote before: a flushed stream
     * waited on it forever, and with it every level load behind the menu
     * music's flush. */
    set_notify_status(d, v, MCPX_HW_NOTIFIER_VOICE_OFF,
                      NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
}

/* Per voice, as in xemu. A title's VOICE_LOCK arrives on its own thread while
 * the frame thread holds d->lock across every voice of a frame, so taking
 * d->lock here made each lock wait out the whole frame. The title only has to
 * wait while the voice processor is working on the voice it locks. */
static void voice_spin_lock(MCPXAPUState *d, uint16_t v)
{
    while (InterlockedCompareExchange(&d->vp.voice_spin[v], 1, 0))
        SwitchToThread();
}

static void voice_spin_unlock(MCPXAPUState *d, uint16_t v)
{
    InterlockedExchange(&d->vp.voice_spin[v], 0);
}

static void voice_lock(MCPXAPUState *d, uint16_t v, bool lock)
{
    assert(v < MCPX_HW_MAX_VOICES);
    voice_spin_lock(d, v);
    d->vp.voice_locked[v] = lock;
    voice_spin_unlock(d, v);
    qemu_cond_signal(&d->cond);
}

static bool is_voice_locked(MCPXAPUState *d, uint16_t v)
{
    assert(v < MCPX_HW_MAX_VOICES);
    return d->vp.voice_locked[v] != 0;
}

/* ============================================================
 * HRIR coefficient setter
 * ============================================================ */

static void set_hrir_coeff_tar(MCPXAPUState *d, int channel, int coeff_idx,
                               int8_t value)
{
    int entry = d->vp.hrtf.current_entry;
    d->vp.hrtf.entries[entry].hrir[channel][coeff_idx] = int8_to_float(value);
}

/* ============================================================
 * Front-End method dispatch
 * ============================================================ */

/* RECOMP_APU_VOLTRACE=1: who writes the voice volumes. Prints, for each
 * distinct guest call chain (return-address-looking words on the guest
 * stack), the voice and the value written; the game function above DSOUND
 * that computes the pan is in there. Runs on whichever thread handles the
 * FE write: g_esp is that thread's guest stack pointer, 0 elsewhere. */
extern RECOMP_TLS uint32_t g_esp;
extern ptrdiff_t xbox_GetMemoryOffset(void);

static void vol_trace(MCPXAPUState *d, char kind, uint32_t argument)
{
    static int on = -1;
    static uint32_t seen[256];
    static int nseen;
    if (on < 0) on = apu_env_on("RECOMP_APU_VOLTRACE");
    if (!on) return;
    uint32_t esp = g_esp;
    if (!esp) return;
    const uint8_t *base = (const uint8_t *)(uintptr_t)xbox_GetMemoryOffset();
    uint32_t cand[10]; int nc = 0; uint32_t h = 2166136261u;
    for (int i = 0; i < 160 && nc < 10; i++) {
        uint32_t w = *(const uint32_t *)(base + esp + i * 4);
        /* .text only (.data starts at 0x3727C0), and only words right after
         * a CALL: E8 rel32, FF D0-D7, FF 50-57 d8, FF 14 SIB d8/none, FF 15
         * or FF 90-97 d32. */
        if (w < 0x00011000u + 6 || w >= 0x00370000u) continue;
        const uint8_t *c = base + w;
        int call = c[-5] == 0xE8
            || (c[-2] == 0xFF && (c[-1] & 0xF8) == 0xD0)
            || (c[-3] == 0xFF && (c[-2] & 0xF8) == 0x50)
            || (c[-3] == 0xFF && c[-2] == 0x14)
            || (c[-6] == 0xFF && (c[-5] == 0x15 || (c[-5] & 0xF8) == 0x90));
        if (!call) continue;
        cand[nc++] = w; h = (h ^ w) * 16777619u;
    }
    if (!nc) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "[VOLTRACE] no return address found on the guest "
                            "stack (esp=%08X)\n", esp);
        return;
    }
    for (int i = 0; i < nseen; i++) if (seen[i] == h) return;
    if (nseen >= 256) return;
    seen[nseen++] = h;
    fprintf(stderr, "[VOLTRACE] vol%c v=%u val=%08X chain:", kind,
            (unsigned)d->regs[NV_PAPU_FECV], argument);
    for (int i = 0; i < nc; i++) fprintf(stderr, " %08X", cand[i]);
    fprintf(stderr, "\n");
}

/* RECOMP_APU_3D: MoH's pan tables. The game turns a source's angle (a byte,
 * 0..255 round the listener) into one gain per speaker with six 256-byte
 * tables (pointers at 0x523C84, channel count at 0x523A5F, speaker layout at
 * 0x523C74, built once by sub_00197CFD). Left and right should mirror each
 * other; this prints them (every 8th entry, hex) to check. Called on the
 * first effect voice and on every F8. */
void pan_dump(const char *why)
{
    static int on = -1;
    if (on < 0) on = apu_env_on("RECOMP_APU_3D");
    if (!on) return;
    const uint8_t *base = (const uint8_t *)(uintptr_t)xbox_GetMemoryOffset();
    if (!base) return;
    unsigned nch = base[0x523A5F], mode = base[0x523C74];
    fprintf(stderr, "[PANTAB] %s t=%u nch=%u layout=%u ang=", why, VEV_NOW(), nch, mode);
    for (int i = 0; i < 6; i++)
        fprintf(stderr, "%.4f%s", *(const float *)(base + 0x523C5C + i * 4), i < 5 ? "," : "");
    fprintf(stderr, "\n");
    for (unsigned ch = 0; ch < nch && ch < 6; ch++) {
        uint32_t p = *(const uint32_t *)(base + 0x523C84 + ch * 4);
        if (p < 0x10000u || p > 0x08000000u) {
            fprintf(stderr, "[PANTAB] ch%u pointer %08X invalid\n", ch, p);
            continue;
        }
        fprintf(stderr, "[PANTAB] ch%u @%08X:", ch, p);
        for (int i = 0; i < 256; i += 8)
            fprintf(stderr, " %02X", base[p + i]);
        fprintf(stderr, "\n");
    }
}

/* Commands the front end has taken since start and the tick of the last one, readable from outside
 * (apudump.py --hist): a game whose sound stopped after a stuck voice shows here whether it stopped sending. */
volatile uint32_t g_apu_fe_calls, g_apu_fe_last_tick;

static void fe_method(MCPXAPUState *d, uint32_t method, uint32_t argument)
{
    unsigned int slot;

    g_apu_fe_calls++;
    g_apu_fe_last_tick = (uint32_t)GetTickCount();
    {
        FeTrace *f = &s_fe_trace[InterlockedIncrement(&s_fe_trace_i) & 63];
        f->t = g_apu_fe_last_tick;
        f->tid = (uint32_t)GetCurrentThreadId();
        f->meth = method;
        f->arg = argument;
        f->cv = d->regs[NV_PAPU_FECV];
    }
    if (s_ring_watch && ring_guard(d, method, argument))
        return;
    d->regs[NV_PAPU_FEDECMETH] = method;
    d->regs[NV_PAPU_FEDECPARAM] = argument;
    unsigned int selected_handle, list;

    switch (method) {
    case NV1BA0_PIO_VOICE_LOCK:
        voice_lock(d, (uint16_t)d->regs[NV_PAPU_FECV], argument & 1);
        break;

    case NV1BA0_PIO_SET_ANTECEDENT_VOICE:
        d->regs[NV_PAPU_FEAV] = argument;
        break;

    case NV1BA0_PIO_VOICE_ON: {
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;
        vev("ON", selected_handle, d);
        InterlockedIncrement(&g_apu_dbg_voice_on);

        bool locked = is_voice_locked(d, (uint16_t)selected_handle);
        if (!locked) voice_lock(d, (uint16_t)selected_handle, true);

        list = GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_LST);
        if (list != NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_INHERIT) {
            unsigned int top_reg = voice_list_regs[list - 1].top;
            voice_set_mask(d, (uint16_t)selected_handle,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           d->regs[top_reg]);
            d->regs[top_reg] = selected_handle;
        } else {
            unsigned int antecedent_voice =
                GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_VALUE);
            assert(antecedent_voice != 0xFFFF);

            uint32_t next_handle = voice_get_mask(
                d, (uint16_t)antecedent_voice, NV_PAVS_VOICE_TAR_PITCH_LINK,
                NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
            voice_set_mask(d, (uint16_t)selected_handle,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           next_handle);
            voice_set_mask(d, (uint16_t)antecedent_voice,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           selected_handle);
        }

        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_OFFSET,
                       NV_PAVS_VOICE_PAR_OFFSET_CBO, 0);
        d->vp.ssl[selected_handle].ssl_seg = 0;
        d->vp.ssl[selected_handle].ssl_index = 0;

        unsigned int ea_start = GET_MASK(argument, NV1BA0_PIO_VOICE_ON_ENVA);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EACUR, ea_start);
        if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY) {
            uint16_t delay_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENV0, NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, delay_time * 16);
        } else if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK) {
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, 0);
        } else if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD) {
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENVA, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, hold_time * 16);
        }

        unsigned int ef_start = GET_MASK(argument, NV1BA0_PIO_VOICE_ON_ENVF);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR, ef_start);
        if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY) {
            uint16_t delay_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENV1, NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, delay_time * 16);
        } else if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK) {
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, 0);
        } else if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD) {
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENVF, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, hold_time * 16);
        }

        voice_reset_filters(d, (uint16_t)selected_handle);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 1);

        if (!locked) voice_lock(d, (uint16_t)selected_handle, false);
        break;
    }

    case NV1BA0_PIO_VOICE_RELEASE: {
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;
        vev("RELEASE", selected_handle, d);

        bool locked = is_voice_locked(d, (uint16_t)selected_handle);
        if (!locked) voice_lock(d, (uint16_t)selected_handle, true);

        uint16_t rr;
        rr = (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
            NV_PAVS_VOICE_TAR_LFO_ENV, NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                       NV_PAVS_VOICE_CUR_ECNT_EACOUNT, rr * 16);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EACUR,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE);

        rr = (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
            NV_PAVS_VOICE_CFG_MISC, NV_PAVS_VOICE_CFG_MISC_EF_RELEASERATE);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                       NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, rr * 16);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE);

        if (!locked) voice_lock(d, (uint16_t)selected_handle, false);
        break;
    }

    case NV1BA0_PIO_VOICE_OFF:
        t_fe_voice_off = 1;
        voice_off(d, (uint16_t)(argument & NV1BA0_PIO_VOICE_OFF_HANDLE));
        t_fe_voice_off = 0;
        break;

    case NV1BA0_PIO_VOICE_PAUSE:
        voice_set_mask(d, (uint16_t)(argument & NV1BA0_PIO_VOICE_PAUSE_HANDLE),
                       NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_PAUSED,
                       (argument & NV1BA0_PIO_VOICE_PAUSE_ACTION) != 0);
        break;

    case NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY:
        d->vp.hrtf.current_entry =
            GET_MASK(argument, NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY_HANDLE);
        break;

    case NV1BA0_PIO_SET_CURRENT_VOICE:
        d->regs[NV_PAPU_FECV] = argument;
        break;

    case NV1BA0_PIO_SET_VOICE_CFG_VBIN:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_VBIN, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_FMT:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENV0:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENV0, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENVA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENVA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENV1:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENV1, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENVF:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENVF, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_MISC:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_MISC, 0xFFFFFFFF, argument);
        break;

    case NV1BA0_PIO_SET_VOICE_TAR_HRTF: {
        int handle = GET_MASK(argument, NV1BA0_PIO_SET_VOICE_TAR_HRTF_HANDLE);
        int current_voice = d->regs[NV_PAPU_FECV];
        voice_set_mask(d, (uint16_t)current_voice,
                       NV_PAVS_VOICE_CFG_HRTF_TARGET,
                       NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE, handle);
        if (current_voice < MCPX_HW_MAX_3D_VOICES &&
            handle != HRTF_NULL_HANDLE) {
            assert(handle < HRTF_ENTRY_COUNT);
            hrtf_filter_set_target_params(&d->vp.filters[current_voice].hrtf,
                                          d->vp.hrtf.entries[handle].hrir,
                                          d->vp.hrtf.entries[handle].itd);
        }
        break;
    }

    case NV1BA0_PIO_SET_VOICE_TAR_VOLA:
        vol_trace(d, 'A', argument);
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLB:
        vol_trace(d, 'B', argument);
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLB, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLC:
        vol_trace(d, 'C', argument);
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLC, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_LFO_ENV:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_LFO_ENV, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_FCA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_FCA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_FCB:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_FCB, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_PITCH:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_PITCH_LINK,
                       NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH,
                       (argument & NV1BA0_PIO_SET_VOICE_TAR_PITCH_STEP) >> 16);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_BASE:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CUR_PSL_START,
                       NV_PAVS_VOICE_CUR_PSL_START_BA, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_LBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                       NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_BUF_CBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_PAR_OFFSET,
                       NV_PAVS_VOICE_PAR_OFFSET_CBO, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_EBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_PAR_NEXT,
                       NV_PAVS_VOICE_PAR_NEXT_EBO, argument);
        break;

    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE:
        d->vp.inbuf_sge_handle = argument & NV1BA0_PIO_SET_CURRENT_INBUF_SGE_HANDLE;
        break;

    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET: {
        hwaddr sge_address =
            d->regs[NV_PAPU_VPSGEADDR] + d->vp.inbuf_sge_handle * 8;
        stl_le_phys(address_space_memory, sge_address,
                    argument & NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET_PARAMETER);
        break;
    }

    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE:
        d->vp.outbuf_sge_handle =
            argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_HANDLE;
        break;

    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET: {
        hwaddr sge_address =
            d->regs[NV_PAPU_VPSGEADDR] + d->vp.outbuf_sge_handle * 8;
        stl_le_phys(address_space_memory, sge_address,
                    argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET_PARAMETER);
        break;
    }

    case NV1BA0_PIO_SET_VOICE_SSL_A: {
        int ssl = 0;
        int current_voice = d->regs[NV_PAPU_FECV];
        assert(current_voice < MCPX_HW_MAX_VOICES);
        d->vp.ssl[current_voice].base[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_BASE);
        d->vp.ssl[current_voice].count[ssl] =
            (uint8_t)GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_COUNT);
        break;
    }
    case NV1BA0_PIO_SET_VOICE_SSL_B: {
        int ssl = 1;
        int current_voice = d->regs[NV_PAPU_FECV];
        assert(current_voice < MCPX_HW_MAX_VOICES);
        d->vp.ssl[current_voice].base[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_BASE);
        d->vp.ssl[current_voice].count[ssl] =
            (uint8_t)GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_COUNT);
        break;
    }

    case NV1BA0_PIO_SET_CURRENT_SSL: {
        assert((argument & 0x3f) == 0);
        assert(argument < (MCPX_HW_MAX_SSL_PRDS * NV_PSGE_SIZE));
        d->vp.ssl_base_page = argument;
        break;
    }

    case NV1BA0_PIO_SET_HRTF_SUBMIXES:
        d->vp.hrtf_submix[0] = (uint8_t)((argument >> 0) & 0x1f);
        d->vp.hrtf_submix[1] = (uint8_t)((argument >> 8) & 0x1f);
        d->vp.hrtf_submix[2] = (uint8_t)((argument >> 16) & 0x1f);
        d->vp.hrtf_submix[3] = (uint8_t)((argument >> 24) & 0x1f);
        break;

    case NV1BA0_PIO_SET_HRTF_HEADROOM:
        d->vp.hrtf_headroom = (uint8_t)(argument & NV1BA0_PIO_SET_HRTF_HEADROOM_AMOUNT);
        break;

    case SE2FE_IDLE_VOICE:
        if (d->regs[NV_PAPU_FETFORCE1] & NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE) {
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FEMETHMODE;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FEMETHMODE_TRAPPED;
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FETRAPREASON;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FETRAPREASON_REQUESTED;
            /* Latched here, once per trap; update_irq no longer re-derives
             * it from FECTL (see there). */
            qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_FETINTSTS);
            d->set_irq = true;
            apu_irq_stats.traps++;
        }
        break;

    default:
        /* Handle range-based cases that can't use case ranges in MSVC */
        if (method >= NV1BA0_PIO_SET_HRIR && method < NV1BA0_PIO_SET_HRIR_X) {
            assert(d->vp.hrtf.current_entry < HRTF_ENTRY_COUNT);
            slot = (method - NV1BA0_PIO_SET_HRIR) / 4;
            int8_t left0 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_LEFT0);
            int8_t right0 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_RIGHT0);
            int8_t left1 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_LEFT1);
            int8_t right1 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_RIGHT1);
            int coeff_idx = slot * 2;
            set_hrir_coeff_tar(d, 0, coeff_idx, left0);
            set_hrir_coeff_tar(d, 1, coeff_idx, right0);
            set_hrir_coeff_tar(d, 0, coeff_idx + 1, left1);
            set_hrir_coeff_tar(d, 1, coeff_idx + 1, right1);
        } else if (method == NV1BA0_PIO_SET_HRIR_X) {
            assert(d->vp.hrtf.current_entry < HRTF_ENTRY_COUNT);
            int8_t left30 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_LEFT30);
            int8_t right30 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_RIGHT30);
            int16_t itd = (int16_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_ITD);
            set_hrir_coeff_tar(d, 0, 30, left30);
            set_hrir_coeff_tar(d, 1, 30, right30);
            d->vp.hrtf.entries[d->vp.hrtf.current_entry].itd = s6p9_to_float(itd);
        } else if (method >= NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET &&
                   method < NV1BA0_PIO_SET_SSL_SEGMENT_LENGTH + 8 * 64) {
            assert((method & 0x3) == 0);
            hwaddr addr = d->regs[NV_PAPU_VPSSLADDR]
                          + (d->vp.ssl_base_page * 8)
                          + (method - NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET);
            stl_le_phys(address_space_memory, addr, argument);
        } else if (method >= NV1BA0_PIO_SET_SUBMIX_HEADROOM &&
                   method <= NV1BA0_PIO_SET_SUBMIX_HEADROOM + 4 * (NUM_MIXBINS - 1)) {
            assert((method & 3) == 0);
            slot = (method - NV1BA0_PIO_SET_SUBMIX_HEADROOM) / 4;
            d->vp.submix_headroom[slot] =
                (uint8_t)(argument & NV1BA0_PIO_SET_SUBMIX_HEADROOM_AMOUNT);
            if (apu_env_on("RECOMP_APU_3D"))
                fprintf(stderr, "[HEADROOM] bin %d = %u\n", slot,
                        (unsigned)d->vp.submix_headroom[slot]);
        } else if ((method >= NV1BA0_PIO_SET_OUTBUF_BA &&
                    method < NV1BA0_PIO_SET_OUTBUF_BA + 32) ||
                   (method >= NV1BA0_PIO_SET_OUTBUF_LEN &&
                    method < NV1BA0_PIO_SET_OUTBUF_LEN + 32)) {
            /* Outbuf base/length - ignore for now */
        } else {
            /* Unknown method - silently ignore */
            DPRINTF("Unknown FE method: 0x%08X arg=0x%08X\n", method, argument);
        }
        break;
    }
}

/* ============================================================
 * VP MMIO read/write (exposed to apu_core.c)
 * ============================================================ */

uint64_t mcpx_apu_vp_read(void *opaque, hwaddr addr, unsigned int size)
{
    (void)opaque; (void)size;

    switch (addr) {
    case NV1BA0_PIO_FREE:
        return 0x80; /* Always pretend queue is empty */
    default:
        break;
    }
    return 0;
}

void mcpx_apu_vp_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    (void)size;

    /* Dispatch known methods through fe_method */
    fe_method(d, (uint32_t)addr, (uint32_t)val);
}

/* ============================================================
 * SGE data pointer resolution
 * ============================================================ */

static hwaddr get_data_ptr(hwaddr sge_base, unsigned int max_sge, uint32_t addr)
{
    unsigned int entry = addr / TARGET_PAGE_SIZE;
    assert(entry <= max_sge);
    uint32_t prd_address =
        ldl_le_phys(address_space_memory, sge_base + entry * 4 * 2);
    return prd_address + addr % TARGET_PAGE_SIZE;
}

/* ============================================================
 * Envelope processing
 * ============================================================ */

static float voice_step_envelope(MCPXAPUState *d, uint16_t v, uint32_t reg_0,
                           uint32_t reg_a, uint32_t rr_reg, uint32_t rr_mask,
                           uint32_t lvl_reg, uint32_t lvl_mask,
                           uint32_t count_mask, uint32_t cur_mask)
{
    uint8_t cur = (uint8_t)voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask);
    switch (cur) {
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_OFF:
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, 0);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0xFF);
        return 1.0f;

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0x00);
        if (count == 0) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
        } else {
            count--;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return 0.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t attack_rate =
            (uint16_t)voice_get_mask(d, v, reg_0, NV_PAVS_VOICE_CFG_ENV0_EA_ATTACKRATE);
        float value;
        if (attack_rate == 0) {
            value = 255.0f;
        } else {
            if (count <= (uint32_t)(attack_rate * 16)) {
                value = (count * 0xFF) / (float)(attack_rate * 16);
            } else {
                value = 255.0f;
            }
        }
        voice_set_mask(d, v, lvl_reg, lvl_mask, (uint32_t)value);
        if (count == (uint32_t)(attack_rate * 16)) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            count = hold_time * 16;
        } else {
            count++;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0xFF);
        if (count == 0) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
            uint16_t decay_rate =
                (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_DECAYRATE);
            count = decay_rate * 16;
        } else {
            count--;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return 1.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_DECAY: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t decay_rate =
            (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_DECAYRATE);
        uint8_t sustain_level =
            (uint8_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL);
        float value;
        if (decay_rate == 0) {
            value = 0.0f;
        } else {
            value = 255.0f * powf(0.99988799f, (decay_rate * 16 - count) *
                                                   4096.0f / decay_rate);
        }
        if (value <= (sustain_level + 0.2f) || (value > 255.0f)) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
        } else {
            count--;
            voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
            voice_set_mask(d, v, lvl_reg, lvl_mask, (uint32_t)value);
        }
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_SUSTAIN: {
        uint8_t sustain_level =
            (uint8_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL);
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, 0x00);
        voice_set_mask(d, v, lvl_reg, lvl_mask, sustain_level);
        return sustain_level / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t release_rate = (uint16_t)voice_get_mask(d, v, rr_reg, rr_mask);
        if (release_rate == 0) count = 0;
        float value = 0;
        if (count == 0) {
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, ++cur);
        } else {
            float pos = clampf(1 - count / (release_rate * 16.0f), 0, 1);
            uint8_t lvl = (uint8_t)voice_get_mask(d, v, lvl_reg, lvl_mask);
            value = powf((float)M_E, -6.91f * pos) * lvl;
            count--;
            voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        }
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_FORCE_RELEASE:
        if (count_mask == NV_PAVS_VOICE_CUR_ECNT_EACOUNT) {
            voice_off(d, v);
        }
        return 0.0f;

    default:
        fprintf(stderr, "[APU] Unknown envelope state 0x%x\n", cur);
        return 0.0f;
    }
}

/* ============================================================
 * Per-voice source statistics (RECOMP_APU_TRACE)
 *
 * What each voice fed the mixer over the last second: its format word, where
 * its data was read from and how that address resolved, the amplitude
 * envelope and the first two volumes/bins, plus the RMS and zero-crossing
 * rate of the fetched samples. The zero-crossing rate separates music
 * (~0.1-0.2) from white noise (~0.5) without a spectrum, so a voice that
 * reads garbage shows up by itself.
 *
 * RECOMP_APU_VOICE_DUMP=<voice> also writes that voice's fetched samples,
 * before gain, as raw s16 stereo 48 kHz to RECOMP_APU_VOICE_DUMP_FILE
 * (default voice_dump.raw in the working directory).
 * ============================================================ */

struct VpVoiceStat {
    uint32_t frames, zc;
    double   sq;
    float    last, ea, rate;
    uint32_t addr, fmt;
    uint16_t vol0, vol1;
    uint8_t  bin0, bin1;
    const char *cls;
};
static struct VpVoiceStat s_vstat[MCPX_HW_MAX_VOICES];
static int s_vstat_on = -1;
static int s_vdump_voice = -1;      /* -2: every voice, one file each */
static FILE *s_vdump;
static FILE *s_vdump_all[MCPX_HW_MAX_VOICES];
static uint8_t s_vdump_failed[MCPX_HW_MAX_VOICES]; /* open failed: no retry */
static char s_vdump_prefix[512];
static unsigned s_vstat_frames, s_vstat_secs;

static void vp_stat_init(void)
{
    const char *e;
    s_vstat_on = apu_env_on("RECOMP_APU_TRACE") || apu_env_on("RECOMP_APU_VSTAT");
    e = getenv("RECOMP_APU_VOICE_DUMP");
    if (e && *e) {
        const char *path = getenv("RECOMP_APU_VOICE_DUMP_FILE");
        if (!strcmp(e, "all")) {
            /* RECOMP_APU_VOICE_DUMP=all: <file>.<voice>.raw for each voice
             * that renders anything (voice slots are not stable between
             * runs, so a sound cannot be picked by number in advance). */
            s_vdump_voice = -2;
            snprintf(s_vdump_prefix, sizeof(s_vdump_prefix), "%s",
                     path && *path ? path : "voice_dump");
            fprintf(stderr, "[APU] voice dump: every voice -> %s.<voice>.raw\n",
                    s_vdump_prefix);
            return;
        }
        s_vdump_voice = atoi(e);
        s_vdump = fopen(path && *path ? path : "voice_dump.raw", "wb");
        fprintf(stderr, "[APU] voice dump: voice %d -> %s\n", s_vdump_voice,
                s_vdump ? (path && *path ? path : "voice_dump.raw") : "(open failed)");
    }
}

static void vp_stat_samples(uint32_t v, const float samples[][2], int n,
                            hwaddr first_addr, uint32_t fmt)
{
    struct VpVoiceStat *s = &s_vstat[v];
    if (s_vstat_on < 0) vp_stat_init();
    if (!s_vstat_on && (int)v != s_vdump_voice && s_vdump_voice != -2) return;
    if (n <= 0) return;
    if (s->frames == 0) {
        s->addr = (uint32_t)first_addr;
        s->cls = mcpx_apu_phys_class(first_addr);
        s->fmt = fmt;
    }
    for (int i = 0; i < n; i++) {
        float x = samples[i][0];
        s->sq += (double)x * x;
        if ((x < 0) != (s->last < 0)) s->zc++;
        s->last = x;
    }
    s->frames += (uint32_t)n;
    if (s_vdump_voice == -2 && !s_vdump_all[v] && !s_vdump_failed[v]) {
        char name[600];
        snprintf(name, sizeof(name), "%s.%u.raw", s_vdump_prefix, (unsigned)v);
        s_vdump_all[v] = fopen(name, "wb");
        s_vdump_failed[v] = s_vdump_all[v] == NULL;
    }
    {
        FILE *df = s_vdump_voice == -2 ? s_vdump_all[v]
                 : (int)v == s_vdump_voice ? s_vdump : NULL;
        int16_t out[NUM_SAMPLES_PER_FRAME * 2];
        int m = n < NUM_SAMPLES_PER_FRAME ? n : NUM_SAMPLES_PER_FRAME;
        if (!df)
            return;
        for (int i = 0; i < m; i++) {
            for (int c = 0; c < 2; c++) {
                float x = samples[i][c] * 32767.0f;
                if (x > 32767.0f) x = 32767.0f;
                if (x < -32768.0f) x = -32768.0f;
                out[2 * i + c] = (int16_t)x;
            }
        }
        fwrite(out, sizeof(int16_t) * 2, (size_t)m, df);
    }
}

static void vp_stat_report(void)
{
    if (s_vstat_on <= 0 && s_vdump_voice == -1) return;
    if (++s_vstat_frames % 1500) return;
    /* Flush the dumps once a second, trace or not: a watchdog _exit or a
     * crash skips the stdio buffers. */
    if (s_vdump) fflush(s_vdump);
    for (unsigned v = 0; v < MCPX_HW_MAX_VOICES; v++)
        if (s_vdump_all[v]) fflush(s_vdump_all[v]);
    if (s_vstat_on <= 0) return;
    s_vstat_secs++;
    for (unsigned v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        struct VpVoiceStat *s = &s_vstat[v];
        if (!s->frames) continue;
        fprintf(stderr, "[APU-VSTAT] t=%us v=%u fmt=%08X stream=%u st=%u ss=%u cs=%u spb=%u"
                " rate=%.3f ea=%.2f vol=%03X/%03X bin=%u/%u addr=%08X %s"
                " frames=%u rms=%.0f zcr=%.2f\n",
                s_vstat_secs, v, s->fmt,
                (unsigned)((s->fmt & NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0),
                (unsigned)((s->fmt & NV_PAVS_VOICE_CFG_FMT_STEREO) != 0),
                (unsigned)((s->fmt & NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE) >> ctz32(NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE)),
                (unsigned)((s->fmt & NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE) >> ctz32(NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE)),
                (unsigned)((s->fmt & NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK) >> ctz32(NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK)),
                s->rate, s->ea, s->vol0, s->vol1, s->bin0, s->bin1, s->addr,
                s->cls ? s->cls : "?", s->frames,
                sqrt(s->sq / s->frames) * 32768.0, (double)s->zc / s->frames);
        if (s->addr) {
            /* The bytes the voice is reading now: says at a glance whether
             * the buffer holds audio and whether anything is writing it. */
            const uint8_t *p = mcpx_apu_phys(s->addr);
            fprintf(stderr, "[APU-VSTAT]     v=%u bytes", v);
            for (int i = 0; p && i < 24; i++)
                fprintf(stderr, " %02X", p[i]);
            fputc('\n', stderr);
        }
        s->frames = 0; s->zc = 0; s->sq = 0;
    }
}

/* ============================================================
 * Ring monitor (RECOMP_APU_RING)
 *
 * A title that streams compressed audio decodes it on the CPU into a looping
 * PCM buffer voice and keeps writing ahead of the play cursor. If the writer
 * falls behind, the VP plays the previous lap's data again: no digital zero,
 * nothing a check on the mix can see, and only a stutter to the ear.
 *
 * Each looping PCM buffer is split into 1 KB blocks, and the samples the VP
 * fetches are hashed per block as it plays them. When the cursor leaves a
 * block it played whole, that hash is compared with the one from the
 * block's previous play. Once some block of a buffer has played differently
 * on two laps, the buffer is a ring (one loaded once and looped -- a sound
 * effect -- never changes and is never judged). In a ring, a non-silent
 * block that plays exactly as it did one lap before is a stale-lap replay.
 *
 * "lead" is how far ahead of the cursor the writer is: the blocks after the
 * one being entered whose memory has changed since they were last played,
 * in source frames.
 *
 *   [APU-RING] stale v=<voice> blk=<b>/<n> lap=<laps> ms=<wall ms>
 *       first 200 stale-lap replays
 *   [APU-RING] t=<s> v=<voice> bytes=<ring size> entries=<blocks played>
 *       stale=<this second> stale_total=<run> lead_min=<frames> lead_avg=<frames>
 *       once a VP second for each ring that played
 * ============================================================ */

#define RING_BLK      1024u
#define RING_MAX_BLKS 1024u

struct VpRing {
    uint32_t ba, bytes, nblk;
    uint32_t cur;                 /* block being played, ~0: none */
    uint32_t cur_frames;          /* frames of it played so far */
    uint64_t acc;                 /* hash of those frames */
    bool     acc_sound;
    uint32_t laps;
    bool     dynamic;
    uint32_t entries, stale, stale_total;
    uint32_t lead_min, lead_n;
    uint64_t lead_sum;
    uint64_t played[RING_MAX_BLKS];   /* hash of its last whole play */
    uint64_t mem[RING_MAX_BLKS];      /* memory hash when it was last entered */
    uint8_t  seen[RING_MAX_BLKS];     /* bit 0: played, bit 1: mem valid */
};
static struct VpRing *s_ring[MCPX_HW_MAX_VOICES];
static int s_ring_on = -1;
static unsigned s_ring_logged;
static unsigned s_ring_frames, s_ring_secs;

static uint64_t ring_hash_block(MCPXAPUState *d, uint32_t ba, uint32_t off,
                                uint32_t len)
{
    uint64_t h = 0x9E3779B97F4A7C15ull;
    while (len) {
        uint32_t lin = ba + off;
        uint32_t run = TARGET_PAGE_SIZE - lin % TARGET_PAGE_SIZE;
        if (run > len) run = len;
        const uint8_t *p = mcpx_apu_phys(
            get_data_ptr(d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFF, lin));
        uint32_t i = 0;
        for (; i + 8 <= run; i += 8) {
            uint64_t w;
            memcpy(&w, p + i, 8);
            h = (h ^ w) * 0x100000001B3ull;
            h ^= h >> 29;
        }
        for (; i < run; i++)
            h = (h ^ p[i]) * 0x100000001B3ull;
        off += run;
        len -= run;
    }
    return h;
}

/* The cursor left block r->cur. */
static void ring_close_block(struct VpRing *r, uint32_t v, uint32_t frames_per_blk)
{
    uint32_t b = r->cur;
    if (b == ~0u || r->cur_frames < frames_per_blk)
        return;                              /* not played whole */
    if (r->seen[b] & 1) {
        if (r->acc != r->played[b]) {
            r->dynamic = true;
        } else if (r->dynamic && r->acc_sound) {
            r->stale++;
            r->stale_total++;
            if (s_ring_logged < 200) {
                s_ring_logged++;
                fprintf(stderr, "[APU-RING] stale v=%u blk=%u/%u lap=%u ms=%llu\n",
                        v, b, r->nblk, r->laps,
                        (unsigned long long)(qemu_clock_get_us(QEMU_CLOCK_REALTIME) / 1000));
            }
        }
    }
    r->played[b] = r->acc;
    r->seen[b] |= 1;
}

/* The VP fetched n frames of a looping PCM buffer voice, the first at
 * offset first; frames are frame_bytes apart in the buffer. */
static void vp_ring_check(MCPXAPUState *d, uint32_t v, uint32_t ba,
                          uint32_t ebo, uint32_t frame_bytes, uint32_t first,
                          const float samples[][2], int n)
{
    struct VpRing *r;
    uint32_t bytes = (ebo + 1) * frame_bytes;
    uint32_t fpb;

    if (s_ring_on < 0)
        s_ring_on = apu_env_on("RECOMP_APU_RING");
    if (!s_ring_on || v >= MCPX_HW_MAX_VOICES || bytes < 2 * RING_BLK ||
        frame_bytes == 0 || RING_BLK % frame_bytes)
        return;
    fpb = RING_BLK / frame_bytes;
    r = s_ring[v];
    if (!r) {
        r = s_ring[v] = calloc(1, sizeof(*r));
        if (!r) return;
        r->ba = ~0u;
    }
    if (r->ba != ba || r->bytes != bytes) {   /* a new buffer on this voice */
        uint32_t keep_total = r->stale_total;
        memset(r, 0, sizeof(*r));
        r->ba = ba;
        r->bytes = bytes;
        r->nblk = bytes / RING_BLK;           /* a partial last block is skipped */
        if (r->nblk > RING_MAX_BLKS) r->nblk = RING_MAX_BLKS;
        r->cur = ~0u;
        r->lead_min = ~0u;
        r->stale_total = keep_total;
    }

    for (int i = 0; i < n; i++) {
        uint32_t b = (first + (uint32_t)i) / fpb;
        if (b != r->cur) {
            ring_close_block(r, v, fpb);
            if (b < r->cur && r->cur != ~0u)
                r->laps++;
            r->cur = b;
            r->cur_frames = 0;
            r->acc = 0x9E3779B97F4A7C15ull;
            r->acc_sound = false;
            if (b < r->nblk) {
                /* Entering b: the writer's lead, then b's memory hash. */
                uint64_t h = ring_hash_block(d, ba, b * RING_BLK, RING_BLK);
                r->entries++;
                if (r->dynamic) {
                    uint32_t k, lead = 0;
                    for (k = 1; k < r->nblk; k++) {
                        uint32_t bb = (b + k) % r->nblk;
                        if (!(r->seen[bb] & 2))
                            break;
                        if (ring_hash_block(d, ba, bb * RING_BLK, RING_BLK) == r->mem[bb])
                            break;
                        lead++;
                    }
                    lead *= fpb;
                    if (lead < r->lead_min) r->lead_min = lead;
                    r->lead_sum += lead;
                    r->lead_n++;
                }
                r->mem[b] = h;
                r->seen[b] |= 2;
            }
        }
        if (b >= r->nblk)
            continue;
        {
            uint32_t w[2];
            memcpy(w, samples[i], sizeof(w));
            r->acc = (r->acc ^ (((uint64_t)w[0] << 32) | w[1])) * 0x100000001B3ull;
            r->acc ^= r->acc >> 29;
            if (w[0] | w[1]) r->acc_sound = true;
            r->cur_frames++;
        }
    }
}

static void vp_ring_report(unsigned secs)
{
    if (s_ring_on <= 0) return;
    for (unsigned v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        struct VpRing *r = s_ring[v];
        if (!r || !r->entries || !r->dynamic) {
            if (r) r->entries = 0;
            continue;
        }
        fprintf(stderr, "[APU-RING] t=%us v=%u bytes=%u entries=%u stale=%u"
                " stale_total=%u lead_min=%u lead_avg=%llu\n",
                secs, v, r->bytes, r->entries, r->stale, r->stale_total,
                r->lead_n ? r->lead_min : 0,
                (unsigned long long)(r->lead_n ? r->lead_sum / r->lead_n : 0));
        r->entries = 0;
        r->stale = 0;
        r->lead_min = ~0u;
        r->lead_sum = 0;
        r->lead_n = 0;
    }
}

/* ============================================================
 * Sample fetching from voice buffers
 * ============================================================ */

/* ============================================================
 * Sound dump (RECOMP_SND_DUMP=<dir>, "1" = sound_dump)
 *
 * Every sound the game plays from a hardware buffer (a one-shot or a looping
 * ADPCM / PCM effect: gunfire, impacts, voices that are not streamed) is
 * saved once as <dir>\<hash16>_<ms>ms_<hz>Hz[_stereo][_loop].wav, with the
 * ORIGINAL samples (before pitch, volume and mixing). The hash is a 64-bit
 * FNV-1a of the buffer's raw bytes plus its format, so the same sound has the
 * same name in every run and a later replacement can look a sound up by it.
 * A file that already exists is not rewritten, only its time is touched (so
 * Explorer's "Date modified" sort puts the sounds of this run on top).
 *
 * Not captured: stream voices (SSL segments) and looping PCM voices -- those
 * are the software mix ring (music, dialogue, ambience), which is refilled
 * all the time and has no fixed buffer to name.
 *
 * The VP thread only copies the bytes and hashes them; decoding and file
 * writing happen on a worker thread so the audio never waits for the disk.
 * ============================================================ */

#ifdef _WIN32
struct SndJob {
    uint8_t *raw;
    uint32_t nbytes, frames, block_size, container_size, sample_size, channels;
    uint32_t hz;
    int adpcm, loop;
    uint64_t hash;
};

#define SND_Q        64
#define SND_SEEN     65536                  /* distinct sounds per run (open addressing) */
#define SND_MAX_BYTES (16u << 20)

static int s_snd_on = -1;
static char s_snd_dir[260];
static float s_snd_rate = 1.0f;             /* voice_resample's ratio for the voice being read */
static struct SndJob s_sq[SND_Q];
static unsigned s_sq_head, s_sq_tail;
static CRITICAL_SECTION s_sq_cs;
static HANDLE s_sq_ev;
static uint64_t *s_snd_seen;
static unsigned s_snd_nseen;
static unsigned s_snd_saved, s_snd_dropped;
static struct { uint32_t ba, ebo, key; } s_snd_last[MCPX_HW_MAX_VOICES];

static uint64_t snd_fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n--) { h ^= *b++; h *= 0x100000001B3ull; }
    return h;
}

/* ============================================================
 * Sound mods (mods\sounds\<hash16>*.wav, RECOMP_MODS / RECOMP_MODS_DIR)
 *
 * The same hash the dump above writes, so a dumped file renamed into
 * mods\sounds (or edited in place and copied there) replaces that sound.
 * The WAV's own samples are mapped onto the voice's own length (cbo's range
 * is still what the game set: pitch, loop point and envelope stay the
 * game's), so a replacement shorter or longer than the original still plays
 * for the original duration -- stretched or compressed, not cut or padded.
 * Packs: mods\sounds first, then every mods\<pack>\sounds by name, a later
 * one overriding an earlier one on the same hash -- identical rule to the
 * texture mods in icon_swap.c (texmod_*).
 * ============================================================ */

typedef struct { uint64_t hash; int16_t *pcm; uint32_t frames; unsigned channels; } ModSnd;
static ModSnd *s_ms;
static int s_ms_n, s_ms_cap, s_ms_init;
static ModSnd *s_snd_active[MCPX_HW_MAX_VOICES];

static uint32_t snd_rl32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t snd_rl16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* A plain RIFF/WAVE PCM file (8/16/24/32-bit int; chunks in any order, any
 * size -- what the dump above writes and what any editor saves) to int16
 * interleaved. Float WAVs and anything else are refused with a log line. */
static int sndmod_wav_load(const char *path, ModSnd *out)
{
    FILE *fp = fopen(path, "rb");
    uint8_t *buf;
    long sz;
    size_t rd, off;
    uint16_t fmt_tag = 0, channels = 0, bits = 0;
    const uint8_t *data = NULL;
    uint32_t data_len = 0, i, frames;
    int16_t *pcm;

    if (!fp)
        return 0;
    fseek(fp, 0, SEEK_END); sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (sz < 44 || sz > (16 << 20)) { fclose(fp); return 0; }
    buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(fp); return 0; }
    rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (rd < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) {
        free(buf);
        return 0;
    }

    for (off = 12; off + 8 <= rd; ) {
        uint32_t len = snd_rl32(buf + off + 4);
        const uint8_t *id = buf + off;
        off += 8;
        if (off + len > rd)
            break;
        if (!memcmp(id, "fmt ", 4) && len >= 16) {
            fmt_tag = snd_rl16(buf + off);
            channels = snd_rl16(buf + off + 2);
            bits = snd_rl16(buf + off + 14);
        } else if (!memcmp(id, "data", 4)) {
            data = buf + off;
            data_len = len;
        }
        off += ((size_t)len + 1) & ~(size_t)1;        /* chunks are word-aligned */
    }
    if (!data || !channels || !bits || (bits != 8 && bits != 16 && bits != 24 && bits != 32)
        || (fmt_tag != 1 && fmt_tag != 0xFFFE)) {
        fprintf(stderr, "[SNDMOD] %s: not a plain integer-PCM WAV, skipped\n", path);
        free(buf);
        return 0;
    }
    frames = data_len / (channels * (uint32_t)(bits / 8));
    pcm = (int16_t *)malloc((size_t)frames * channels * sizeof(int16_t));
    if (!pcm) { free(buf); return 0; }
    for (i = 0; i < frames * channels; i++) {
        const uint8_t *p = data + (size_t)i * (bits / 8);
        int16_t s = 0;
        switch (bits) {
        case 8:  s = (int16_t)(((int)p[0] - 128) << 8); break;
        case 16: s = (int16_t)(p[0] | (p[1] << 8)); break;
        case 24: {
            int32_t v = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
            if (v & 0x800000) v |= ~0xFFFFFF;          /* sign-extend */
            s = (int16_t)(v >> 8);
            break;
        }
        case 32: s = (int16_t)((int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)) >> 16); break;
        default: break;
        }
        pcm[i] = s;
    }
    free(buf);
    out->pcm = pcm;
    out->frames = frames;
    out->channels = channels;
    if (!frames) {
        free(pcm);
        return 0;
    }
    return 1;
}

static void sndmod_scan_dir(const char *dir)
{
    WIN32_FIND_DATAA fd;
    char pat[MAX_PATH + 8];
    HANDLE h;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        const char *n = fd.cFileName, *dot = strrchr(n, '.');
        uint64_t hv = 0;
        int i, ok = 1, found = -1;
        char path[MAX_PATH + 16];
        ModSnd loaded;

        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !dot || _stricmp(dot, ".wav"))
            continue;
        for (i = 0; i < 16 && ok; i++) {
            char c = n[i];
            if (c >= '0' && c <= '9') hv = hv * 16 + (uint64_t)(c - '0');
            else if (c >= 'a' && c <= 'f') hv = hv * 16 + (uint64_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') hv = hv * 16 + (uint64_t)(c - 'A' + 10);
            else ok = 0;
        }
        if (!ok)
            continue;
        snprintf(path, sizeof path, "%s\\%s", dir, n);
        if (!sndmod_wav_load(path, &loaded))
            continue;
        for (i = 0; i < s_ms_n; i++)
            if (s_ms[i].hash == hv) { found = i; break; }
        if (found < 0) {
            if (s_ms_n == s_ms_cap) {
                ModSnd *p = (ModSnd *)realloc(s_ms, (size_t)(s_ms_cap ? s_ms_cap * 2 : 64) * sizeof *p);
                if (!p) { free(loaded.pcm); continue; }
                s_ms = p;
                s_ms_cap = s_ms_cap ? s_ms_cap * 2 : 64;
            }
            found = s_ms_n++;
        } else {
            free(s_ms[found].pcm);                      /* a later pack replaces an earlier one */
        }
        s_ms[found] = loaded;
        s_ms[found].hash = hv;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void sndmod_init(void)
{
    const char *e = getenv("RECOMP_MODS"), *d = getenv("RECOMP_MODS_DIR");
    char root[MAX_PATH], sub[MAX_PATH + 16], pat[MAX_PATH + 8];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    s_ms_init = 1;
    if (e && e[0] == '0')
        return;
    snprintf(root, sizeof root, "%s", d && d[0] ? d : "mods");
    snprintf(sub, sizeof sub, "%s\\sounds", root);
    sndmod_scan_dir(sub);                                /* mods\sounds */
    snprintf(pat, sizeof pat, "%s\\*", root);            /* then every pack: mods\<pack>\sounds, by name */
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') {
                snprintf(sub, sizeof sub, "%s\\%s\\sounds", root, fd.cFileName);
                sndmod_scan_dir(sub);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (s_ms_n) {
        fprintf(stderr, "  [SNDMOD] %d replacement sound(s) found under %s\n", s_ms_n, root);
        fflush(stderr);
    }
}

static ModSnd *sndmod_find(uint64_t hash)
{
    int i;
    for (i = 0; i < s_ms_n; i++)
        if (s_ms[i].hash == hash)
            return &s_ms[i];
    return NULL;
}

static void snd_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Worker: decode one job to 16-bit PCM and write it, or touch the file a
 * previous run wrote under the same hash. */
static void snd_write_job(struct SndJob *j)
{
    char pat[400], name[420];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    SYSTEMTIME st;
    FILETIME ft;
    int16_t *pcm;
    uint32_t i, ch = j->channels, total, nblocks;
    unsigned ms;

    snprintf(pat, sizeof pat, "%s\\%016llX_*.wav", s_snd_dir, (unsigned long long)j->hash);
    h = FindFirstFileA(pat, &fd);
    GetSystemTime(&st);
    SystemTimeToFileTime(&st, &ft);
    if (h != INVALID_HANDLE_VALUE) {          /* heard in an earlier run: only its time moves */
        HANDLE f;
        FindClose(h);
        snprintf(name, sizeof name, "%s\\%s", s_snd_dir, fd.cFileName);
        f = CreateFileA(name, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            SetFileTime(f, NULL, NULL, &ft);
            CloseHandle(f);
        }
        return;
    }

    total = j->frames * ch;
    pcm = (int16_t *)calloc((size_t)total + 2u * 65u * 32u * 2u, sizeof(int16_t));
    if (!pcm)
        return;
    if (j->adpcm) {
        nblocks = (j->frames + ADPCM_SAMPLES_PER_BLOCK - 1) / ADPCM_SAMPLES_PER_BLOCK;
        for (i = 0; i < nblocks; i++) {
            int16_t dec[65 * ADPCM_MAX_BLOCKS * 2];
            uint32_t n = ADPCM_SAMPLES_PER_BLOCK, k;
            if ((size_t)(i + 1) * j->block_size > j->nbytes)
                break;
            adpcm_decode_block(dec, j->raw + (size_t)i * j->block_size, j->block_size, (int)ch);
            if ((i + 1) * ADPCM_SAMPLES_PER_BLOCK > j->frames)
                n = j->frames - i * ADPCM_SAMPLES_PER_BLOCK;
            for (k = 0; k < n * ch; k++)
                pcm[(size_t)i * ADPCM_SAMPLES_PER_BLOCK * ch + k] = dec[k];
        }
    } else {
        for (i = 0; i < j->frames; i++) {
            uint32_t c;
            for (c = 0; c < ch; c++) {
                size_t o = (size_t)i * j->block_size + (size_t)c * j->container_size;
                int16_t s = 0;
                if (o + j->container_size <= j->nbytes) {
                    const uint8_t *p = j->raw + o;
                    switch (j->sample_size) {
                    case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_U8:  s = (int16_t)(((int)p[0] - 128) << 8); break;
                    case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16: s = (int16_t)(p[0] | (p[1] << 8)); break;
                    case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24:
                        s = (int16_t)(((int32_t)((uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)) << 8)) >> 16);
                        break;
                    case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S32:
                        s = (int16_t)((int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)) >> 16);
                        break;
                    default: break;
                    }
                }
                pcm[(size_t)i * ch + c] = s;
            }
        }
    }

    ms = (unsigned)((uint64_t)j->frames * 1000u / (j->hz ? j->hz : 48000u));
    snprintf(name, sizeof name, "%s\\%016llX_%ums_%uHz%s%s.wav", s_snd_dir,
             (unsigned long long)j->hash, ms, j->hz, ch == 2 ? "_stereo" : "", j->loop ? "_loop" : "");
    {
        FILE *fp = fopen(name, "wb");
        if (fp) {
            uint8_t hd[44];
            uint32_t bytes = total * 2;
            memcpy(hd, "RIFF", 4);  snd_le32(hd + 4, 36 + bytes);
            memcpy(hd + 8, "WAVEfmt ", 8);  snd_le32(hd + 16, 16);
            hd[20] = 1; hd[21] = 0; hd[22] = (uint8_t)ch; hd[23] = 0;
            snd_le32(hd + 24, j->hz);  snd_le32(hd + 28, j->hz * ch * 2);
            hd[32] = (uint8_t)(ch * 2); hd[33] = 0; hd[34] = 16; hd[35] = 0;
            memcpy(hd + 36, "data", 4);  snd_le32(hd + 40, bytes);
            fwrite(hd, 1, 44, fp);
            fwrite(pcm, 2, total, fp);
            fclose(fp);
            s_snd_saved++;
            if (s_snd_saved <= 5 || s_snd_saved % 50 == 0)
                fprintf(stderr, "[SNDDUMP] %u sounds saved; latest %s\n", s_snd_saved, name);
        }
    }
    free(pcm);
}

static DWORD WINAPI snd_worker(LPVOID arg)
{
    (void)arg;
    for (;;) {
        struct SndJob j;
        int have = 0;
        EnterCriticalSection(&s_sq_cs);
        if (s_sq_tail != s_sq_head) {
            j = s_sq[s_sq_tail % SND_Q];
            s_sq_tail++;
            have = 1;
        }
        LeaveCriticalSection(&s_sq_cs);
        if (!have) {
            WaitForSingleObject(s_sq_ev, INFINITE);
            continue;
        }
        snd_write_job(&j);
        free(j.raw);
    }
    return 0;
}

static int snd_init(void)
{
    const char *e = getenv("RECOMP_SND_DUMP");
    if (!e || !*e || *e == '0')
        return 0;
    snprintf(s_snd_dir, sizeof s_snd_dir, "%s", (e[0] == '1' && !e[1]) ? "sound_dump" : e);
    CreateDirectoryA(s_snd_dir, NULL);
    s_snd_seen = (uint64_t *)calloc(SND_SEEN, sizeof(uint64_t));
    if (!s_snd_seen)
        return 0;
    InitializeCriticalSection(&s_sq_cs);
    s_sq_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!s_sq_ev || !CreateThread(NULL, 0, snd_worker, NULL, 0, NULL))
        return 0;
    fprintf(stderr, "[SNDDUMP] saving every effect sound the game plays to %s\\\n", s_snd_dir);
    return 1;
}

/* The hardware voice rate for the voice the next voice_get_samples reads:
 * 48000 / ratio is the source rate, snapped to the usual ones. */
static uint32_t snd_source_hz(void)
{
    static const uint32_t usual[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 };
    float hz = s_snd_rate > 0.0f ? 48000.0f / s_snd_rate : 48000.0f;
    size_t i;
    for (i = 0; i < sizeof usual / sizeof usual[0]; i++)
        if (hz > usual[i] * 0.97f && hz < usual[i] * 1.03f)
            return usual[i];
    return (uint32_t)(hz + 0.5f);
}

/* A hardware (non-stream) voice read from its first sample: dumps it
 * (RECOMP_SND_DUMP) and/or arms its mods\sounds replacement in
 * s_snd_active[v] for the rest of this sound (cleared the moment the voice
 * starts a different one, including an unmatched one). */
static void snd_dump_voice(MCPXAPUState *d, uint32_t v, uint32_t ba, uint32_t ebo,
                           bool adpcm, bool loop, unsigned channels,
                           unsigned sample_size, unsigned container_size, size_t block_size)
{
    uint32_t frames = ebo + 1, nbytes, off, key;
    uint64_t h;
    uint8_t *raw;
    unsigned idx, tries;
    int want_dump, want_mods;

    if (v >= MCPX_HW_MAX_VOICES)
        return;
    if (s_snd_on < 0)
        s_snd_on = snd_init();
    if (!s_ms_init)
        sndmod_init();
    want_dump = s_snd_on;
    want_mods = s_ms_n > 0;
    if (!want_dump && !want_mods)
        return;
    if (loop && !adpcm) {                        /* the software mix ring and the 32-sample FX loops */
        s_snd_active[v] = NULL;
        return;
    }
    if (frames < 64 || frames > 48000u * 120u || block_size == 0) {
        s_snd_active[v] = NULL;
        return;
    }

    key = (adpcm ? 1u : 0u) | (loop ? 2u : 0u) | (channels << 2) | (sample_size << 4)
        | (container_size << 8) | ((uint32_t)block_size << 12);
    if (loop && s_snd_last[v].ba == ba && s_snd_last[v].ebo == ebo && s_snd_last[v].key == key)
        return;                                  /* a loop coming round again: s_snd_active[v] already set */
    s_snd_last[v].ba = ba; s_snd_last[v].ebo = ebo; s_snd_last[v].key = key;

    if (adpcm)
        nbytes = ((ebo / ADPCM_SAMPLES_PER_BLOCK) + 1) * (uint32_t)block_size;
    else
        nbytes = ebo * (uint32_t)block_size + channels * container_size;
    nbytes = (nbytes + 3) & ~3u;
    if (nbytes == 0 || nbytes > SND_MAX_BYTES) {
        s_snd_active[v] = NULL;
        return;
    }
    raw = (uint8_t *)malloc((size_t)nbytes + 8);
    if (!raw) {
        s_snd_active[v] = NULL;
        return;
    }
    for (off = 0; off < nbytes; off += 4) {
        uint32_t w = ldl_le_phys(address_space_memory,
                                 get_data_ptr(d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFF, ba + off));
        memcpy(raw + off, &w, 4);
    }
    memset(raw + nbytes, 0, 8);

    h = snd_fnv(0xCBF29CE484222325ull, raw, nbytes);
    h = snd_fnv(h, &key, sizeof key);
    h = snd_fnv(h, &frames, sizeof frames);
    if (h == 0)
        h = 1;

    s_snd_active[v] = want_mods ? sndmod_find(h) : NULL;

    if (!want_dump) {
        free(raw);
        return;
    }
    idx = (unsigned)(h % SND_SEEN);
    for (tries = 0; tries < SND_SEEN; tries++, idx = (idx + 1) % SND_SEEN) {
        if (s_snd_seen[idx] == h) {              /* already handled in this run */
            free(raw);
            return;
        }
        if (s_snd_seen[idx] == 0)
            break;
    }
    if (tries == SND_SEEN || s_snd_nseen > SND_SEEN * 3 / 4) {
        free(raw);
        return;
    }

    EnterCriticalSection(&s_sq_cs);
    if (s_sq_head - s_sq_tail < SND_Q) {
        struct SndJob *j = &s_sq[s_sq_head % SND_Q];
        j->raw = raw; j->nbytes = nbytes; j->frames = frames; j->block_size = (uint32_t)block_size;
        j->container_size = container_size; j->sample_size = sample_size; j->channels = channels;
        j->hz = snd_source_hz(); j->adpcm = adpcm; j->loop = loop; j->hash = h;
        s_sq_head++;
        raw = NULL;
        s_snd_seen[idx] = h;
        s_snd_nseen++;
    }
    LeaveCriticalSection(&s_sq_cs);
    if (raw) {                                   /* queue full: try again the next time it plays */
        free(raw);
        s_snd_dropped++;
        return;
    }
    SetEvent(s_sq_ev);
}
#else
static float s_snd_rate = 1.0f;
static void snd_dump_voice(MCPXAPUState *d, uint32_t v, uint32_t ba, uint32_t ebo,
                           bool adpcm, bool loop, unsigned channels,
                           unsigned sample_size, unsigned container_size, size_t block_size)
{
    (void)d; (void)v; (void)ba; (void)ebo; (void)adpcm; (void)loop; (void)channels;
    (void)sample_size; (void)container_size; (void)block_size;
}
#endif

/* ============================================================
 * Music tap (RECOMP_MUSIC_TAP=<prefix>)
 *
 * The samples a music voice (a looping PCM ring the title refills, or any
 * stream voice) is read as, before pitch, filter, volume and mixing, written
 * as one stereo 48 kHz WAV per voice: <prefix>_v<voice>_<n>.wav, a new file
 * when the voice's buffer changes. Set against the final mix (RECOMP_AUDIO_WAV)
 * it says which stage a defect in the music enters at. RECOMP_MUSIC_TAP_SECS
 * caps each file (default 240).
 * ============================================================ */

static struct MusicTap {
    FILE *fp;
    uint64_t frames, cap;
    uint32_t ba, ebo, nfile;
} s_tap[MCPX_HW_MAX_VOICES];
static int s_tap_on = -1;
static char s_tap_prefix[256];
static uint64_t s_tap_cap;

static void tap_header(struct MusicTap *t)
{
    uint8_t h[44];
    uint32_t data = (uint32_t)(t->frames * 4), rate = 48000;
    memcpy(h, "RIFF", 4);  h[4] = (uint8_t)(36 + data); h[5] = (uint8_t)((36 + data) >> 8);
    h[6] = (uint8_t)((36 + data) >> 16); h[7] = (uint8_t)((36 + data) >> 24);
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[17] = h[18] = h[19] = 0; h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0;
    h[24] = (uint8_t)rate; h[25] = (uint8_t)(rate >> 8); h[26] = (uint8_t)(rate >> 16); h[27] = 0;
    h[28] = (uint8_t)(rate * 4); h[29] = (uint8_t)((rate * 4) >> 8);
    h[30] = (uint8_t)((rate * 4) >> 16); h[31] = 0;
    h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4);
    h[40] = (uint8_t)data; h[41] = (uint8_t)(data >> 8); h[42] = (uint8_t)(data >> 16);
    h[43] = (uint8_t)(data >> 24);
    fseek(t->fp, 0, SEEK_SET);
    fwrite(h, 1, 44, t->fp);
    fseek(t->fp, 0, SEEK_END);
    fflush(t->fp);
}

static void music_tap(uint32_t v, const float samples[][2], int n, bool stream,
                      bool adpcm, unsigned channels, uint32_t ba, uint32_t ebo,
                      bool loop)
{
    struct MusicTap *t;
    int16_t out[NUM_SAMPLES_PER_FRAME * 4 + 2][2];

    if (s_tap_on < 0) {
        const char *e = getenv("RECOMP_MUSIC_TAP"), *s = getenv("RECOMP_MUSIC_TAP_SECS");
        s_tap_on = e && *e;
        if (s_tap_on) {
            snprintf(s_tap_prefix, sizeof s_tap_prefix, "%s", e);
            s_tap_cap = (uint64_t)((s ? atof(s) : 240.0) * 48000.0);
        }
    }
    if (!s_tap_on || v >= MCPX_HW_MAX_VOICES || n > (int)(sizeof out / sizeof out[0]))
        return;
    t = &s_tap[v];
    if (t->fp && (t->ba != ba || t->ebo != ebo)) {     /* a new buffer on the voice */
        tap_header(t);
        fclose(t->fp);
        t->fp = NULL;
    }
    if (!t->fp) {
        char name[300];
        if (t->nfile >= 20)
            return;
        snprintf(name, sizeof name, "%s_v%u_%u.wav", s_tap_prefix, v, t->nfile++);
        t->fp = fopen(name, "wb+");
        if (!t->fp)
            return;
        t->frames = 0; t->ba = ba; t->ebo = ebo;
        tap_header(t);
        fprintf(stderr, "[MUSICTAP] %s: v=%u stream=%d adpcm=%d ch=%u base %08X ebo %u loop=%d rate=%.4f\n",
                name, v, (int)stream, (int)adpcm, channels, ba, ebo, (int)loop,
                (double)g_dbg.vp.v[v].rate);
    }
    if (t->frames >= s_tap_cap)
        return;
    for (int i = 0; i < n; i++)
        for (int c = 0; c < 2; c++) {
            float f = samples[i][c] * 32768.0f;
            out[i][c] = (int16_t)(f > 32767.0f ? 32767 : f < -32768.0f ? -32768 : (int)f);
        }
    fwrite(out, 4, (size_t)n, t->fp);
    t->frames += (uint64_t)n;
    if ((t->frames & 0xFFFF) < (uint64_t)n)           /* about every 1.4 s */
        tap_header(t);
}

/* RECOMP_APU_LAG (voice_get_samples): per voice, the VP frame it was last
 * read in, where it stopped, and whether it has gone round its loop since
 * it started. Only the VP thread touches them. */
static uint32_t s_lag_frame;
static uint32_t s_lag_seen[MCPX_HW_MAX_VOICES], s_lag_last_cbo[MCPX_HW_MAX_VOICES];
static uint8_t s_lag_lapped[MCPX_HW_MAX_VOICES];

static int voice_get_samples(MCPXAPUState *d, uint32_t v, float samples[][2],
                       int num_samples_requested)
{
    assert(v < MCPX_HW_MAX_VOICES);
    bool stereo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_STEREO) != 0;
    unsigned int channels = stereo ? 2 : 1;
    unsigned int sample_size = voice_get_mask(
        d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE);
    unsigned int container_sizes[4] = { 1, 2, 0, 4 };
    unsigned int container_size_index = voice_get_mask(
        d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE);
    unsigned int container_size = container_sizes[container_size_index];
    bool stream = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0;
    bool paused = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                                 NV_PAVS_VOICE_PAR_STATE_PAUSED) != 0;
    bool loop = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                               NV_PAVS_VOICE_CFG_FMT_LOOP) != 0;
    uint32_t ebo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_NEXT,
                                  NV_PAVS_VOICE_PAR_NEXT_EBO);
    uint32_t cbo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                                  NV_PAVS_VOICE_PAR_OFFSET_CBO);
    uint32_t lbo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                                  NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO);
    uint32_t ba = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CUR_PSL_START,
                                 NV_PAVS_VOICE_CUR_PSL_START_BA);
    unsigned int samples_per_block =
        1 + voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                           NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK);
    bool persist = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                  NV_PAVS_VOICE_CFG_FMT_PERSIST) != 0;

    int ssl_index = 0, ssl_seg = 0, page = 0, count = 0;
    int seg_len = 0, seg_cs = 0, seg_spb = 0, seg_s = 0;
    hwaddr segment_offset = 0;
    uint32_t segment_length = 0;
    size_t block_size;

    int adpcm_block_index = -1;
    /* Room for the largest block the MCPX accepts (see the check below). */
    uint32_t adpcm_block[36 * ADPCM_MAX_BLOCKS / 4];
    int16_t adpcm_decoded[65 * ADPCM_MAX_BLOCKS * 2];

    voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                   NV_PAVS_VOICE_PAR_STATE_NEW_VOICE, 0);

    if (paused) return -1;

    if (stream) {
        if (!persist) {
            int eacur = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                                       NV_PAVS_VOICE_PAR_STATE_EACUR);
            if (eacur < NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE) {
                voice_off(d, (uint16_t)v);
                return -1;
            }
        }

        assert(!loop);
        ssl_index = d->vp.ssl[v].ssl_index;
        ssl_seg = d->vp.ssl[v].ssl_seg;
        page = d->vp.ssl[v].base[ssl_index] + ssl_seg;
        count = d->vp.ssl[v].count[ssl_index];

        if (count == 0) {
            voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                           NV_PAVS_VOICE_PAR_OFFSET_CBO, 0);
            d->vp.ssl[v].ssl_seg = 0;
            if (!persist) {
                d->vp.ssl[v].ssl_index = 0;
                voice_off(d, (uint16_t)v);
            } else {
                set_notify_status(d, v, MCPX_HW_NOTIFIER_SSLA_DONE +
                                  d->vp.ssl[v].ssl_index,
                                  NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
            }
            return -1;
        }

        hwaddr addr = d->regs[NV_PAPU_VPSSLADDR] + page * 8;
        segment_offset = ldl_le_phys(address_space_memory, addr);
        segment_length = ldl_le_phys(address_space_memory, addr + 4);
        assert(segment_offset != 0);
        assert(segment_length != 0);
        seg_len = (segment_length >> 0) & 0xffff;
        seg_cs = (segment_length >> 16) & 3;
        seg_spb = (segment_length >> 18) & 0x1f;
        seg_s = (segment_length >> 23) & 1;
        container_size_index = seg_cs;
        if (seg_cs == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM) {
            sample_size = NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24;
        }
        assert(seg_len > 0);
        ebo = seg_len - 1;
    }

    bool adpcm =
        (container_size_index == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM);

    if (adpcm) {
        /* The block is 36 bytes per channel times the descriptor's 5-bit
         * samples-per-block field, read into the fixed buffers above. A
         * value past what they hold overran the stack. */
        if (samples_per_block > ADPCM_MAX_BLOCKS) {
            static int warned;
            if (!warned++)
                fprintf(stderr, "[APU] voice %u: ADPCM samples per block %u "
                        "is above %d; voice skipped\n",
                        v, samples_per_block, ADPCM_MAX_BLOCKS);
            return -1;
        }
        block_size = 36;
    } else {
        block_size = container_size;
    }
    block_size *= samples_per_block;

    /* RECOMP_APU_LAG=n with RECOMP_APU_LAG_BA=lo-hi (hex): a looping PCM
     * voice whose buffer starts in [lo, hi) is read n samples behind the
     * position it reports. A title that refills a short loop just ahead of
     * the position it reads, and is late now and then, had the APU play what
     * the loop held a lap earlier -- MoH: European Assault's music and speech
     * stream (a 2400-sample loop), heard as a whistle and a crackle over
     * every song. The reads trail by n; until the voice has gone round once
     * the first n samples are silence, so a sound starts n samples late
     * instead of out of order. A voice not seen for a few frames is new. */
    int lag_this = 0, lag_first = 0, lag_quiet = 0;
    uint32_t lag_len = 0, lag_l = 0;
    {
        /* Which voices: a buffer base in RECOMP_APU_LAG_BA=lo-hi, or a loop
         * of exactly RECOMP_APU_LAG_LOOP samples -- the stream's own shape,
         * which holds wherever DirectSound puts its buffer (the base moved
         * between the boot menu and later ones). A static loop caught by it
         * only plays shifted in phase, which sounds the same. */
        static int lag_on = -1;
        static uint32_t lag_n, lag_lo, lag_hi, lag_loop;
        if (lag_on < 0) {
            const char *e = getenv("RECOMP_APU_LAG");
            const char *b = getenv("RECOMP_APU_LAG_BA");
            const char *L = getenv("RECOMP_APU_LAG_LOOP");
            unsigned lo = 0, hi = 0;
            lag_n = e ? (uint32_t)atoi(e) : 0;
            if (!(b && sscanf(b, "%x-%x", &lo, &hi) == 2 && lo < hi))
                lo = hi = 0;
            lag_lo = lo; lag_hi = hi;
            lag_loop = L ? (uint32_t)atoi(L) : 0;
            lag_on = lag_n && (lag_hi || lag_loop);
            if (lag_on)
                fprintf(stderr, "[APU] looping PCM voices (buffer %08X-%08X, or a"
                        " %u-sample loop) read %u samples behind\n",
                        lag_lo, lag_hi, lag_loop, lag_n);
        }
        if (lag_on && !stream && !adpcm && loop && lbo == 0
            && ((ba >= lag_lo && ba < lag_hi) || (lag_loop && ebo + 1 == lag_loop))
            && v < MCPX_HW_MAX_VOICES) {
            uint32_t now = s_lag_frame;
            lag_this = 1;
            lag_len = ebo + 1;
            lag_l = lag_n % lag_len;
            if (!s_lag_seen[v] || now - s_lag_seen[v] > 4)
                s_lag_lapped[v] = 0;               /* a new sound */
            else if (cbo < s_lag_last_cbo[v])
                s_lag_lapped[v] = 1;               /* went round */
            s_lag_seen[v] = now ? now : 1;
            lag_first = !s_lag_lapped[v];
        }
    }

    if (!stream && cbo == 0)
        snd_dump_voice(d, v, ba, ebo, adpcm, loop, channels, sample_size,
                       container_size, block_size);

    int sample_count = 0;
    hwaddr stat_addr = 0;
    for (; (sample_count < num_samples_requested) && (cbo <= ebo);
         sample_count++, cbo++) {
        if (sample_count == 0 && s_vstat_on != 0) {
            uint32_t lin = adpcm ? (cbo / ADPCM_SAMPLES_PER_BLOCK) * (uint32_t)block_size
                                 : cbo * (uint32_t)block_size;
            stat_addr = stream ? segment_offset + lin
                               : get_data_ptr(d->regs[NV_PAPU_VPSGEADDR],
                                              0xFFFFFFFF, ba + lin);
        }
#ifdef _WIN32
        if (!stream && s_snd_active[v]) {
            /* mods\sounds replacement: the file's own samples, mapped from
             * this frame onto the voice's own length so pitch, loop point
             * and envelope stay exactly what the game set. */
            const ModSnd *m = s_snd_active[v];
            uint32_t total = ebo + 1;
            uint32_t midx = (total > 1 && m->frames > 1)
                ? (uint32_t)(((uint64_t)cbo * (m->frames - 1)) / (total - 1))
                : 0;
            float l;
            if (midx >= m->frames)
                midx = m->frames ? m->frames - 1 : 0;
            l = m->frames ? int16_to_float(m->pcm[(size_t)midx * m->channels]) : 0.0f;
            samples[sample_count][0] = l;
            if (stereo)
                samples[sample_count][1] = m->channels >= 2
                    ? int16_to_float(m->pcm[(size_t)midx * m->channels + 1]) : l;
        } else
#endif
        if (adpcm) {
            unsigned int block_index = cbo / ADPCM_SAMPLES_PER_BLOCK;
            unsigned int block_position = cbo % ADPCM_SAMPLES_PER_BLOCK;
            if (adpcm_block_index != (int)block_index) {
                uint32_t linear_addr = block_index * (uint32_t)block_size;
                if (stream) {
                    hwaddr addr = segment_offset + linear_addr;
                    memcpy(adpcm_block, mcpx_apu_phys(addr),
                           block_size);
                } else {
                    linear_addr += ba;
                    for (unsigned int word_index = 0;
                         word_index < (9 * samples_per_block); word_index++) {
                        hwaddr addr = get_data_ptr(d->regs[NV_PAPU_VPSGEADDR],
                                                   0xFFFFFFFF, linear_addr);
                        adpcm_block[word_index] =
                            ldl_le_phys(address_space_memory, addr);
                        linear_addr += 4;
                    }
                }
                adpcm_decode_block(adpcm_decoded, (uint8_t *)adpcm_block,
                                   block_size, channels);
                adpcm_block_index = block_index;
            }

            samples[sample_count][0] =
                int16_to_float(adpcm_decoded[block_position * channels]);
            if (stereo) {
                samples[sample_count][1] = int16_to_float(
                    adpcm_decoded[block_position * channels + 1]);
            }
        } else {
            hwaddr addr;
            if (stream) {
                addr = segment_offset + cbo * block_size;
            } else {
                uint32_t rc = cbo;
                if (lag_this) {                /* RECOMP_APU_LAG, below */
                    if (lag_first && cbo < lag_l)
                        lag_quiet = 1;
                    rc = cbo >= lag_l ? cbo - lag_l : cbo + lag_len - lag_l;
                }
                uint32_t linear_addr = ba + rc * (uint32_t)block_size;
                addr = get_data_ptr(d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFF,
                                    linear_addr);
            }

            for (unsigned int channel = 0; channel < channels; channel++) {
                uint32_t ival;
                float fval;
                switch (sample_size) {
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_U8:
                    ival = ldub_phys(address_space_memory, addr);
                    fval = uint8_to_float((uint8_t)(ival & 0xff));
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16:
                    ival = lduw_le_phys(address_space_memory, addr);
                    fval = int16_to_float((int16_t)(ival & 0xffff));
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24:
                    ival = ldl_le_phys(address_space_memory, addr);
                    fval = int24_to_float(ival);
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S32:
                    ival = ldl_le_phys(address_space_memory, addr);
                    fval = int32_to_float(ival);
                    break;
                default:
                    fval = 0.0f;
                    break;
                }
                samples[sample_count][channel] = fval;
                addr += container_size;
            }
            if (lag_quiet) {
                /* Its first lap, before the lag has passed: silence, so
                 * a sound starts late rather than out of order. */
                samples[sample_count][0] = samples[sample_count][1] = 0.0f;
                lag_quiet = 0;
            }
        }

        if (!stereo) {
            samples[sample_count][1] = samples[sample_count][0];
        }
    }
    if (lag_this)
        s_lag_last_cbo[v] = cbo;

    /* RECOMP_APU_MUTE_BA=lo-hi (hex): voices whose buffer starts in
     * [lo, hi) play silence -- a diagnostic: which voices a sound comes
     * from. (MoH: European Assault's music and speech share one stream,
     * buffer base 0x1E70..0x1E78; its whistle is fixed by RECOMP_APU_LAG.) */
    {
        static int mute_on = -1;
        static uint32_t mute_lo, mute_hi;
        if (mute_on < 0) {
            const char *e = getenv("RECOMP_APU_MUTE_BA");
            unsigned lo = 0, hi = 0;
            mute_on = e && sscanf(e, "%x-%x", &lo, &hi) == 2 && lo < hi;
            mute_lo = lo; mute_hi = hi;
            if (mute_on)
                fprintf(stderr, "[APU] voices with a buffer in %08X-%08X muted\n",
                        mute_lo, mute_hi);
        }
        if (mute_on && ba >= mute_lo && ba < mute_hi && sample_count > 0)
            memset(samples, 0, (size_t)sample_count * sizeof samples[0]);
        if (s_vstat_on > 0 && sample_count > 0) {
            /* With RECOMP_APU_VSTAT: each voice's buffer base when it
             * changes, the number a mute range is made from. */
            static uint32_t last_ba[MCPX_HW_MAX_VOICES];
            if (last_ba[v] != ba) {
                last_ba[v] = ba;
                fprintf(stderr, "[APU-VSTAT] v=%u base %08X ebo %u lbo %u loop %d\n",
                        v, ba, ebo, lbo, (int)loop);
            }
        }
    }
    if (sample_count > 0 && (lag_this || stream))
        music_tap(v, samples, sample_count, stream, adpcm, channels, ba, ebo, loop);
    vp_stat_samples(v, samples, sample_count, stat_addr,
                    voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                   0xFFFFFFFF));
    if (!stream && loop && !adpcm && sample_count > 0)
        vp_ring_check(d, v, ba, ebo, (uint32_t)block_size,
                      cbo - (uint32_t)sample_count, samples, sample_count);

    if (cbo >= ebo) {
        if (stream) {
            d->vp.ssl[v].ssl_seg += 1;
            cbo = 0;
            if (d->vp.ssl[v].ssl_seg < d->vp.ssl[v].count[ssl_index]) {
                /* Move to next segment */
            } else {
                int next_index = (ssl_index + 1) % 2;
                d->vp.ssl[v].ssl_index = next_index;
                d->vp.ssl[v].ssl_seg = 0;
                set_notify_status(d, v, MCPX_HW_NOTIFIER_SSLA_DONE + ssl_index,
                                  NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
            }
        } else {
            if (loop) {
                cbo = lbo;
            } else {
                cbo = ebo;
                voice_off(d, (uint16_t)v);
            }
        }
    }

    voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                   NV_PAVS_VOICE_PAR_OFFSET_CBO, cbo);
    return sample_count;
}

/* ============================================================
 * Voice resampling (linear interpolation in place of libsamplerate)
 *
 * libsamplerate is stubbed; voice_resample below interpolates linearly
 * between source frames at the pitch's ratio, keeping its position and
 * the last frame across calls in the voice's filter state.
 * ============================================================ */

static int voice_resample(MCPXAPUState *d, uint16_t v, float samples[][2],
                          int requested_num, float rate)
{
    /* Linear interpolation in place of libsamplerate. `rate` is the output
     * to source ratio the VP's pitch encodes (libsamplerate's src_ratio):
     * 1.088 for 44.1 kHz material at the 48 kHz frame rate, so
     * each output consumes 1/rate source samples. Fetching one source sample
     * per output, as the stub before did, played everything a semitone and a
     * half high, 8.8 % too fast -- and drained each stream buffer faster than
     * the title filled it, which is where the periodic dropouts came from.
     *
     * State per voice: the two source samples the output sits between
     * (rs_last at 0, rs_next at 1) and the output's position rs_pos in
     * [0,1). Source samples arrive from voice_get_samples in the voice's own
     * units; a shortfall (the voice ended or its stream ran dry) returns the
     * outputs that could be made, and a fetch failure with nothing made is
     * passed through as it was. */
    MCPXAPUVoiceFilter *f = &d->vp.filters[v];
    float src[NUM_SAMPLES_PER_FRAME * 4 + 2][2];
    const int max_src = (int)(sizeof(src) / sizeof(src[0]));
    float step;
    int need, got, in, out;

    if (!(rate > 0.0f)) rate = 1.0f;
    s_snd_rate = rate;                     /* the sound dump names a sound by its source rate */
    step = 1.0f / rate;

    if (!f->rs_have) {
        got = voice_get_samples(d, v, src, 2);
        /* A first fetch that gets one sample returns no output and drops
         * that sample: the next call fetches two afresh. A voice whose
         * stream starts one sample at a time loses its first sample. */
        if (got < 2) return got < 0 ? got : 0;
        f->rs_last[0] = src[0][0]; f->rs_last[1] = src[0][1];
        f->rs_next[0] = src[1][0]; f->rs_next[1] = src[1][1];
        f->rs_pos = 0.0f;
        f->rs_have = 1;
    }

    /* Source samples the next requested_num outputs will step past, exactly:
     * output k sits at rs_pos + k * step, and every whole unit that position
     * reaches before the last output is a source sample consumed. The last
     * output's own step is left pending in rs_pos for the next call, so
     * nothing fetched here goes unused -- fetching floor(rs_pos + n * step)
     * instead over-read by up to one sample a frame, which was dropped: the
     * music came out 3 % fast. A step above about 4.2 (rate below 0.24, a
     * voice pitched more than two octaves up) wants more than the 130-frame
     * buffer holds over 31 steps; make fewer outputs rather than lose
     * source, so such a voice comes out short (and late) each frame. */
    for (;;) {
        need = (int)floorf(f->rs_pos + (float)(requested_num - 1) * step);
        if (need <= max_src || requested_num <= 1) break;
        requested_num /= 2;
    }
    if (need > max_src) need = max_src;
    if (need < 0) need = 0;

    got = need > 0 ? voice_get_samples(d, v, src, need) : 0;
    if (got < 0) got = 0;

    for (in = 0, out = 0; out < requested_num; out++) {
        while (f->rs_pos >= 1.0f) {
            if (in >= got) goto done;
            f->rs_last[0] = f->rs_next[0]; f->rs_last[1] = f->rs_next[1];
            f->rs_next[0] = src[in][0];    f->rs_next[1] = src[in][1];
            in++;
            f->rs_pos -= 1.0f;
        }
        samples[out][0] = f->rs_last[0] + f->rs_pos * (f->rs_next[0] - f->rs_last[0]);
        samples[out][1] = f->rs_last[1] + f->rs_pos * (f->rs_next[1] - f->rs_last[1]);
        f->rs_pos += step;
    }
done:
    if (out == 0 && got == 0) {
        /* Nothing new and nothing left to interpolate: the voice has stopped
         * or its stream is empty. voice_process treats 0 as an underrun and
         * a negative as the end; the voice's active bit says which. */
        int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                    NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
        return active ? 0 : -1;
    }
    return out;
}

/* ============================================================
 * Voice processing (main per-voice function)
 * ============================================================ */

static void voice_process(MCPXAPUState *d,
                          float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME],
                          float sample_buf[NUM_SAMPLES_PER_FRAME][2],
                          uint16_t v, int voice_list)
{
    assert(v < MCPX_HW_MAX_VOICES);
    bool stereo = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_STEREO) != 0;
    unsigned int channels = stereo ? 2 : 1;
    bool paused = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                 NV_PAVS_VOICE_PAR_STATE_PAUSED) != 0;

    struct McpxApuDebugVoice *dbg = &g_dbg.vp.v[v];
    dbg->active = true;
    dbg->stereo = stereo;
    dbg->paused = paused;

    if (paused) return;

    /* Step filter envelope */
    float ef_value = voice_step_envelope(
        d, v, NV_PAVS_VOICE_CFG_ENV1, NV_PAVS_VOICE_CFG_ENVF,
        NV_PAVS_VOICE_CFG_MISC, NV_PAVS_VOICE_CFG_MISC_EF_RELEASERATE,
        NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EFLVL,
        NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, NV_PAVS_VOICE_PAR_STATE_EFCUR);
    if (ef_value < 0.0f) ef_value = 0.0f;
    if (ef_value > 1.0f) ef_value = 1.0f;

    int16_t p = (int16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                                         NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH);
    int8_t ps = (int8_t)voice_get_mask(d, v, NV_PAVS_VOICE_CFG_ENV0,
                                        NV_PAVS_VOICE_CFG_ENV0_EF_PITCHSCALE);
    float rate = 1.0f / powf(2.0f, (p + ps * 32 * ef_value) / 4096.0f);
    dbg->rate = rate;

    /* Step amplitude envelope */
    float ea_value = voice_step_envelope(
        d, v, NV_PAVS_VOICE_CFG_ENV0, NV_PAVS_VOICE_CFG_ENVA,
        NV_PAVS_VOICE_TAR_LFO_ENV, NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE,
        NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_EALVL,
        NV_PAVS_VOICE_CUR_ECNT_EACOUNT, NV_PAVS_VOICE_PAR_STATE_EACUR);
    if (ea_value < 0.0f) ea_value = 0.0f;
    if (ea_value > 1.0f) ea_value = 1.0f;

    float samples[NUM_SAMPLES_PER_FRAME][2];
    memset(samples, 0, sizeof(samples));

    bool multipass = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS) != 0;
    dbg->multipass = multipass;

    if (multipass) {
        /* Read from multipass bin */
        int mp_bin = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS_BIN);
        dbg->multipass_bin = (uint8_t)mp_bin;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            samples[i][0] = mixbins[mp_bin][i];
            samples[i][1] = mixbins[mp_bin][i];
        }
        bool clear_mix = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                        NV_PAVS_VOICE_CFG_FMT_CLEAR_MIX) != 0;
        if (clear_mix) {
            memset(&mixbins[mp_bin][0], 0, sizeof(mixbins[0]));
        }
    } else {
        int empty = 0;
        for (int sample_count = 0; sample_count < NUM_SAMPLES_PER_FRAME;) {
            int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                        NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
            if (!active) return;
            int count = voice_resample(d, v, &samples[sample_count],
                                       NUM_SAMPLES_PER_FRAME - sample_count, rate);
            if (count < 0) break;
            /* An active voice that keeps producing nothing is an underrun,
             * not a reason to spin. Asking it again at once cannot change the
             * answer, and this loop holds d->lock: Burnout 3's vehicle select
             * sat here indefinitely while the title waited in voice_lock. */
            if (count == 0 && ++empty > 4) break;
            sample_count += count;
        }
    }

    int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
    if (!active) return;

    /* Get volume bins */
    int bin[8];
    bin[0] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V0BIN);
    bin[1] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V1BIN);
    bin[2] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V2BIN);
    bin[3] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V3BIN);
    bin[4] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V4BIN);
    bin[5] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V5BIN);
    bin[6] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_V6BIN);
    bin[7] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_V7BIN);

    if (v < MCPX_HW_MAX_3D_VOICES) {
        bin[0] = d->vp.hrtf_submix[0];
        bin[1] = d->vp.hrtf_submix[1];
        bin[2] = d->vp.hrtf_submix[2];
        bin[3] = d->vp.hrtf_submix[3];
    }

    uint16_t vol[8];
    vol[0] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME0);
    vol[1] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME1);
    vol[2] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME2);
    vol[3] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME3);
    vol[4] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME4);
    vol[5] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME5);
    vol[6] = (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME6_B11_8) << 8);
    vol[6] |= (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME6_B7_4) << 4);
    vol[6] |= (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME6_B3_0);
    vol[7] = (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME7_B11_8) << 8);
    vol[7] |= (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME7_B7_4) << 4);
    vol[7] |= (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME7_B3_0);

    for (int i = 0; i < 8; i++) {
        dbg->bin[i] = (uint8_t)bin[i];
        dbg->vol[i] = vol[i];
    }
    if (s_vstat_on > 0) {
        s_vstat[v].ea = ea_value;
        s_vstat[v].rate = rate;
        s_vstat[v].vol0 = vol[0]; s_vstat[v].vol1 = vol[1];
        s_vstat[v].bin0 = (uint8_t)bin[0]; s_vstat[v].bin1 = (uint8_t)bin[1];
    }

    if (voice_should_mute(v)) return;

    /* Low-pass filter */
    int fmode = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_MISC,
                               NV_PAVS_VOICE_CFG_MISC_FMODE);
    bool lpf = false;
    if (v < MCPX_HW_MAX_3D_VOICES) {
        lpf = (fmode == 1);
    } else {
        lpf = stereo ? (fmode == 1) : (fmode & 1) != 0;
    }
    /* RECOMP_APU_LPF=0 skips the per-voice low-pass (for the rear-sound
     * A/B). RECOMP_APU_3D also logs the filter's raw settings and the peak
     * before it, so a "silent" voice can be told from a muffled one. */
    static int lpf_on = -1, lpf_dbg = -1;
    if (lpf_on < 0) {
        const char *e = getenv("RECOMP_APU_LPF");
        lpf_on = !(e && e[0] == '0');
        lpf_dbg = apu_env_on("RECOMP_APU_3D");
    }
    float pk_pre = 0.0f;
    int fcraw0 = 0, fqraw0 = 0, fcraw1 = 0, fqraw1 = 0;
    if (lpf_dbg) {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
            pk_pre = fmaxf(pk_pre, fmaxf(fabsf(samples[i][0]), fabsf(samples[i][1])));
        fcraw0 = (int16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_FCA,
                                         NV_PAVS_VOICE_TAR_FCA_FC0);
        fqraw0 = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_FCA,
                                          NV_PAVS_VOICE_TAR_FCA_FC1);
        fcraw1 = (int16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_FCA + 4,
                                         NV_PAVS_VOICE_TAR_FCA_FC0);
        fqraw1 = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_FCA + 4,
                                          NV_PAVS_VOICE_TAR_FCA_FC1);
    }
    if (lpf && lpf_on) {
        for (int ch = 0; ch < 2; ch++) {
            int16_t fc = (int16_t)voice_get_mask(
                d, v, NV_PAVS_VOICE_TAR_FCA + (ch % channels) * 4,
                NV_PAVS_VOICE_TAR_FCA_FC0);
            float fc_f = clampf(powf(2, fc / 4096.0f), 0.003906f, 1.0f);
            uint16_t q = (uint16_t)voice_get_mask(
                d, v, NV_PAVS_VOICE_TAR_FCA + (ch % channels) * 4,
                NV_PAVS_VOICE_TAR_FCA_FC1);
            float q_f = clampf(q / (1.0f * 0x8000), 0.079407f, 1.0f);
            sv_filter *filter = &d->vp.filters[v].svf[ch];
            setup_svf(filter, fc_f, q_f, F_LP);
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                samples[i][ch] = run_svf(filter, samples[i][ch]);
                samples[i][ch] = fminf(fmaxf(samples[i][ch], -1.0f), 1.0f);
            }
        }
    }

    /* HRTF processing for 3D voices. RECOMP_APU_HRTF=0 turns it off: it is
     * on by default (xemu's), and so far checked on one title. */
    static int hrtf_on = -1;
    if (hrtf_on < 0) {
        const char *e = getenv("RECOMP_APU_HRTF");
        hrtf_on = !(e && e[0] == '0');
    }
    if (v < MCPX_HW_MAX_3D_VOICES && g_config.audio.hrtf && hrtf_on) {
        uint16_t hrtf_handle =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CFG_HRTF_TARGET,
                                     NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE);
        if (hrtf_handle != HRTF_NULL_HANDLE) {
            hrtf_filter_process(&d->vp.filters[v].hrtf, samples, samples);
        }
    }

    /* RECOMP_APU_3D=1: every 150 VP frames (0.1 s), one line per active voice (not the
     * idle multipass ones, 64-67) with the bins and volumes the guest set, for
     * the rear-sound complaint. */
    {
        static int dbg3d = -1;
        unsigned dbg3d_frame = s_lag_frame;
        if (dbg3d < 0) dbg3d = apu_env_on("RECOMP_APU_3D");
        if (dbg3d && (v < 64 || v > 67)) {
            if (dbg3d_frame % 150 == 0) {
                float pk = 0.0f;
                for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
                    pk = fmaxf(pk, fmaxf(fabsf(samples[i][0]), fabsf(samples[i][1])));
                fprintf(stderr, "[3D] t=%u f=%u v=%u hrtfh=%u bins=%u,%u,%u,%u,%u,%u,%u,%u "
                        "vol=%X,%X,%X,%X,%X,%X,%X,%X ea=%.3f peak=%.3f "
                        "pre=%.3f fmode=%d lpf=%d fc=%d/%d q=%u/%u st=%d\n",
                        VEV_NOW(), dbg3d_frame, (unsigned)v,
                        (unsigned)voice_get_mask(d, v, NV_PAVS_VOICE_CFG_HRTF_TARGET,
                                                 NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE),
                        bin[0], bin[1], bin[2], bin[3], bin[4], bin[5], bin[6], bin[7],
                        vol[0], vol[1], vol[2], vol[3], vol[4], vol[5], vol[6], vol[7],
                        ea_value, pk, pk_pre, fmode, (int)(lpf && lpf_on),
                        fcraw0, fcraw1, (unsigned)fqraw0, (unsigned)fqraw1,
                        (int)stereo);
            }
        }
    }

    /* Mix into bins */
    for (int b = 0; b < 8; b++) {
        float g = ea_value;
        float hr;
        if ((v < MCPX_HW_MAX_3D_VOICES) && (b < 4)) {
            hr = (float)(1 << d->vp.hrtf_headroom);
        } else {
            hr = (float)(1 << d->vp.submix_headroom[bin[b]]);
        }
        g *= attenuate(vol[b]) / hr;
        /* MoH's own 3D pan leaves sounds behind the player 8-13 dB quieter
         * (they fade out as you turn around). RECOMP_REAR_BOOST_DB (default
         * 0 = off) lifts the rear-left/right bins of effect voices only;
         * the level stream (224-229, music/speech) is left alone. */
        if ((bin[b] == 4 || bin[b] == 5) && v >= 68 && v < 224) {
            static float rear_boost = -1.0f;
            if (rear_boost < 0.0f) {
                const char *e = getenv("RECOMP_REAR_BOOST_DB");
                float db = (e && *e) ? (float)atof(e) : 0.0f;
                rear_boost = powf(10.0f, db / 20.0f);
            }
            g *= rear_boost;
        }
        /* The volumes are target registers: the hardware moves the level
         * toward them rather than jumping. Stepping to each frame's level
         * every 32 samples turned a voice whose volume the title changes
         * every frame (an engine that follows the revs) into a buzz at the
         * frame rate. Ramp from the gain the last frame ended on. */
        MCPXAPUVoiceFilter *f = &d->vp.filters[v];
        float g0 = f->gain_valid ? f->gain_prev[b] : g;
        float step = (g - g0) / NUM_SAMPLES_PER_FRAME;
        f->gain_prev[b] = g;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            mixbins[bin[b]][i] += (g0 + step * (i + 1)) * samples[i][b % channels];
        }
    }
    d->vp.filters[v].gain_valid = true;

    /* VP monitor mix */
    if (d->monitor.point == MCPX_APU_DEBUG_MON_VP) {
        float g = 0.0f;
        for (int b = 0; b < 8; b++) {
            float hr = (float)(1 << d->vp.submix_headroom[bin[b]]);
            float bg = attenuate(vol[b]) / hr;
            if (bg > g) g = bg;
        }
        g *= ea_value;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            sample_buf[i][0] += g * samples[i][0];
            sample_buf[i][1] += g * samples[i][1];
        }
    }

    (void)voice_list;
}

/* ============================================================
 * VP Frame - Process all voice lists
 *
 * Simplified single-threaded version (no worker threads initially)
 * ============================================================ */

/* RECOMP_APU_TRACE: every 60 VP frames, each active voice's play offset and
 * end offset, in samples (CBO and EBO as the VP holds them). A title that
 * times video or game events off a voice's play position follows this
 * clock; set against the flip count it shows whether audio runs fast or
 * slow relative to the frames. */
static unsigned s_pos_frames;

static void vp_trace_positions(MCPXAPUState *d)
{
    if (s_vstat_on < 0)
        vp_stat_init();
    if (s_vstat_on <= 0 || ++s_pos_frames % 60)
        return;

    for (int list = 0; list < 3; list++) {
        uint32_t h = d->regs[voice_list_regs[list].top];
        for (int i = 0; h != 0xFFFF && i < MCPX_HW_MAX_VOICES; i++) {
            uint16_t v = (uint16_t)h;
            if (voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                               NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE))
                fprintf(stderr, "[APU] voice %u pos=%u/%u\n", (unsigned)v,
                        voice_get_mask(d, v, NV_PAVS_VOICE_PAR_OFFSET,
                                       NV_PAVS_VOICE_PAR_OFFSET_CBO),
                        voice_get_mask(d, v, NV_PAVS_VOICE_PAR_NEXT,
                                       NV_PAVS_VOICE_PAR_NEXT_EBO));
            h = voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                               NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
        }
    }
}

void mcpx_apu_vp_frame(MCPXAPUState *d,
                        float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    s_lag_frame++;                     /* RECOMP_APU_LAG's frame stamp */
    {
        /* F8 in the game window (fb_present.c) drops a marker into the
         * RECOMP_APU_3D log: "now the rear sounds are gone". */
        static LONG seen_mark;
        LONG m = g_audio_mark;
        if (m != seen_mark) {
            seen_mark = m;
            fprintf(stderr, "[USERMARK] %ld f=%u t=%u\n", (long)m, (unsigned)s_lag_frame, VEV_NOW());
            if (m == 1) pan_dump("f8-first");
        }
    }
    memset(d->vp.sample_buf, 0, sizeof(d->vp.sample_buf));

    for (int list = 0; list < 3; list++) {
        hwaddr top, current, next;
        top = voice_list_regs[list].top;
        current = voice_list_regs[list].current;
        next = voice_list_regs[list].next;

        d->regs[current] = d->regs[top];

        for (int i = 0; d->regs[current] != 0xFFFF; i++) {
            if (i >= MCPX_HW_MAX_VOICES) {
                DPRINTF("Voice list contains invalid entry!\n");
                break;
            }

            uint16_t v = (uint16_t)d->regs[current];
            d->regs[next] = voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                               NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);

            if (!voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)) {
                /* One trap at a time: once the front end is trapped, a second
                 * idle voice in the same frame would overwrite FEDECPARAM
                 * before the ISR has read the first. The rest trap on later
                 * frames, after the ISR has resumed the front end. */
                if ((d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE)
                        != NV_PAPU_FECTL_FEMETHMODE_TRAPPED)
                    fe_method(d, SE2FE_IDLE_VOICE, v);
            } else {
                /* Process voice directly (single-threaded) */
                s_ring_visit[v] = s_lag_frame;
                voice_spin_lock(d, v);
                voice_process(d, mixbins, d->vp.sample_buf, v, list);
                voice_spin_unlock(d, v);
            }
            d->regs[current] = d->regs[next];
        }
    }

    /* Ring voices restarted by apu_ring_rearm: the title's DSound already took them out of its voice list, so play them
     * here (as 2D voices) until they stop; a voice the list reached this frame is not played twice. */
    for (uint16_t v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        if (!s_ring_pin[v])
            continue;
        if (!voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)) {
            s_ring_pin[v] = 0;
            continue;
        }
        if (s_ring_visit[v] == s_lag_frame)
            continue;
        voice_spin_lock(d, v);
        voice_process(d, mixbins, d->vp.sample_buf, v, 0);
        voice_spin_unlock(d, v);
    }

    {
        /* RECOMP_APU_3D: the loudest sample in each of the six speaker bins
         * after every voice has been mixed in, every 150 frames. */
        static int mix_on = -1;
        if (mix_on < 0) mix_on = apu_env_on("RECOMP_APU_3D");
        if (mix_on && s_lag_frame % 150 == 0) {
            float pkb[6] = {0};
            for (int b = 0; b < 6; b++)
                for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
                    pkb[b] = fmaxf(pkb[b], fabsf(mixbins[b][i]));
            fprintf(stderr, "[MIX] t=%u f=%u FL=%.3f FR=%.3f FC=%.3f LFE=%.3f RL=%.3f RR=%.3f\n",
                    VEV_NOW(), (unsigned)s_lag_frame,
                    pkb[0], pkb[1], pkb[2], pkb[3], pkb[4], pkb[5]);
        }
    }

    vp_trace_positions(d);
    vp_stat_report();
    if (s_ring_on > 0 && ++s_ring_frames % 1500 == 0)
        vp_ring_report(++s_ring_secs);

    /* VP monitor output */
    if (d->monitor.point == MCPX_APU_DEBUG_MON_VP) {
        int16_t isamp[NUM_SAMPLES_PER_FRAME * 2];
        src_float_to_short_array((float *)d->vp.sample_buf, isamp,
                                 NUM_SAMPLES_PER_FRAME * 2);
        int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            d->monitor.frame_buf[off + i][0] += isamp[2 * i];
            d->monitor.frame_buf[off + i][1] += isamp[2 * i + 1];
        }
        memset(d->vp.sample_buf, 0, sizeof(d->vp.sample_buf));
        memset(mixbins, 0, sizeof(float) * NUM_MIXBINS * NUM_SAMPLES_PER_FRAME);
    }
}

/* ============================================================
 * VP Init / Finalize / Reset
 * ============================================================ */

void mcpx_apu_vp_init(MCPXAPUState *d)
{
    /* Single-threaded - no worker dispatch needed */
    (void)d;
}

/* Shutdown, after the frame thread has stopped: close the voice dumps, free
 * the ring monitors and clear the statistics, so a new instance reads the
 * switches again and starts from nothing. */
void mcpx_apu_vp_finalize(MCPXAPUState *d)
{
    (void)d;
    if (s_vdump)
        fclose(s_vdump);
    s_vdump = NULL;
    for (unsigned v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        if (s_vdump_all[v])
            fclose(s_vdump_all[v]);
        s_vdump_all[v] = NULL;
        free(s_ring[v]);
        s_ring[v] = NULL;
    }
    memset(s_vdump_failed, 0, sizeof(s_vdump_failed));
    memset(s_vstat, 0, sizeof(s_vstat));
    s_vstat_on = -1;
    s_vdump_voice = -1;
    s_vstat_frames = s_vstat_secs = s_pos_frames = 0;
    s_ring_on = -1;
    s_ring_logged = 0;
    s_ring_frames = s_ring_secs = 0;
}

void mcpx_apu_vp_reset(MCPXAPUState *d)
{
    d->vp.ssl_base_page = 0;
    d->vp.hrtf_headroom = 0;
    memset(d->vp.ssl, 0, sizeof(d->vp.ssl));
    memset(d->vp.hrtf_submix, 0, sizeof(d->vp.hrtf_submix));
    memset(d->vp.submix_headroom, 0, sizeof(d->vp.submix_headroom));
    memset((void *)d->vp.voice_locked, 0, sizeof(d->vp.voice_locked));
    for (int v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        hrtf_filter_init(&d->vp.filters[v].hrtf);
    }
}
