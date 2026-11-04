/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <math.h>

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

/* Thread-local in the runtime: a plain extern would name the TLS template,
 * not the calling thread's copy (see main.c). */
extern __declspec(thread) uint32_t g_eax, g_esp;
extern ptrdiff_t g_xbox_mem_offset;

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    /* Missed by the disassembler; see game_patches.c. */
    extern void sub_001D68F0(void);
    if (xbox_va == 0x001D68F0u)
        return sub_001D68F0;

    /* The vblank callback, for RECOMP_60FPS; see game_patches.c. */
    extern void game_vblank_callback(void);
    if (xbox_va == 0x00013130u)
        return game_vblank_callback;

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

/* ── D3D fence semaphore ───────────────────────────────────── */

/* sub_002DE0C0 is XDK D3D_BlockOnTime: it spins until the GPU semaphore at
 * *(device + 0x30) reaches the fence. The pushbuffer executor only writes
 * BACK_END_WRITE_SEMAPHORE_RELEASE (0x1D70) once it is told where that
 * semaphore lives, so on the first wait hand it over
 * (docs/technical/d3d8ltcg-device-context.md). The device pointer is at
 * 0x002E9548. Needs RECOMP_PB_EXEC=1. */
void nv2a_pb_set_semaphore_target(uint32_t guest_va);

#define GAME_D3D_DEVICE_PTR 0x002E9548u

/* D3D's interrupt handler for software method 5, which BlockOnTime pushes
 * (NO_OPERATION 5) before sleeping on the device's event at +0x1DCC when the
 * GPU is far behind: the handler sets that event. Nothing raises GPU
 * interrupts here, so without this the title slept for good -- with the GPU
 * renderer the executor keeps up well enough that D3D takes that path, and
 * the level load hung in BlockOnResource. */
void nv2a_pb_set_swmethod_handler(void (*fn)(uint32_t param));
void xbox_KeSetEventGuest(uint32_t guest_va);
#define GAME_D3D_BUSY_EVENT 0x1DCCu

/* RECOMP_BLOCK_STATS=1: BlockOnTime (sub_002DE0C0) calls a second, how far
 * the GPU's time was behind D3D's at each, and how long the executor took
 * to reach the sw-method a sleeping one waits for (a full drain). */
extern uint64_t xbox_HostNowNs(void);
static int s_blk_stats = -1;
static volatile uint64_t s_blk_enter_ns;
static uint64_t s_blk_drain_ns, s_blk_t0, s_blk_gap;
static unsigned s_blk_calls, s_blk_signals, s_blk_gapmax;
/* who calls BlockOnTime: the three nearest return addresses on the guest stack, with calls / sleeps / drain time */
static struct { uint32_t k[3]; unsigned calls, slept; uint64_t drain_ns; } s_blk_tab[64];
static int s_blk_cur = -1, s_blk_n;

static void game_d3d_swmethod(uint32_t param)
{
    if ((param & 0x1Fu) == 5u) {
        uint32_t dev = *(volatile uint32_t *)((uintptr_t)0x002E9548u + g_xbox_mem_offset);
        if (s_blk_stats > 0 && s_blk_enter_ns) {
            uint64_t dn = xbox_HostNowNs() - s_blk_enter_ns;
            s_blk_drain_ns += dn;
            s_blk_enter_ns = 0;
            s_blk_signals++;
            if (s_blk_cur >= 0) {
                s_blk_tab[s_blk_cur].slept++;
                s_blk_tab[s_blk_cur].drain_ns += dn;
            }
        }
        if (dev)
            xbox_KeSetEventGuest(dev + GAME_D3D_BUSY_EVENT);
    }
}

/* D3D's pushbuffer ring (the commands the game writes for the GPU) is 512 KB by default
 * (sub_002D9CE0 stores 0x80000 at 0x2EBCEC, the kick size 0x8000 at 0x2EBCE8). A frame of ~1000 draws is
 * ~340 KB, so the game wraps onto commands the executor has not finished and sleeps for them
 * (BlockOnTime from sub_002DE230, ~1 ms, about every other frame). RECOMP_PUSHBUF_KB=<n> makes the ring
 * n KB (a power of two, 512..16384), allocated by the same code at device creation. */
uint32_t game_pushbuf_size(uint32_t def)
{
    const char *e = getenv("RECOMP_PUSHBUF_KB");
    unsigned kb = e ? (unsigned)strtoul(e, NULL, 0) : 0;
    if (kb >= 512 && kb <= 16384 && (kb & (kb - 1)) == 0) {
        fprintf(stderr, "[D3D] pushbuffer %u KB (the game's default %u KB)\n", kb, def / 1024);
        return kb * 1024u;
    }
    return def;
}

/* BlockOnResource (sub_002DE800 -> sub_002DE3F0) as the engine's render target switch (sub_001CDD70,
 * sub_001CD5C0) calls it: it waits until the executor has drained everything that used the surface, about
 * two full drains a frame (~12% of the frame). The executor is in order, so the GPU side needs no such
 * wait; RECOMP_SKIP_RT_WAIT=1 skips it at those three call sites (the caller is told by the return address).
 * Switch it off again if reflections / post effects show stale or torn pictures. */
int sub_002DE800_skip(void)
{
    static int on = -1;
    static unsigned skipped;
    uint32_t ret;
    if (on < 0) {
        const char *e = getenv("RECOMP_SKIP_RT_WAIT");
        on = e && *e == '1';
    }
    if (!on)
        return 0;
    ret = *(volatile uint32_t *)((uintptr_t)(uint32_t)g_esp + g_xbox_mem_offset);
    if (ret == 0x001CDDD7u || ret == 0x001CD6EEu || ret == 0x001CD7C9u) {
        if (++skipped == 1 || skipped % 100000 == 0)
            fprintf(stderr, "[RTWAIT] skipped %u waits\n", skipped);
        return 1;
    }
    return 0;
}

void sub_002DE0C0_enter(void)
{
    static int done;
    uint32_t dev, sem;

    if (s_blk_stats < 0)
        s_blk_stats = getenv("RECOMP_BLOCK_STATS") != NULL;
    if (s_blk_stats > 0 && done) {
        uint64_t now = xbox_HostNowNs();
        uint32_t d = *(volatile uint32_t *)((uintptr_t)GAME_D3D_DEVICE_PTR + g_xbox_mem_offset);
        uint32_t cur = *(volatile uint32_t *)((uintptr_t)(d + 0x2C) + g_xbox_mem_offset);
        uint32_t sv = *(volatile uint32_t *)((uintptr_t)(d + 0x30) + g_xbox_mem_offset);
        uint32_t gpu = *(volatile uint32_t *)((uintptr_t)sv + g_xbox_mem_offset);
        uint32_t gap = cur - gpu;
        s_blk_calls++;
        s_blk_gap += gap;
        if (gap > s_blk_gapmax)
            s_blk_gapmax = gap;
        s_blk_enter_ns = now;
        {   /* the caller chain: the first three code addresses on the stack */
            uint32_t k[3] = { 0, 0, 0 }, sp = (uint32_t)g_esp, i, nk = 0;
            int j;
            for (i = 0; i < 96 && nk < 3; i++) {
                uint32_t v = *(volatile uint32_t *)((uintptr_t)(sp + i * 4u) + g_xbox_mem_offset);
                if (v >= 0x00011000u && v < 0x002D8000u)
                    k[nk++] = v;
            }
            for (j = 0; j < s_blk_n; j++)
                if (!memcmp(s_blk_tab[j].k, k, sizeof k))
                    break;
            if (j == s_blk_n && s_blk_n < 64) {
                memcpy(s_blk_tab[j].k, k, sizeof k);
                s_blk_n++;
            }
            s_blk_cur = j < s_blk_n ? j : -1;
            if (s_blk_cur >= 0)
                s_blk_tab[s_blk_cur].calls++;
        }
        if (!s_blk_t0)
            s_blk_t0 = now;
        else if (now - s_blk_t0 >= 1000000000ull) {
            fprintf(stderr, "[BLOCK] 1 s: %u BlockOnTime, mean gap %.1f (max %u),"
                    " %u slept, mean drain %.2f ms, total %.1f ms\n",
                    s_blk_calls, s_blk_calls ? (double)s_blk_gap / s_blk_calls : 0.0,
                    s_blk_gapmax, s_blk_signals,
                    s_blk_signals ? s_blk_drain_ns / 1e6 / s_blk_signals : 0.0,
                    s_blk_drain_ns / 1e6);
            {   /* every 5th second: the callers that sleep the longest */
                static int sec;
                if (++sec % 5 == 0) {
                    int a, b, shown = 0;
                    for (a = 0; a < s_blk_n && shown < 6; a++) {
                        int best = -1;
                        for (b = 0; b < s_blk_n; b++)
                            if (s_blk_tab[b].calls && (best < 0 || s_blk_tab[b].drain_ns > s_blk_tab[best].drain_ns))
                                best = b;
                        if (best < 0)
                            break;
                        fprintf(stderr, "[BLOCKER] %08X <- %08X <- %08X : %u calls, %u slept, %.1f ms drained\n",
                                s_blk_tab[best].k[0], s_blk_tab[best].k[1], s_blk_tab[best].k[2],
                                s_blk_tab[best].calls, s_blk_tab[best].slept, s_blk_tab[best].drain_ns / 1e6);
                        s_blk_tab[best].calls = 0;
                        shown++;
                    }
                    memset(s_blk_tab, 0, sizeof s_blk_tab);
                    s_blk_n = 0;
                    s_blk_cur = -1;
                }
            }
            s_blk_calls = s_blk_signals = s_blk_gapmax = 0;
            s_blk_gap = s_blk_drain_ns = 0;
            s_blk_t0 = now;
        }
    }
    if (done)
        return;
    dev = *(volatile uint32_t *)((uintptr_t)GAME_D3D_DEVICE_PTR + g_xbox_mem_offset);
    if (!dev)
        return;
    sem = *(volatile uint32_t *)((uintptr_t)(dev + 0x30) + g_xbox_mem_offset);
    if (!sem)
        return;
    nv2a_pb_set_semaphore_target(sem);
    nv2a_pb_set_swmethod_handler(game_d3d_swmethod);
    done = 1;
    fprintf(stderr, "[D3D] GPU semaphore at 0x%08X (device 0x%08X)\n", sem, dev);
}

/* ── DirectSound stream flush ──────────────────────────────── */

/* Before loading a level, sub_00196467 flushes the menu music stream
 * (IDirectSoundStream_FlushEx, sub_002ECCBF) and spins on GetStatus until
 * DSSTREAMSTATUS_PLAYING clears. Status comes from sub_002F15BD (CMcpxStream
 * in ecx), which reads the voice state word at +0x12: the flush asks the APU
 * to stop the voice, and only the APU's voice-stopped notification moves that
 * word on. The emulated APU never sends it, so the campaign never loads.
 *
 * Remember the stream FlushEx was called on. If it still reads as active
 * DS_STOP_GRACE_MS later, clear the active/stopping bits the way the
 * notification would. Only inside DS_STOP_WINDOW_MS after the flush, so a
 * stream the title starts again later is left alone. Entry hooks, not
 * overrides: overriding these functions changed codegen elsewhere and broke
 * boot. */
#include <windows.h>

/* RECOMP_DEV_KEYS=1 (dev launchers only) turns on the developer hotkeys: End / Pause marks, the [ ] corpse detail keys,
 * and in the toolkit F2..F9 / F12. A player build ignores all of them. */
int game_dev_keys(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("RECOMP_DEV_KEYS");
        v = (e && *e && *e != '0') ? 1 : 0;
    }
    return v;
}

extern __declspec(thread) uint32_t g_ecx;

#define GUEST16(va) (*(volatile uint16_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define GUEST32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define GUEST8(va)  (*(volatile uint8_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define DS_STOP_GRACE_MS   500u
#define DS_STOP_WINDOW_MS 3000u

static volatile uint32_t s_flush_mcpx;     /* CMcpxStream of the last flush */
static volatile DWORD s_flush_tick;

/* Camera projection setup: this = camera, (fov, aspect, near, far) floats at
 * esp+4... The game calls it from the view update (return 0x18E2D2) a few times
 * every frame with different fovs (world, then a narrower one for the weapon,
 * then the world again), so each call position in the frame is tracked on its
 * own. RECOMP_FOV=<deg> widens the view, RECOMP_FAR_MULT the far plane,
 * RECOMP_FOV_BLEND_MS eases the jumps the game makes (scopes), 0 = off. */
#define FOV_ROLES 4
typedef struct {
    float to, from, cur;                /* the game's own fov (degrees): target, blend start, shown */
    float wide;                         /* what was written into the camera's fov field last */
    double t0;
} FovRole;
static FovRole s_fovrole[FOV_ROLES];

extern volatile uint32_t g_game_frames;
extern volatile LONG g_mkb_fov_steps;       /* F6 / F7 presses (mkb_input.c) */

static double fov_now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

#define FOV_PI 3.14159265358979
#define FOV_TAN(deg) tan((double)(deg) * FOV_PI / 360.0)
#define FOV_ATAN(t)  ((float)(2.0 * atan(t) * 180.0 / FOV_PI))

static uint32_t s_last_ret;
static float s_last_in, s_last_out;

static float guest_f(uint32_t va)
{
    float f;
    uint32_t w = GUEST32(va);
    memcpy(&f, &w, 4);
    return f;
}

static void guest_set_f(uint32_t va, float f)
{
    uint32_t w;
    memcpy(&w, &f, 4);
    GUEST32(va) = w;
}

static int s_fov_on = -1;                   /* RECOMP_FOV_LOG=1 */
static float s_want_fov = -1.0f, s_far_mult = 1.0f, s_blend_ms = 160.0f;
static int s_fov_mode = 2;                  /* 2: widen the half angles where the game sets them; 1: rewrite the camera's fov field each view update; 0: only the projection call's argument (RECOMP_FOV_INPLACE) */

/* Settings from the launcher / fov_saved.txt, and the F6 / F7 steps. */
static void fov_poll(void)
{
    LONG steps;
    if (s_want_fov < 0.0f) {
        const char *e = getenv("RECOMP_FOV"), *f = getenv("RECOMP_FAR_MULT"), *b = getenv("RECOMP_FOV_BLEND_MS");
        const char *l = getenv("RECOMP_FOV_LOG"), *p = getenv("RECOMP_FOV_INPLACE");
        s_want_fov = e ? (float)atof(e) : 0.0f;        /* degrees for the normal view (the game's own is 70) */
        s_far_mult = f ? (float)atof(f) : 1.0f;
        if (b) s_blend_ms = (float)atof(b);
        s_fov_on = l && *l == '1';
        if (p && (*p == '0' || *p == '1')) s_fov_mode = *p - '0';
        if (s_want_fov != 0.0f && (s_want_fov < 30.0f || s_want_fov > 150.0f)) s_want_fov = 0.0f;
        if (s_far_mult < 1.0f || s_far_mult > 50.0f) s_far_mult = 1.0f;
        if (s_blend_ms < 0.0f || s_blend_ms > 2000.0f) s_blend_ms = 160.0f;
        {   /* the value chosen in the last session (F6/F7) wins over the launcher's */
            FILE *fp = fopen("fov_saved.txt", "r");
            float v;
            if (fp) {
                if (fscanf(fp, "%f", &v) == 1 && v >= 50.0f && v <= 140.0f)
                    s_want_fov = v;
                fclose(fp);
            }
        }
    }
    {   /* RECOMP_FOV_WWATCH_VA=0x8xxxxxxx: who writes that guest dword (armed at the first view setup) */
        static int armed;
        const char *w = getenv("RECOMP_FOV_WWATCH_VA");
        if (!armed && w && *w) {
            extern int xbox_WatchArmAt(uint32_t va, unsigned max_reports, int reads);
            armed = 1;
            xbox_WatchArmAt((uint32_t)strtoul(w, NULL, 0), 300, 0);
        }
    }
    steps = InterlockedExchange(&g_mkb_fov_steps, 0);
    if (steps) {                                /* F6 / F7: 5 degrees a step */
        float v = (s_want_fov != 0.0f ? s_want_fov : 70.0f) + 5.0f * (float)steps;
        FILE *fp;
        s_want_fov = v < 50.0f ? 50.0f : v > 140.0f ? 140.0f : v;
        fprintf(stderr, "[FOV] field of view now %.0f\n", s_want_fov);
        fp = fopen("fov_saved.txt", "w");
        if (fp) {
            fprintf(fp, "%.0f\n", s_want_fov);
            fclose(fp);
        }
    }
}

/* For the settings menu: the field of view in degrees (70 = the game's own), and a new one. */
float game_fov_get(void)
{
    fov_poll();
    return s_want_fov != 0.0f ? s_want_fov : 70.0f;
}

void game_fov_set(float v)
{
    FILE *fp;
    fov_poll();
    s_want_fov = v < 50.0f ? 50.0f : v > 140.0f ? 140.0f : v;
    fprintf(stderr, "[FOV] field of view now %.0f (settings menu)\n", s_want_fov);
    fp = fopen("fov_saved.txt", "w");
    if (fp) {
        fprintf(fp, "%.0f\n", s_want_fov);
        fclose(fp);
    }
}

/* One game frame's length in ms, a running average measured where the view is set each frame. */
static double s_frame_ms, s_frame_mark;
static void fov_frame_clock(void)
{
    double t = fov_now_ms(), d;
    if (s_frame_mark > 0.0 && (d = t - s_frame_mark) > 0.5 && d < 500.0)
        s_frame_ms = s_frame_ms > 0.0 ? s_frame_ms * 0.9 + d * 0.1 : d;
    s_frame_mark = t;
}

/* How long a jump is eased: RECOMP_FOV_BLEND_MS, but at least 8 game frames, so at 30 fps a scope jump shows about
 * 8 steps instead of 5 (5 steps read as a cut). Capped at 400 ms. From 60 fps up it stays RECOMP_FOV_BLEND_MS. */
static double fov_blend_span(void)
{
    double span = s_blend_ms;
    if (s_frame_ms * 8.0 > span)
        span = s_frame_ms * 8.0;
    return span > 400.0 ? 400.0 : span;
}

/* The game's fov for one call position -> the fov to use: eases jumps, then widens. */
static float fov_step(FovRole *r, float a1, double now)
{
    if (r->cur <= 0.0f) {
        r->cur = r->from = r->to = a1;
    } else if (a1 != r->to) {
        /* small steps are the game's own ramp and are followed as they are; a jump is eased */
        if (s_blend_ms > 0.0f && fabsf(a1 - r->to) > 3.0f) {
            r->from = r->cur;
            r->t0 = now;
        } else {
            r->from = a1;
        }
        r->to = a1;
    }
    if (r->from != r->to) {
        /* ease in zoom space (tan of the half angle): equal steps feel like a steady zoom */
        double u = (now - r->t0) / fov_blend_span(), t0, t1;
        if (u >= 1.0) {
            u = 1.0;
            r->from = r->to;
        }
        u = u * u * (3.0 - 2.0 * u);
        t0 = FOV_TAN(r->from);
        t1 = FOV_TAN(r->to);
        r->cur = FOV_ATAN(t0 + (t1 - t0) * u);
    } else {
        r->cur = a1;
    }
    a1 = r->cur;
    if (s_want_fov != 0.0f) {
        /* keep the zoom ratio against the 70 degree view: aiming and scopes widen with it */
        a1 = FOV_ATAN(FOV_TAN(a1) / FOV_TAN(70.0) * FOV_TAN(s_want_fov));
    }
    return a1;
}

static FovRole *fov_next_role(void)
{
    static uint32_t frame = 0xFFFFFFFFu;
    static int calls;
    FovRole *r;
    if (g_game_frames != frame) {
        frame = g_game_frames;
        calls = 0;
        fov_frame_clock();
    }
    r = &s_fovrole[calls < FOV_ROLES ? calls : FOV_ROLES - 1];
    calls++;
    return r;
}

/* sub_0018B480(this = camera params, halfH, halfV): the one place the view angle is set. It stores both
 * half angles' tangents (+0xB8, +0xBC) and the full angles (+0xC0, +0xC4), and the game's visibility tests
 * for characters and other objects read those. Widening the two half angles here (same zoom ratio against
 * the 70 degree view as before) makes every reader see the new view; jumps are eased per call position. */
static void draw_poll(void);
void sub_0018B480_enter(void)
{
    float h, v, tanh_, tanv, deg, wide, s;
    FovRole *r;
    double now;
    fov_poll();
    draw_poll();
    if (s_fov_mode != 2)
        return;
    h = guest_f(g_esp + 4);
    v = guest_f(g_esp + 8);
    if (!(h > 0.01f && h < 1.45f && v > 0.01f && v < 1.45f))
        return;
    tanh_ = (float)tan((double)h);
    tanv = (float)tan((double)v);
    if (!(tanv > 0.0f && tanh_ / tanv > 1.2f && tanh_ / tanv < 2.5f))
        return;                                 /* not the player's wide-screen view (the other cameras are square) */
    {   /* RECOMP_FOV_RWATCH=pfov:<hexoff>: who reads the camera params' view angle fields (use RECOMP_WATCH_RLEN=16 with pfov:B8) */
        static uint32_t armed3, seen_this[16], done3;
        static int seen_n[16], nthis;
        const char *w = getenv("RECOMP_FOV_RWATCH");
        if (w && !strncmp(w, "pfov:", 5)) {
            extern int xbox_WatchArmAt(uint32_t va, unsigned max_reports, int reads);
            extern void xbox_WatchDisarm(void);
            uint32_t self = (uint32_t)g_ecx, off = (uint32_t)strtoul(w + 5, NULL, 16);
            int k;
            for (k = 0; k < nthis && seen_this[k] != self; k++) ;
            if (k == nthis && nthis < 16)
                seen_this[nthis++] = self;
            if (k < 16)
                seen_n[k]++;
            /* follow the camera that lives longest: re-arm on a params object once it has been seen 600 times */
            if (k < 16 && seen_n[k] >= 600 && !(done3 >> k & 1u)) {
                done3 |= 1u << k;
                xbox_WatchDisarm();
                armed3 = self;
                fprintf(stderr, "[FOV] read watch on params %08X + %X (frame %u)\n", self, off, g_game_frames);
                xbox_WatchArmAt(self + off, 3000, 1);
            }
        }
    }
    r = fov_next_role();
    now = fov_now_ms();
    deg = h * 2.0f * 57.29578f;
    wide = fov_step(r, deg, now);               /* eased and widened full angle, degrees */
    s = (float)(FOV_TAN(wide) / tanh_);         /* the factor on tan(halfH); the same on tan(halfV) */
    if (s_fov_on > 0) {
        static int lines;
        if (fabsf(deg - r->to) > 3.0f && lines < 300) {
            fprintf(stderr, "[FOV] %.0f ms game fov %.3f -> %.3f\n", now, r->to, deg);
            lines++;
        }
        (void)lines;
    }
    guest_set_f(g_esp + 4, (float)atan(tanh_ * s));
    guest_set_f(g_esp + 8, (float)atan(tanv * s));
}

/* RECOMP_FOV_SCAN=1: once, some seconds into a level, list every guest dword that still holds the game's
 * original view angle in some form (radians, degrees, tan/cos/sin of the half angle, vertical tan). A
 * visibility test that ignores the widened field must be reading one of these. */
static void fov_scan(float rawdeg, float aspect)
{
    static const uint32_t regions[][2] = { {0x00000000u, 0x00800000u}, {0x80000000u, 0x84000000u} };
    double half = (double)rawdeg * FOV_PI / 360.0, ta = tan(half);
    struct { float v; const char *name; } t[] = {
        { (float)(half * 2.0), "rad" }, { rawdeg, "deg" }, { (float)half, "halfrad" }, { (float)ta, "tan(half)" },
        { (float)cos(half), "cos(half)" }, { (float)sin(half), "sin(half)" }, { (float)(1.0 / ta), "cot(half)" },
        { (float)(ta / aspect), "tan(vhalf)" }, { (float)cos(atan(ta / aspect)), "cos(vhalf)" },
        { (float)(half * 2.0 * 0.75), "rad*0.75" }, { (float)tan(half * 0.75), "tan(0.75half)" },
    };
    int hits = 0, r, k;
    for (r = 0; r < 2; r++) {
        uint32_t va = regions[r][0];
        while (va < regions[r][1] && hits < 300) {
            MEMORY_BASIC_INFORMATION mbi;
            uintptr_t host = (uintptr_t)va + g_xbox_mem_offset;
            uint32_t n;
            if (!VirtualQuery((void *)host, &mbi, sizeof mbi))
                break;
            n = (uint32_t)((uintptr_t)mbi.BaseAddress + mbi.RegionSize - host);
            if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && mbi.Protect != 0) {
                uint32_t off;
                for (off = 0; off + 4 <= n && va + off < regions[r][1] && hits < 300; off += 4) {
                    float f;
                    memcpy(&f, (const void *)(host + off), 4);
                    if (!(f > 0.01f && f < 200.0f))
                        continue;
                    for (k = 0; k < (int)(sizeof t / sizeof t[0]); k++)
                        if (fabsf(f - t[k].v) <= 2e-5f * fabsf(t[k].v) + 1e-6f) {
                            fprintf(stderr, "[SCAN] %08X = %.5f (%s)\n", va + off, f, t[k].name);
                            hits++;
                            break;
                        }
                }
            }
            if (n == 0)
                break;
            va += n;
        }
    }
    fprintf(stderr, "[SCAN] done, %d hits (raw %.3f deg, aspect %.3f)\n", hits, rawdeg, aspect);
}

/* The view update (this = view, camera params at +0xE4), several times a frame. Everything that reads the
 * params' fov (+0xC0), not only the projection call, has to see the widened value, or the game's own
 * visibility tests keep culling with the old view cone: characters and objects vanish at the screen edges.
 * So the field itself is rewritten here. The game writes it as an absolute value each time (its ramp
 * 70 -> 46 is then re-widened), and a value equal to the one written last time is not widened again. */
void sub_0018E150_enter(void)
{
    uint32_t view = g_ecx, params;
    float asp_w, asp_h, near_, far_, v, deg;
    FovRole *r;
    int k, mine = 0;
    double now;
    fov_poll();
    if (s_fov_mode != 1 || !view)
        return;
    params = GUEST32(view + 0xE4);
    if (!params || !GUEST8(params + 0x190))
        return;
    asp_w = guest_f(params + 0xB8);
    asp_h = guest_f(params + 0xBC);
    near_ = guest_f(params + 0xB0);
    far_  = guest_f(params + 0xB4);
    v     = guest_f(params + 0xC0);                 /* radians */
    if (!(asp_h > 0.0f && asp_w / asp_h > 1.2f && asp_w / asp_h < 2.5f && near_ < 0.5f && far_ >= 100.0f &&
          v > 0.02f && v < 2.9f))
        return;
    {   /* RECOMP_FOV_WWATCH=<offset>: who writes that dword of the camera params */
        static int armed;
        const char *w = getenv("RECOMP_FOV_WWATCH");
        if (!armed && w && *w) {
            extern int xbox_WatchArmAt(uint32_t va, unsigned max_reports, int reads);
            armed = 1;
            fprintf(stderr, "[FOV] write watch on %08X\n", params + (uint32_t)strtoul(w, NULL, 0));
            xbox_WatchArmAt(params + (uint32_t)strtoul(w, NULL, 0), 400, 0);
        }
    }
    {   /* RECOMP_FOV_SCAN=1: after ~1500 gameplay view updates */
        static int cnt, done;
        const char *w = getenv("RECOMP_FOV_SCAN");
        if (!done && w && *w == '1' && ++cnt == 1500) {
            done = 1;
            fov_scan(s_fovrole[0].to > 1.0f ? s_fovrole[0].to : v * 57.29578f, asp_w / asp_h);
        }
    }
    {   /* RECOMP_FOV_RWATCH=1: who reads the camera's fov field (needs the read watch of the memory layer) */
        static int armed;
        const char *w = getenv("RECOMP_FOV_RWATCH");
        if (!armed && w && *w == '1') {
            extern int xbox_WatchArmAt(uint32_t va, unsigned max_reports, int reads);
            armed = 1;
            fprintf(stderr, "[FOV] read watch on %08X\n", params + 0xC0);
            xbox_WatchArmAt(params + 0xC0, 1200, 1);
        }
    }
    r = fov_next_role();
    if (s_fov_on > 0) {
        static int ent;
        if (ent < 60) {
            fprintf(stderr, "[ENTRY] frame %u role %d v %.4f wide %.4f to %.3f\n",
                    g_game_frames, (int)(r - s_fovrole), v, r->wide, r->to);
            ent++;
        }
    }
    for (k = 0; k < FOV_ROLES; k++)
        if (s_fovrole[k].wide != 0.0f && v == s_fovrole[k].wide)
            mine = 1 + (&s_fovrole[k] == r);
    if (mine == 1)
        return;                                 /* a value of ours another position wrote: already wide */
    now = fov_now_ms();
    deg = mine ? r->to : v * 57.29578f;         /* still ours: the game's value is the target, an ease may go on */
    if (s_fov_on > 0) {
        static int lines;
        if (fabsf(deg - r->to) > 3.0f && lines < 300) {
            fprintf(stderr, "[FOV] %.0f ms game fov %.3f -> %.3f\n", now, r->to, deg);
            lines++;
        }
    }
    r->wide = fov_step(r, deg, now) / 57.29578f;
    guest_set_f(params + 0xC0, r->wide);
    if (r->from != r->to)
        GUEST8(params + 0x194) = 1;             /* an ease is running: rebuild the projection every frame */
}

/* The game's per-object draw distance test (sub_00130010 / sub_0012FCA0): an object is drawn while
 * dist^2 * s < maxdist^2, with s = camera view angle (radians) * K, which is 1 at the game's 70 degrees.
 * A wider view makes s > 1, so it hid objects at a shorter distance (the wider the fov the nearer), and a
 * narrower one (aiming) showed them farther. Here s is held at 1 at most (a wide view keeps the 70 degree
 * distance) and divided by RECOMP_DRAW_MULT^2: the multiplier makes every object's distance that many times
 * longer (RECOMP_DRAW_MULT=3: three times as far). */
extern volatile LONG g_mkb_draw_steps;          /* F4 / F5 presses (mkb_input.c) */
static float s_draw_inv = -1.0f, s_draw_mult = 1.0f;

static void draw_poll(void);

/* For the settings menu: the draw distance multiplier and a new one (1..20). */
float game_draw_get(void)
{
    draw_poll();
    return s_draw_mult;
}

void game_draw_set(float v)
{
    FILE *fp;
    draw_poll();
    s_draw_mult = v < 1.0f ? 1.0f : v > 20.0f ? 20.0f : v;
    s_draw_inv = 1.0f / (s_draw_mult * s_draw_mult);
    fprintf(stderr, "[DRAW] distance multiplier now %.2f (settings menu)\n", s_draw_mult);
    fp = fopen("draw_saved.txt", "w");
    if (fp) {
        fprintf(fp, "%.1f\n", s_draw_mult);
        fclose(fp);
    }
}

/* The multiplier in effect, for the game-thread sampler (main.c). */
float game_draw_mult_now(void)
{
    return s_draw_mult;
}

/* RECOMP_DRAW_MULT=<x> from the launcher, draw_saved.txt (the F4 / F5 choice of the last session) wins;
 * F4 / F5 step 0.5 (to 1 at the lowest, 20 at the highest) while playing. */
static void draw_poll(void)
{
    extern void game_wdef_poll(void);
    extern void game_prof_poll(void);
    extern void game_corpse_watch_poll(void);
    extern void game_vfx_poll(void);
    extern void game_rain_poll(void);
    LONG steps = InterlockedExchange(&g_mkb_draw_steps, 0);
    game_wdef_poll();
    game_prof_poll();
    game_corpse_watch_poll();
    game_vfx_poll();
    game_rain_poll();
    if (s_draw_inv < 0.0f) {
        const char *e = getenv("RECOMP_DRAW_MULT");
        FILE *fp = fopen("draw_saved.txt", "r");
        float v;
        s_draw_mult = e ? (float)atof(e) : 1.0f;
        if (fp) {
            if (fscanf(fp, "%f", &v) == 1 && v >= 0.5f && v <= 20.0f)
                s_draw_mult = v;
            fclose(fp);
        }
        if (!(s_draw_mult >= 0.25f && s_draw_mult <= 50.0f))
            s_draw_mult = 1.0f;
        s_draw_inv = 1.0f / (s_draw_mult * s_draw_mult);
        fprintf(stderr, "[DRAW] distance multiplier %.2f\n", s_draw_mult);
    }
    if (steps) {
        float v = s_draw_mult + 0.5f * (float)steps;
        FILE *fp;
        s_draw_mult = v < 1.0f ? 1.0f : v > 20.0f ? 20.0f : v;
        s_draw_inv = 1.0f / (s_draw_mult * s_draw_mult);
        fprintf(stderr, "[DRAW] distance multiplier now %.2f (game frame %u)\n", s_draw_mult, g_game_frames);
        fp = fopen("draw_saved.txt", "w");
        if (fp) {
            fprintf(fp, "%.1f\n", s_draw_mult);
            fclose(fp);
        }
    }
}

/* RECOMP_DRAW_ADAPT=1: the draw distance multiplier in use (s_draw_eff) follows the frame time. When the game loop
 * runs slower than 100 fps for a moment (a firefight, explosions: the game's own work grows, 7 ms -> 15 ms) it
 * comes down fast toward RECOMP_DRAW_ADAPT_MIN (1 = the game's own distance), taking the extra objects'
 * work with it; when the frames are quick again it climbs back slowly, never past the F4 / F5 value.
 * Called once per frame by game_sampler_note_thread (main.c) with the smoothed frame time in ms. */
static float s_draw_eff = -1.0f;
float game_draw_eff_now(void)
{
    return s_draw_eff > 0.0f ? s_draw_eff : s_draw_mult;
}

void game_draw_adapt(double ema_ms)
{
    static int on = -1;
    static float lo_mult = 1.0f;
    if (s_draw_inv < 0.0f)
        draw_poll();
    if (on < 0) {
        const char *e = getenv("RECOMP_DRAW_ADAPT"), *m = getenv("RECOMP_DRAW_ADAPT_MIN");
        on = e && *e && *e != '0';
        if (m && atof(m) >= 1.0)
            lo_mult = (float)atof(m);
        if (on)
            fprintf(stderr, "[DRAW] adaptive distance: down toward x%.1f while frames are slower than"
                    " 100 fps, back up to the F4 / F5 value when they are quick\n", lo_mult);
    }
    if (!on)
        return;
    if (s_draw_eff < 0.0f || s_draw_eff > s_draw_mult)
        s_draw_eff = s_draw_mult;
    {
        /* Relative to the frame rate the launcher asks for (RECOMP_FPS; the game's own 30 without it): down when
         * a frame takes 20% longer than that, up when it is within 4.5% of it (a limiter that is waiting). */
        static double target = -1.0;
        if (target < 0.0) {
            const char *f = getenv("RECOMP_FPS"), *s = getenv("RECOMP_60FPS");
            target = 1000.0 / ((f && atoi(f) > 60) ? atoi(f) : (s && *s && *s != '0') ? 60 : 30);
        }
        if (ema_ms > target * 1.2)
            s_draw_eff -= 0.08f;
        else if (ema_ms < target * 1.045)
            s_draw_eff += 0.02f;
    }
    if (s_draw_eff < lo_mult)
        s_draw_eff = lo_mult;
    if (s_draw_eff > s_draw_mult)
        s_draw_eff = s_draw_mult;
    s_draw_inv = 1.0f / (s_draw_eff * s_draw_eff);
}

/* RECOMP_CULL_STATS=1: every ~5 s, how many objects passed the draw distance test, how many of those only
 * because of the multiplier (visible at x m but not at x1), by the object's own max distance (obj+0xA0). The
 * arguments are the values the game compares: dist^2 * scale (scale already divided by m^2) and maxdist^2. */
void game_cull_note(float lhs, float a0sq)
{
    static int on = -1;
    static uint32_t total, vis_m, vis_1, last_frame, ext[8], extall[8];
    static const float edge[7] = { 10, 25, 50, 100, 200, 500, 1000 };
    int k;
    if (on < 0) {
        const char *e = getenv("RECOMP_CULL_STATS");
        on = e && *e == '1';
    }
    if (!on)
        return;
    total++;
    if (lhs < a0sq) {
        float a0 = sqrtf(a0sq > 0.0f ? a0sq : 0.0f);
        vis_m++;
        for (k = 0; k < 7 && a0 >= edge[k]; k++) ;
        extall[k]++;
        if (lhs * s_draw_mult * s_draw_mult >= a0sq)   /* hidden at x1 */
            ext[k]++;
        else
            vis_1++;
    }
    if (g_game_frames - last_frame >= 600) {
        fprintf(stderr, "[CULL] x%.1f frames %u: tests %u visible %u (only because of the multiplier %u);"
                " extra by object max distance <10:%u <25:%u <50:%u <100:%u <200:%u <500:%u <1000:%u >=1000:%u"
                " | all visible by max distance: %u %u %u %u %u %u %u %u\n",
                s_draw_mult, g_game_frames - last_frame, total, vis_m, vis_m - vis_1,
                ext[0], ext[1], ext[2], ext[3], ext[4], ext[5], ext[6], ext[7],
                extall[0], extall[1], extall[2], extall[3], extall[4], extall[5], extall[6], extall[7]);
        total = vis_m = vis_1 = 0;
        memset(ext, 0, sizeof ext);
        memset(extall, 0, sizeof extall);
        last_frame = g_game_frames;
    }
}

/* The draw-distance test's right-hand side (the object's max distance squared). RECOMP_SMALL_CAP=<x>: objects
 * whose own max distance is under RECOMP_SMALL_MAXDIST (default 100: the tiny props that make up nearly all of
 * the extra draws at high multipliers) are extended at most x times, however high the multiplier is; bigger
 * objects keep the full multiplier. Off (0) by default. */
static float s_cap = -1.0f, s_cap_maxd2 = 10000.0f;
static int s_cap_on;

static void cap_init(void)
{
    const char *e = getenv("RECOMP_SMALL_CAP"), *m = getenv("RECOMP_SMALL_MAXDIST");
    s_cap = e ? (float)atof(e) : 0.0f;
    if (m && atof(m) > 0.0)
        s_cap_maxd2 = (float)(atof(m) * atof(m));
    s_cap_on = s_cap >= 1.0f;
    if (s_cap < 1.0f)
        s_cap = 2.5f;                       /* what F12 switches on without a launcher value */
    if (s_cap_on)
        fprintf(stderr, "[DRAW] objects with max distance under %.0f are extended at most x%.1f"
                " (F12 switches it)\n", sqrtf(s_cap_maxd2), s_cap);
}

/* F12 (toggle != 0) flips the cap; returns whether it is on. */
int game_cap_state(int toggle)
{
    if (s_cap < 0.0f)
        cap_init();
    if (toggle)
        s_cap_on = !s_cap_on;
    return s_cap_on;
}

float game_cull_adjust(float lhs, float a0sq)
{
    const float cap = s_cap, maxd2 = s_cap_maxd2;
    game_cull_note(lhs, a0sq);
    if (s_cap < 0.0f) {
        cap_init();
        return a0sq;
    }
    if (s_cap_on && game_draw_eff_now() > cap && a0sq < maxd2) {
        float r = cap / game_draw_eff_now();
        return a0sq * r * r;
    }
    return a0sq;
}

float game_cull_scale(float s)
{
    if (s_draw_inv < 0.0f)
        draw_poll();
    if (!(s < 1.0f))                              /* also catches NaN */
        s = 1.0f;
    return s * s_draw_inv;
}

/* Camera sphere-in-view test (this = camera). RECOMP_FOV_NOCULL=1 skips it (everything counts as visible);
 * RECOMP_FOV_LOG=1 counts the tests per cone fov. */
int sub_001C66F0_hook(uint32_t cam)
{
    static int nocull = -1;
    static uint32_t last_frame;
    static float bin_fov[8];
    static int bin_n[8], nbins;
    if (nocull < 0) {
        const char *e = getenv("RECOMP_FOV_NOCULL");
        nocull = e && *e == '1';
    }
    if (s_fov_on > 0) {
        float f = guest_f(cam + 0x130);
        int k;
        for (k = 0; k < nbins && fabsf(bin_fov[k] - f) > 0.5f; k++) ;
        if (k == nbins && nbins < 8)
            bin_fov[nbins++] = f;
        if (k < 8)
            bin_n[k]++;
        if (g_game_frames - last_frame >= 120) {
            int i;
            fprintf(stderr, "[CULLTEST] frame %u:", g_game_frames);
            for (i = 0; i < nbins; i++) {
                fprintf(stderr, "  cone %.1f x%d", bin_fov[i], bin_n[i]);
                bin_n[i] = 0;
            }
            fprintf(stderr, "\n");
            last_frame = g_game_frames;
        }
    }
    return nocull;
}

/* End of the projection setup (this = camera): the view cone the game culls with is in +0x160..+0x17C. */
void sub_001C68C0_leave(uint32_t cam)
{
    static int lines;
    {   /* RECOMP_FOV_CONE_MARGIN=<x>: widen the cone the game's visibility tests use (tan of each half angle * x) */
        static float margin = -1.0f;
        if (margin < 0.0f) {
            const char *e = getenv("RECOMP_FOV_CONE_MARGIN");
            margin = e ? (float)atof(e) : 1.0f;
            if (margin < 1.0f || margin > 20.0f) margin = 1.0f;
        }
        if (margin != 1.0f && s_last_ret == 0x0018E2D2u) {
            static const uint32_t offs[4] = { 0x160, 0x168, 0x170, 0x178 };
            int k;
            for (k = 0; k < 4; k++) {
                float t = guest_f(cam + offs[k]) * margin;
                guest_set_f(cam + offs[k], t);
                guest_set_f(cam + offs[k] + 4, 1.0f / sqrtf(1.0f + t * t));
            }
        }
    }
    {   /* RECOMP_FOV_RWATCH=cone: who reads the camera's view cone (+0x160..) */
        static int armed;
        const char *w = getenv("RECOMP_FOV_RWATCH");
        if (!armed && w && !strcmp(w, "cone") && s_last_ret == 0x0018E2D2u) {
            extern int xbox_WatchArmAt(uint32_t va, unsigned max_reports, int reads);
            armed = 1;
            fprintf(stderr, "[FOV] read watch on the cone %08X\n", cam + 0x168);
            xbox_WatchArmAt(cam + 0x168, 1500, 1);
        }
    }
    {   /* RECOMP_FOV_RWATCH=cam:<hex offset>: who reads that field of the camera (e.g. cam:13C = the stored far plane) */
        static int armed2;
        const char *w = getenv("RECOMP_FOV_RWATCH");
        if (!armed2 && w && !strncmp(w, "cam:", 4) && s_last_ret == 0x0018E2D2u) {
            extern int xbox_WatchArmAt(uint32_t va, unsigned max_reports, int reads);
            uint32_t off = (uint32_t)strtoul(w + 4, NULL, 16);
            armed2 = 1;
            fprintf(stderr, "[FOV] read watch on camera field %08X (cam %08X + %X)\n", cam + off, cam, off);
            xbox_WatchArmAt(cam + off, 1500, 1);
        }
    }
    if (s_fov_on > 0 && s_last_ret == 0x0018E2D2u && lines < 40) {
        fprintf(stderr, "[CONE] fov %.2f->%.2f  160: %.3f %.3f  168: %.3f %.3f  170: %.3f %.3f  178: %.3f %.3f  scale180: %.3f %.3f %.3f %.3f  130: %.3f %.3f %.3f %.3f\n",
                s_last_in, s_last_out,
                guest_f(cam + 0x160), guest_f(cam + 0x164), guest_f(cam + 0x168), guest_f(cam + 0x16C),
                guest_f(cam + 0x170), guest_f(cam + 0x174), guest_f(cam + 0x178), guest_f(cam + 0x17C),
                guest_f(cam + 0x180), guest_f(cam + 0x184), guest_f(cam + 0x188), guest_f(cam + 0x18C),
                guest_f(cam + 0x130), guest_f(cam + 0x134), guest_f(cam + 0x138), guest_f(cam + 0x13C));
        lines++;
    }
}

void sub_001C68C0_enter(void)
{
    float a1, a2, a3, a4;
    uint32_t w;
    fov_poll();
    w = GUEST32(g_esp + 4);  memcpy(&a1, &w, 4);
    w = GUEST32(g_esp + 8);  memcpy(&a2, &w, 4);
    w = GUEST32(g_esp + 12); memcpy(&a3, &w, 4);
    w = GUEST32(g_esp + 16); memcpy(&a4, &w, 4);
    s_last_ret = GUEST32(g_esp);
    s_last_in = s_last_out = a1;
    {   /* RECOMP_FOV_SCAN=1: after ~1500 projection calls, once */
        static int cnt, done;
        const char *w = getenv("RECOMP_FOV_SCAN");
        if (!done && w && *w == '1' && s_last_ret == 0x0018E2D2u && ++cnt == 1500) {
            done = 1;
            fov_scan(s_fovrole[0].to > 1.0f ? s_fovrole[0].to : a1, a2);
        }
    }
    if (s_fov_on > 0) {   /* every distinct kind of call (caller, fov, aspect, near, far), once */
        static struct { uint32_t ret; float a1, a2, a3, a4; } seen[128];
        static int nseen;
        uint32_t ret = GUEST32(g_esp);
        int k;
        for (k = 0; k < nseen; k++)
            if (seen[k].ret == ret && fabsf(seen[k].a1 - a1) < 0.01f && fabsf(seen[k].a2 - a2) < 0.001f &&
                fabsf(seen[k].a3 - a3) < 0.001f && fabsf(seen[k].a4 - a4) < 0.01f)
                break;
        if (k == nseen && nseen < 128) {
            seen[nseen].ret = ret; seen[nseen].a1 = a1; seen[nseen].a2 = a2;
            seen[nseen].a3 = a3; seen[nseen].a4 = a4;
            nseen++;
            fprintf(stderr, "[FOVCALL] ret %08X fov %.3f aspect %.4f near %.4f far %.1f\n", ret, a1, a2, a3, a4);
        }
    }
    /* the player's camera: from the view update (0x18E2D2), 4:3 or 16:9, far >= 100 */
    if (GUEST32(g_esp) == 0x0018E2D2u && a2 > 1.2f && a2 < 2.5f && a3 < 0.5f && a4 >= 100.0f &&
        a1 > 1.0f && a1 < 160.0f) {
        if (s_fov_mode == 0 && a1 < 120.0f) {
            /* old path: only the argument of this call is widened */
            a1 = fov_step(fov_next_role(), a1, fov_now_ms());
            s_last_out = a1;
            guest_set_f(g_esp + 4, a1);
        }
        if (s_far_mult != 1.0f)
            guest_set_f(g_esp + 16, a4 * s_far_mult);
    }
}

/* FlushEx(iface, rt, flags) */
void sub_002ECCBF_enter(void)
{
    uint32_t iface = GUEST32(g_esp + 4);

    if (!iface)
        return;
    /* As the thunk does: CDirectSoundStream = iface - 0x1C, its
     * CMcpxStream at +0x20. */
    s_flush_mcpx = GUEST32(iface - 0x1C + 0x20);
    s_flush_tick = GetTickCount();
    {
        static int shown;
        uint16_t st = s_flush_mcpx ? GUEST16(s_flush_mcpx + 0x12) : 0;
        if ((st & 3) == 3 && shown++ < 50)
            fprintf(stderr, "[DSOUND] FlushEx active stream 0x%08X flags %u state 0x%04X\n",
                    s_flush_mcpx, GUEST32(g_esp + 16), st);
    }
}

/* CMcpxStream::GetStatus */
void sub_002F15BD_enter(void)
{
    static int enabled = -1;
    uint32_t mcpx = g_ecx;
    uint16_t state;
    DWORD age;

    /* Opt-in (RECOMP_DS_FORCE_STOP=1): forcing the state lets the title
     * release a stream the APU still has on its voice list, and DirectSound's
     * service routine then walks freed memory. */
    if (enabled < 0) {
        const char *e = getenv("RECOMP_DS_FORCE_STOP");
        enabled = e && *e == '1';
    }
    if (!mcpx || mcpx != s_flush_mcpx)
        return;
    if (!enabled) {
        /* Diagnostic: a flushed stream still active 2 s on -- dump it once. */
        static uint32_t dumped;
        if (dumped != mcpx && GetTickCount() - s_flush_tick > 2000
                && (GUEST16(mcpx + 0x12) & 3) == 3) {
            int k;
            dumped = mcpx;
            fprintf(stderr, "[DSOUND] stream 0x%08X still active 2 s after FlushEx:", mcpx);
            for (k = 0; k < 0x90; k += 4) {
                if (!(k & 0x1F))
                    fprintf(stderr, "\n  +%02X:", k);
                fprintf(stderr, " %08X", GUEST32(mcpx + k));
            }
            fprintf(stderr, "\n");
            /* Each voice's four notifier slots (FENADDR 0x590000, base
             * offset 2, four per voice), status bytes 14/15, as the
             * contiguous window (0x8...) and as RAM hold them. */
            {
                uint16_t v[3];
                int a, n;
                v[0] = (uint16_t)GUEST32(mcpx + 0x0C);
                v[1] = (uint16_t)(GUEST32(mcpx + 0x0C) >> 16);
                v[2] = (uint16_t)GUEST32(mcpx + 0x10);
                for (a = 0; a < 3; a++) {
                    fprintf(stderr, "  voice %u notifiers win/ram:", v[a]);
                    for (n = 0; n < 4; n++) {
                        uint32_t p = 0x590000u + 16u * (2u + v[a] * 4u + (uint32_t)n);
                        fprintf(stderr, " [%d] %04X/%04X", n,
                                GUEST16(0x80000000u + p + 14), GUEST16(p + 14));
                    }
                    fprintf(stderr, "\n");
                }
            }
        }
        return;
    }
    age = GetTickCount() - s_flush_tick;
    if (age > DS_STOP_WINDOW_MS) {
        s_flush_mcpx = 0;
        return;
    }
    state = GUEST16(mcpx + 0x12);
    if ((state & 3) != 3 && (state & 0x8001) != 0x8001) {
        s_flush_mcpx = 0;                  /* stopped on its own */
        return;
    }
    if (age > DS_STOP_GRACE_MS) {
        GUEST16(mcpx + 0x12) = (uint16_t)(state & ~0x8003u);
        s_flush_mcpx = 0;
        fprintf(stderr, "[DSOUND] stream 0x%08X (state 0x%04X) stop never acknowledged "
                        "by the APU; marking it stopped\n", mcpx, state);
    }
}

/* ── AC'97 channel reset ───────────────────────────────────── */

/* sub_002F3C24 resets an AC'97 bus-master channel: it writes RR (2) to the
 * channel's control byte at 0xFEC0010B + offset, reads it back once and spins
 * while RR is set. The runtime's write trap clears RR as the write lands, but
 * only while the page is still protected; make sure it is before the write. */
void xbox_Ac97EnsureTrap(void);

void sub_002F3C24_enter(void)
{
    xbox_Ac97EnsureTrap();
}

/* ── DirectSound GetStatus poll ────────────────────────────── */

/* sub_00196467 polls IDirectSoundStream_GetStatus (this thunk) in a tight loop
 * until a flushed stream stops. The stop is DirectSound's DPC processing the
 * APU's voice-done notifications, and that DPC needs the DirectSound lock the
 * poll takes and drops millions of times a second; host critical sections are
 * not fair, so the DPC never got it and the campaign never loaded. Before the
 * lock is taken: once the same stream has been polled many times in a row,
 * give the CPU away for a millisecond. Ordinary play polls several streams in
 * turn and never builds a streak. */
void sub_002ECD1F_enter(void)
{
    static __declspec(thread) uint32_t last_iface;
    static __declspec(thread) unsigned streak;
    uint32_t iface = GUEST32(g_esp + 4);

    if (iface != last_iface) {
        last_iface = iface;
        streak = 0;
        return;
    }
    if (++streak > 64)
        Sleep(1);
}

/* ── Hand-edited functions (game_patches.c) ────────────────── */

/* Defined here so the recompiler skips the original; the body lives in
 * game_patches.c, which includes the generated headers. */
void game_patch_00196467(void);

void sub_00196467(void)
{
    game_patch_00196467();
}

/* The frame limiter, for RECOMP_FPS (game_patches.c). */
void game_patch_000137C0(void);

void sub_000137C0(void)
{
    game_patch_000137C0();
}

/* The per-frame millisecond timer, for frame times under a frame. */
void game_patch_0017B210(void);

void sub_0017B210(void)
{
    game_patch_0017B210();
}

/* The sound system's per-frame update, 60 Hz under RECOMP_FPS. */
void game_patch_001AD630(void);

void sub_001AD630(void)
{
    game_patch_001AD630();
}

/* The look update's yaw and pitch steps, with native mouse aim. */
void game_patch_001149E0(void);
void game_patch_00114B60(void);

void sub_001149E0(void)
{
    game_patch_001149E0();
}

void sub_00114B60(void)
{
    game_patch_00114B60();
}

/* The character's move update, with the sprint. */
void game_patch_0011AC60(void);

void sub_0011AC60(void)
{
    game_patch_0011AC60();
}

/* The A/B experiment's line (scratchpad abconv.py): one lifted function as
 * generated (old) against the same with its registers in locals (new),
 * calls alternating, mean cycles per call each. */
void recomp_ab_report(const char *name, uint64_t c0, uint64_t n0, uint64_t c1, uint64_t n1)
{
    double a = n0 ? (double)c0 / n0 : 0, b = n1 ? (double)c1 / n1 : 0;
    fprintf(stderr, "[AB] %s: old %.0f cycles/call (%llu), new %.0f (%llu), new/old %.3f\n",
            name, a, (unsigned long long)n0, b, (unsigned long long)n1, a ? b / a : 0.0);
}

/* RECOMP_RNG_LOG=1: which code calls the random functions while the fire trigger is held.
 * ids: 1 = CRT rand (sub_002BE66C), 2 = game int rand (sub_002404E0), 3 = game float rand
 * (sub_00240540), 4 = game rand fill (sub_00240580). Every 5 s the table is printed, best "fire only"
 * callers first, then cleared. */
extern volatile int g_pad_rt_down;

#define RNG_TAB 256
static struct { uint32_t id, k[3]; unsigned fire, idle; } s_rng[RNG_TAB];
static int s_rng_n;
static uint64_t s_rng_t0;
static int s_rng_on = -1;

void game_rng_enter(uint32_t id)
{
    uint32_t k[3] = { 0, 0, 0 }, sp, i, nk = 0;
    uint64_t now;
    int j, fire;

    if (s_rng_on < 0) {
        const char *e = getenv("RECOMP_RNG_LOG");
        s_rng_on = e && *e == '1';
    }
    if (!s_rng_on)
        return;
    sp = (uint32_t)g_esp;
    for (i = 0; i < 64 && nk < 3; i++) {
        uint32_t v = *(volatile uint32_t *)((uintptr_t)(sp + i * 4u) + g_xbox_mem_offset);
        if (v >= 0x00011000u && v < 0x002D8000u)
            k[nk++] = v;
    }
    fire = g_pad_rt_down;
    for (j = 0; j < s_rng_n; j++)
        if (s_rng[j].id == id && !memcmp(s_rng[j].k, k, sizeof k))
            break;
    if (j == s_rng_n) {
        if (s_rng_n >= RNG_TAB)
            return;
        s_rng[j].id = id;
        memcpy(s_rng[j].k, k, sizeof k);
        s_rng_n++;
    }
    if (fire)
        s_rng[j].fire++;
    else
        s_rng[j].idle++;
    now = xbox_HostNowNs();
    if (!s_rng_t0)
        s_rng_t0 = now;
    else if (now - s_rng_t0 >= 5000000000ull) {
        int a, shown = 0;
        s_rng_t0 = now;
        fprintf(stderr, "[RNG] --- 5 s window, %d callers (fire = trigger held) ---\n", s_rng_n);
        for (a = 0; a < s_rng_n && shown < 24; a++) {
            int best = -1, b;
            for (b = 0; b < s_rng_n; b++) {
                if (s_rng[b].id == 0xFFFFFFFFu)
                    continue;
                if (best < 0 || (long)s_rng[b].fire - (long)s_rng[b].idle > (long)s_rng[best].fire - (long)s_rng[best].idle)
                    best = b;
            }
            if (best < 0)
                break;
            fprintf(stderr, "[RNG] id%u %06X < %06X < %06X   fire %u  idle %u\n", s_rng[best].id,
                    s_rng[best].k[0], s_rng[best].k[1], s_rng[best].k[2], s_rng[best].fire, s_rng[best].idle);
            s_rng[best].id = 0xFFFFFFFFu;
            shown++;
        }
        s_rng_n = 0;
    }
}

/* RECOMP_WDEF_LOG=1: the weapon definition loader (sub_000F6B40: one call per data field of one weapon, this+0x20C =
 * the entries' base, this+0x214 = the entry being filled, 0x164 bytes each; the field is the third argument and its
 * name CRC32 is MEM32(MEM32(field))). The previous call's changes to the entry are printed at the next call:
 * "[WDEF] w<n> h<name crc> +<offset> = <hex> (<float>)".
 *
 * RECOMP_WDEF_KEYS=1: numpad 1..6 zero a group of the loaded weapon entries' fields live (pressed again: the
 * original values come back), to find which fields are the spread / recoil without a rebuild per guess. */
#define WD_MAX 48
#define WD_WORDS (0x164 / 4)
static struct { uint32_t va; uint32_t orig[WD_WORDS]; int have; uint32_t sig;
                uint32_t dmg_va; float dmg_orig; int dmg_have;
                char name[32]; int name_have;                      /* 0 untried, 1 tried+failed, 2 have it */
                uint32_t wmod_ballist_va; int wmod_ballist_tried; } s_wd[WD_MAX];
/* +0x114 is NOT in the signature: its low 16 bits are magazine size, which a live ammo test (below)
 * writes on purpose -- including it here would make wd_apply() see a "changed" entry and drop it,
 * silently breaking Super Accuracy for that weapon. */
#define WD_SIG(p) ((p)[0x0E0 / 4] ^ ((p)[0x128 / 4] * 5u) ^ ((p)[0x02C / 4] * 7u))
static int s_wd_n;
static uint64_t s_wd_last_ns;
static unsigned s_wd_mask;                      /* groups currently zeroed */
static const struct { const char *name; int n; uint32_t off[4]; } k_wd_grp[6] = {
    { "1: +134 +138", 2, { 0x134, 0x138, 0, 0 } },
    { "2: +038 +040 +044 +04C", 4, { 0x038, 0x040, 0x044, 0x04C } },
    { "3: +050 +05C", 2, { 0x050, 0x05C, 0, 0 } },
    { "4: +130", 1, { 0x130, 0, 0, 0 } },
    { "5: +13C(CameraKick) +140", 2, { 0x13C, 0x140, 0, 0 } },
    { "6: +0C8", 1, { 0x0C8, 0, 0, 0 } },
};

static const int k_wd_key[6] = { VK_INSERT, VK_DELETE, VK_HOME, VK_END, VK_PRIOR, VK_NEXT };

static void wd_apply(int force_all)
{
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    int e, g, k;
    (void)force_all;
    for (e = 0; e < s_wd_n; e++) {
        uint32_t *p;
        if (!s_wd[e].have)
            continue;
        p = (uint32_t *)(mem + s_wd[e].va);
        if (WD_SIG(p) != s_wd[e].sig) {            /* no longer the definition we captured: leave it alone */
            s_wd[e].have = 0;
            continue;
        }
        for (g = 0; g < 6; g++)
            for (k = 0; k < k_wd_grp[g].n; k++) {
                uint32_t o = k_wd_grp[g].off[k] / 4;
                p[o] = (s_wd_mask >> g & 1) ? 0u : s_wd[e].orig[o];
            }
    }
}

/* RECOMP_WDEF_KEYS=1, key 'O': a live test of the three fields the value table suggests are magazine
 * size (+0x114 low 16 bits), maximum carried ammo (+0x154 low 16 bits) and starting ammo (+0x01C, a
 * full word) -- see [[moh-weapon-field-map]]. Each write is masked to its own sub-word so it does not
 * disturb FireRate (+0x114 high 16), CanMelee/SoundLoop (+0x154 high 16). The definition is a shared
 * template: the effect is expected at the next reload or the next time this weapon is picked up, not
 * on the one already in the player's hands. */
static int s_wd_ammo_test;

static void wd_apply_ammo(void)
{
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    int e;
    for (e = 0; e < s_wd_n; e++) {
        uint32_t *p;
        if (!s_wd[e].have)
            continue;
        p = (uint32_t *)(mem + s_wd[e].va);
        if (s_wd_ammo_test) {
            p[0x114 / 4] = (p[0x114 / 4] & 0xFFFF0000u) | 99u;          /* magazine size -> 99 */
            p[0x154 / 4] = (p[0x154 / 4] & 0xFFFF0000u) | 999u;         /* max carried ammo -> 999 */
            p[0x01C / 4] = s_wd[e].orig[0x01C / 4] * 5u;                /* starting ammo x5 */
        } else {
            p[0x114 / 4] = (p[0x114 / 4] & 0xFFFF0000u) | (s_wd[e].orig[0x114 / 4] & 0xFFFFu);
            p[0x154 / 4] = (p[0x154 / 4] & 0xFFFF0000u) | (s_wd[e].orig[0x154 / 4] & 0xFFFFu);
            p[0x01C / 4] = s_wd[e].orig[0x01C / 4];
        }
    }
}

/* RECOMP_WDEF_KEYS=1, key 'P': a live test of the candidate Damage field found 2026-10-31 night in the
 * separate ~28-byte ballistics-table slot that +0x124 keys into (see [[moh-weapon-field-map]], the
 * [WDEF-SCAN]/[WDEF-DUMP] lines) -- NOT inside the weapon entry itself, so it needs its own address
 * (s_wd[e].dmg_va, set the first time wd_scan_for_value sees a real lead for that weapon). Sets it to
 * 500.0 (huge, so any effect is obvious) or restores the captured original. */
static int s_wd_dmg_test;
static int wd_guest_readable(uint32_t va, uint32_t n);        /* defined below, needed here first */

static void wd_apply_damage(void)
{
    uint8_t *mem = (uint8_t *)(uintptr_t)g_xbox_mem_offset;
    int e;
    for (e = 0; e < s_wd_n; e++) {
        float v;
        if (!s_wd[e].dmg_have || !wd_guest_readable(s_wd[e].dmg_va, 4))
            continue;
        v = s_wd_dmg_test ? 500.0f : s_wd[e].dmg_orig;
        memcpy(mem + s_wd[e].dmg_va, &v, 4);
    }
}

/* RECOMP_WDEF_LOG=1 (same env var as above): once the weapon entries settle, follow two fields that
 * look like pointers/references rather than plain numbers, not to change them, only to read them:
 * +0E0 (a candidate weapon NAME string -- a future mods\weapons\<name>.txt needs a real, stable key,
 * not the unstable w<n> load order) and +124 (what the "ProjectileBullet" field points to or hashes;
 * looking for a separate projectile/bullet definition, which is the likely home of Damage and Range --
 * neither is a plain number anywhere in this 0x164-byte entry). Every read is checked with VirtualQuery
 * first (committed, readable, not a guard page); a wrong guess here must never crash the game. */
/* The two guest regions that hold real game data (same ranges fov_scan uses): low memory and the main
 * heap. A host VirtualQuery alone is not enough -- some "hash" values happen to also be a committed
 * HOST address by pure coincidence, which is not evidence of a guest pointer (it produced two false
 * "valid pointer, all zero" hits the first time this ran). Restricting to these ranges first removes
 * that false-positive class entirely. */
static int wd_guest_readable(uint32_t va, uint32_t n)
{
    static const uint32_t regions[][2] = { {0x00000000u, 0x00800000u}, {0x80000000u, 0x84000000u} };
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t host;
    int r, in_region = 0;
    if (va < 0x10000)                        /* small numbers are never real pointers here; skip the query */
        return 0;
    for (r = 0; r < 2; r++)
        if (va >= regions[r][0] && n <= regions[r][1] - va) {
            in_region = 1;
            break;
        }
    if (!in_region)
        return 0;
    host = (uintptr_t)va + g_xbox_mem_offset;
    if (!VirtualQuery((void *)host, &mbi, sizeof mbi))
        return 0;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || mbi.Protect == 0)
        return 0;
    return (uint32_t)((uintptr_t)mbi.BaseAddress + mbi.RegionSize - host) >= n;
}

/* Reads the weapon's own name string (the +0x0E0 pointer) -- used by the weapon mods file reader below
 * to match a mods\weapons\<Name>.txt to the right entry. Same safety rule as everything else here:
 * every byte is checked with wd_guest_readable first. */
static int wd_read_name(uint32_t name_ptr, char *out, size_t outsz)
{
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    int nlen = 0;
    if (!wd_guest_readable(name_ptr, 1))
        return 0;
    for (; nlen < (int)outsz - 1; nlen++) {
        uint8_t c;
        if (!wd_guest_readable(name_ptr + (uint32_t)nlen, 1))
            return 0;
        c = mem[name_ptr + (uint32_t)nlen];
        if (!c)
            break;
        if (c < 0x20 || c > 0x7E)
            return 0;
        out[nlen] = (char)c;
    }
    out[nlen] = 0;
    return nlen > 0;
}

/* RECOMP_WDEF_LOG=1: +124 failed the pointer test for every weapon once wd_guest_readable was fixed to
 * the real guest ranges -- it is a hash/id, not an address. This searches both guest ranges for that
 * exact 32-bit value (read-only, never writes), on the theory that it is the CRC32 key of a separate
 * projectile/bullet definition kept in some OTHER table -- finding that table's entries is how Damage
 * and Range (absent from the weapon entry itself) might be reached. At most 8 hits are logged per
 * weapon so one coincidence-heavy value cannot flood the log. */
/* A hit inside the weapon entry that owns `target` itself is expected (that is just +0x124 of that
 * same entry, found again) and not interesting. Anything else is a real lead. */
static int wd_is_home(uint32_t addr)
{
    int k;
    for (k = 0; k < s_wd_n; k++)
        if (s_wd[k].have && addr >= s_wd[k].va && addr < s_wd[k].va + 0x164u)
            return 1;
    return 0;
}

static void wd_scan_for_value(uint32_t target, int wi)
{
    static const uint32_t regions[][2] = { {0x00000000u, 0x00800000u}, {0x80000000u, 0x84000000u} };
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    int r, hits = 0, shown = 0;
    for (r = 0; r < 2 && hits < 8; r++) {
        uint32_t va = regions[r][0];
        while (va < regions[r][1] && hits < 8) {
            MEMORY_BASIC_INFORMATION mbi;
            uintptr_t host = (uintptr_t)va + g_xbox_mem_offset;
            uint32_t n, off, blockEnd;
            if (!VirtualQuery((void *)host, &mbi, sizeof mbi))
                break;
            blockEnd = (uint32_t)((uintptr_t)mbi.BaseAddress + mbi.RegionSize - g_xbox_mem_offset);
            n = blockEnd > va ? blockEnd - va : 0;
            if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && mbi.Protect != 0) {
                for (off = 0; off + 4 <= n && va + off < regions[r][1] && hits < 8; off += 4) {
                    uint32_t v, hit = va + off;
                    memcpy(&v, mem + hit, 4);
                    if (v != target)
                        continue;
                    hits++;
                    if (wd_is_home(hit)) {
                        fprintf(stderr, "[WDEF-SCAN] w%d target=%08X found at %08X (its own entry, expected)\n",
                                wi, target, hit);
                        continue;
                    }
                    fprintf(stderr, "[WDEF-SCAN] w%d target=%08X found at %08X -- NOT inside any captured "
                            "weapon entry (a real lead)\n", wi, target, hit);
                    /* The FIRST real lead per weapon: remember +8 of this slot (a candidate Damage field,
                     * see [[moh-weapon-field-map]] -- across 5 weapons it read 24,25,26,24,50, MG42
                     * clearly highest) for a live test key, same idea as the magazine-size test above. */
                    if (!s_wd[wi].dmg_have && wd_guest_readable(hit, 12)) {
                        s_wd[wi].dmg_va = hit + 8;
                        memcpy(&s_wd[wi].dmg_orig, mem + hit + 8, 4);
                        s_wd[wi].dmg_have = 1;
                    }
                    if (shown++ < 3 && hit >= 32u && wd_guest_readable(hit - 32u, 64u)) {
                        /* dump a small window around it: 8 dwords before, the hit, 7 dwords after,
                         * as both hex and float, to look for weapon-shaped numbers (e.g. the Kar98k
                         * reading higher than the SMGs would be a strong sign of Damage or Range). */
                        int d;
                        fprintf(stderr, "[WDEF-DUMP] around %08X:\n", hit);
                        for (d = -8; d <= 7; d++) {
                            uint32_t a = hit + (uint32_t)(d * 4);
                            uint32_t w;
                            float f;
                            memcpy(&w, mem + a, 4);
                            memcpy(&f, mem + a, 4);
                            fprintf(stderr, "  %08X %+3d: %08X  %g%s\n", a, d * 4, w, (double)f,
                                    a == hit ? "   <-- hash" : "");
                        }
                    }
                }
            }
            if (!n)
                break;
            va += n;
        }
    }
    if (!hits)
        fprintf(stderr, "[WDEF-SCAN] w%d target=%08X not found in guest memory (0x0-0x800000 / 0x80000000-0x84000000)\n", wi, target);
}

/* Same idea as wd_scan_for_value, quiet (no logging, no dump) and stops at the first external hit --
 * used by the weapon mods file reader below to locate a weapon's ballistics-table slot ONLY when a
 * mods file actually asks for Damage/Range/Falloff (so a player not using those pays nothing). */
static uint32_t wd_find_ballist_base(uint32_t target)
{
    static const uint32_t regions[][2] = { {0x00000000u, 0x00800000u}, {0x80000000u, 0x84000000u} };
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    int r;
    for (r = 0; r < 2; r++) {
        uint32_t va = regions[r][0];
        while (va < regions[r][1]) {
            MEMORY_BASIC_INFORMATION mbi;
            uintptr_t host = (uintptr_t)va + g_xbox_mem_offset;
            uint32_t n, off, blockEnd;
            if (!VirtualQuery((void *)host, &mbi, sizeof mbi))
                break;
            blockEnd = (uint32_t)((uintptr_t)mbi.BaseAddress + mbi.RegionSize - g_xbox_mem_offset);
            n = blockEnd > va ? blockEnd - va : 0;
            if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && mbi.Protect != 0) {
                for (off = 0; off + 4 <= n && va + off < regions[r][1]; off += 4) {
                    uint32_t v;
                    memcpy(&v, mem + va + off, 4);
                    if (v == target && !wd_is_home(va + off))
                        return va + off;
                }
            }
            if (!n)
                break;
            va += n;
        }
    }
    return 0;
}

/* ============================================================
 * Weapon mods (mods\weapons\<WeaponName>.txt, RECOMP_MODS / RECOMP_MODS_DIR)
 *
 * key=value lines naming the fields [[moh-weapon-field-map]] found 2026-10-31:
 *   MagazineSize, MaxAmmo, StartingAmmo  -- MagazineSize is LIVE-CONFIRMED (key 'O' test); MaxAmmo and
 *                                           StartingAmmo are strong value-based guesses, not yet tested.
 *   FireRate                             -- name CONFIRMED by CRC32; which direction is faster is NOT.
 *   Spread, Recoil                       -- CONFIRMED (the same groups Super Accuracy already uses).
 *   MuzzleSpeed, ZoomFOV, ZoomSensitivity -- name CONFIRMED by CRC32, in-game feel not separately tested.
 *   Damage, Range, Falloff               -- EXPERIMENTAL: a separate ~28-byte table reached through the
 *                                           +0x124 field; candidate meaning only (see the field-map doc),
 *                                           found by a one-time guest-memory scan the first time a mods
 *                                           file for that weapon asks for one of these three.
 * The file's name is the weapon's own name (the +0x0E0 string, e.g. "Karabiner.txt"), not the unstable
 * load-order index. Same pack rule as every other mod type: mods\weapons\<Name>.txt first, then every
 * mods\<pack>\weapons\<Name>.txt by name, a later one overriding an earlier one.
 * ============================================================ */

typedef struct {
    char name[32];
    int has_mag, has_maxammo, has_startammo, has_firerate, has_spread, has_recoil,
        has_muzzlespeed, has_zoomfov, has_zoomsens, has_damage, has_range, has_falloff;
    uint32_t mag, maxammo, startammo, firerate;
    float spread, recoil, muzzlespeed, zoomfov, zoomsens, damage, range, falloff;
} WMod;
static WMod *s_wm;
static int s_wm_n, s_wm_cap, s_wm_init;

static WMod *wmod_slot(const char *name)
{
    int i;
    WMod *p;
    for (i = 0; i < s_wm_n; i++)
        if (!_stricmp(s_wm[i].name, name))
            return &s_wm[i];
    if (s_wm_n == s_wm_cap) {
        p = (WMod *)realloc(s_wm, (size_t)(s_wm_cap ? s_wm_cap * 2 : 16) * sizeof *p);
        if (!p)
            return NULL;
        s_wm = p;
        s_wm_cap = s_wm_cap ? s_wm_cap * 2 : 16;
    }
    p = &s_wm[s_wm_n++];
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", name);
    return p;
}

static void wmod_parse_file(const char *path, const char *name)
{
    FILE *f = fopen(path, "r");
    char line[192];
    WMod *w;
    if (!f)
        return;
    w = wmod_slot(name);
    if (!w) {
        fclose(f);
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *eq, *k, *v, *tail;
        line[strcspn(line, "\r\n")] = 0;
        tail = strchr(line, '#');
        if (tail)
            *tail = 0;
        k = line;
        while (*k == ' ' || *k == '\t')
            k++;
        if (!*k)
            continue;
        eq = strchr(k, '=');
        if (!eq)
            continue;
        *eq = 0;
        v = eq + 1;
        tail = k + strlen(k);
        while (tail > k && (tail[-1] == ' ' || tail[-1] == '\t'))
            *--tail = 0;
        while (*v == ' ' || *v == '\t')
            v++;
        tail = v + strlen(v);
        while (tail > v && (tail[-1] == ' ' || tail[-1] == '\t'))
            *--tail = 0;
        if (!*k || !*v)
            continue;
        if (!_stricmp(k, "MagazineSize"))       { w->mag = (uint32_t)atoi(v); w->has_mag = 1; }
        else if (!_stricmp(k, "MaxAmmo"))        { w->maxammo = (uint32_t)atoi(v); w->has_maxammo = 1; }
        else if (!_stricmp(k, "StartingAmmo"))   { w->startammo = (uint32_t)atoi(v); w->has_startammo = 1; }
        else if (!_stricmp(k, "FireRate"))       { w->firerate = (uint32_t)atoi(v); w->has_firerate = 1; }
        else if (!_stricmp(k, "Spread"))         { w->spread = (float)atof(v); w->has_spread = 1; }
        else if (!_stricmp(k, "Recoil"))         { w->recoil = (float)atof(v); w->has_recoil = 1; }
        else if (!_stricmp(k, "MuzzleSpeed"))    { w->muzzlespeed = (float)atof(v); w->has_muzzlespeed = 1; }
        else if (!_stricmp(k, "ZoomFOV"))        { w->zoomfov = (float)atof(v); w->has_zoomfov = 1; }
        else if (!_stricmp(k, "ZoomSensitivity")) { w->zoomsens = (float)atof(v); w->has_zoomsens = 1; }
        else if (!_stricmp(k, "Damage"))         { w->damage = (float)atof(v); w->has_damage = 1; }
        else if (!_stricmp(k, "Range"))          { w->range = (float)atof(v); w->has_range = 1; }
        else if (!_stricmp(k, "Falloff"))        { w->falloff = (float)atof(v); w->has_falloff = 1; }
        else
            fprintf(stderr, "[WMOD] %s: unknown key \"%s\" (skipped)\n", name, k);
    }
    fclose(f);
}

static void wmod_scan_dir(const char *dir)
{
    WIN32_FIND_DATAA fd;
    char pat[MAX_PATH + 8];
    HANDLE h;
    snprintf(pat, sizeof pat, "%s\\*.txt", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        char name[32], path[MAX_PATH + 16];
        size_t L;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        L = strlen(fd.cFileName);
        if (L < 5 || L - 4 >= sizeof name)             /* need at least "x.txt", and the name must fit */
            continue;
        memcpy(name, fd.cFileName, L - 4);
        name[L - 4] = 0;
        if (!_strnicmp(name, "README", 6))              /* notes for players, not a weapon */
            continue;
        snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
        wmod_parse_file(path, name);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void wmod_init(void)
{
    const char *e = getenv("RECOMP_MODS"), *d = getenv("RECOMP_MODS_DIR");
    char root[MAX_PATH], sub[MAX_PATH + 24], pat[MAX_PATH + 8];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    s_wm_init = 1;
    if (e && e[0] == '0')
        return;
    snprintf(root, sizeof root, "%s", d && d[0] ? d : "mods");
    snprintf(sub, sizeof sub, "%s\\weapons", root);
    wmod_scan_dir(sub);                                  /* mods\weapons */
    snprintf(pat, sizeof pat, "%s\\*", root);            /* then every pack: mods\<pack>\weapons, by name */
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') {
                snprintf(sub, sizeof sub, "%s\\%s\\weapons", root, fd.cFileName);
                wmod_scan_dir(sub);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (s_wm_n) {
        fprintf(stderr, "  [WMOD] %d weapon mod file(s) found under %s\n", s_wm_n, root);
        fflush(stderr);
    }
}

static WMod *wmod_find(const char *name)
{
    int i;
    for (i = 0; i < s_wm_n; i++)
        if (!_stricmp(s_wm[i].name, name))
            return &s_wm[i];
    return NULL;
}

static void wmod_apply(void)
{
    uint8_t *mem = (uint8_t *)(uintptr_t)g_xbox_mem_offset;
    int e;

    if (!s_wm_init)
        wmod_init();
    if (!s_wm_n)
        return;
    for (e = 0; e < s_wd_n; e++) {
        uint32_t *p;
        WMod *w;
        if (!s_wd[e].have)
            continue;
        p = (uint32_t *)(mem + s_wd[e].va);
        if (WD_SIG(p) != s_wd[e].sig) {            /* no longer the entry we captured: leave it alone */
            s_wd[e].have = 0;
            continue;
        }
        if (!s_wd[e].name_have)
            s_wd[e].name_have = wd_read_name(s_wd[e].orig[0x0E0 / 4], s_wd[e].name, sizeof s_wd[e].name) ? 2 : 1;
        if (s_wd[e].name_have != 2)
            continue;
        w = wmod_find(s_wd[e].name);
        if (!w)
            continue;
        if (w->has_mag)        p[0x114 / 4] = (p[0x114 / 4] & 0xFFFF0000u) | (w->mag & 0xFFFFu);
        if (w->has_firerate)   p[0x114 / 4] = (p[0x114 / 4] & 0x0000FFFFu) | (w->firerate << 16);
        if (w->has_maxammo)    p[0x154 / 4] = (p[0x154 / 4] & 0xFFFF0000u) | (w->maxammo & 0xFFFFu);
        if (w->has_startammo)  p[0x01C / 4] = w->startammo;
        if (w->has_spread)   { memcpy(&p[0x134 / 4], &w->spread, 4); memcpy(&p[0x138 / 4], &w->spread, 4); }
        if (w->has_recoil)   { memcpy(&p[0x13C / 4], &w->recoil, 4); memcpy(&p[0x140 / 4], &w->recoil, 4); }
        if (w->has_muzzlespeed) memcpy(&p[0x120 / 4], &w->muzzlespeed, 4);
        if (w->has_zoomfov)     memcpy(&p[0x0BC / 4], &w->zoomfov, 4);
        if (w->has_zoomsens)    memcpy(&p[0x0C0 / 4], &w->zoomsens, 4);
        if (w->has_damage || w->has_range || w->has_falloff) {
            if (!s_wd[e].wmod_ballist_tried) {
                s_wd[e].wmod_ballist_tried = 1;
                s_wd[e].wmod_ballist_va = wd_find_ballist_base(s_wd[e].orig[0x124 / 4]);
                if (!s_wd[e].wmod_ballist_va)
                    fprintf(stderr, "[WMOD] %s: Damage/Range/Falloff requested but the ballistics table "
                            "was not found this run (not applied, nothing else is affected)\n", s_wd[e].name);
            }
            if (s_wd[e].wmod_ballist_va && wd_guest_readable(s_wd[e].wmod_ballist_va, 24)) {
                if (w->has_damage)  memcpy(mem + s_wd[e].wmod_ballist_va + 8,  &w->damage,  4);
                if (w->has_falloff) memcpy(mem + s_wd[e].wmod_ballist_va + 12, &w->falloff, 4);
                if (w->has_range)   memcpy(mem + s_wd[e].wmod_ballist_va + 20, &w->range,   4);
                {   /* read-back, once per weapon, so a mod test is never ambiguous */
                    static uint32_t rb_done[64];
                    if (e < 64 && !rb_done[e]) {
                        float d, fo, r;
                        rb_done[e] = 1;
                        memcpy(&d,  mem + s_wd[e].wmod_ballist_va + 8,  4);
                        memcpy(&fo, mem + s_wd[e].wmod_ballist_va + 12, 4);
                        memcpy(&r,  mem + s_wd[e].wmod_ballist_va + 20, 4);
                        fprintf(stderr, "[WMOD] %s: ballistics row %08X read back: damage %g falloff %g range %g\n",
                                s_wd[e].name, s_wd[e].wmod_ballist_va, (double)d, (double)fo, (double)r);
                    }
                }
            }
        }
    }
}

static void wd_deref_log(void)
{
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    int e;
    for (e = 0; e < s_wd_n; e++) {
        uint32_t name_ptr, ref124;
        char name[64];
        int nlen, i;

        if (!s_wd[e].have)
            continue;
        name_ptr = s_wd[e].orig[0x0E0 / 4];
        nlen = 0;
        if (wd_guest_readable(name_ptr, 1)) {
            for (; nlen < (int)sizeof name - 1; nlen++) {
                uint8_t c;
                if (!wd_guest_readable(name_ptr + (uint32_t)nlen, 1)) {
                    nlen = -1;
                    break;
                }
                c = mem[name_ptr + (uint32_t)nlen];
                if (!c)
                    break;
                if (c < 0x20 || c > 0x7E) {      /* not printable text: this guess is wrong, say so */
                    nlen = -1;
                    break;
                }
                name[nlen] = (char)c;
            }
        } else {
            nlen = -1;
        }
        if (nlen > 0) {
            name[nlen] = 0;
            fprintf(stderr, "[WDEF-NAME] w%d +0E0=%08X \"%s\"\n", e, name_ptr, name);
        } else {
            fprintf(stderr, "[WDEF-NAME] w%d +0E0=%08X (not a readable text string)\n", e, name_ptr);
        }

        ref124 = s_wd[e].orig[0x124 / 4];
        if (wd_guest_readable(ref124, 64)) {
            fprintf(stderr, "[WDEF-REF124] w%d ptr=%08X floats:", e, ref124);
            for (i = 0; i < 16; i++) {
                float f;
                memcpy(&f, mem + ref124 + (uint32_t)i * 4, 4);
                fprintf(stderr, " %g", (double)f);
            }
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, "[WDEF-REF124] w%d +124=%08X (not a valid pointer -- a hash/id, not an address)\n",
                    e, ref124);
            wd_scan_for_value(ref124, e);       /* does this value appear anywhere else, as a key into another table? */
        }
    }
}

void game_wdef_poll(void)
{
    static int keys = -1;
    static unsigned down;
    int g, e;
    uint64_t now;

    if (keys < 0) {
        const char *e2 = getenv("RECOMP_WDEF_KEYS");
        const char *e3 = getenv("RECOMP_NO_RECOIL");
        keys = (e2 && *e2 == '1') ? 1 : 0;          /* the hotkeys (Insert... / numpad) only when asked for with RECOMP_WDEF_KEYS=1: a player
                                                     * build must not toggle Super Accuracy groups from keys a player may have bound */
        if (e3 && *e3 == '1')
            s_wd_mask = 1u | 16u;          /* groups 1 (+134 +138) and 5 (CameraKick +13C +140) together:
                                            * recoil steady, spread zero, the aim ring closes to a small circle.
                                            * Group 5 alone leaves the spread random (user test 2026-10-29). */
    }
    now = xbox_HostNowNs();
    {   /* the zeroing is written again every second: the game may rewrite a definition (a reload, a pickup) */
        static uint64_t last_apply;
        if (now - last_apply > 1000000000ull) {
            last_apply = now;
            if (s_wd_mask || s_wd_ammo_test || s_wd_dmg_test)
                wd_apply(1);
            if (s_wd_ammo_test)
                wd_apply_ammo();
            if (s_wd_dmg_test)
                wd_apply_damage();
            wmod_apply();                   /* mods\weapons\<Name>.txt -- cheap no-op with no mod files */
        }
    }
    if (s_wd_last_ns && now - s_wd_last_ns > 2000000000ull) {   /* loading settled: remember the real values */
        int got = 0;
        for (e = 0; e < s_wd_n; e++)
            if (!s_wd[e].have) {
                memcpy(s_wd[e].orig, (const uint8_t *)(uintptr_t)g_xbox_mem_offset + s_wd[e].va, sizeof s_wd[e].orig);
                s_wd[e].sig = WD_SIG(s_wd[e].orig);
                s_wd[e].have = 1;
                got++;
            }
        if (got) {
            fprintf(stderr, "[WDEF] %d weapon entries ready (keys Insert Delete Home End PageUp PageDown = groups 1..6, or numpad 1..6)\n", s_wd_n);
            if (s_wd_mask)
                wd_apply(1);
            wd_deref_log();
        }
    }
    for (g = 0; keys && g < 6; g++) {
        int dn = ((GetAsyncKeyState(k_wd_key[g]) | GetAsyncKeyState(VK_NUMPAD1 + g)) & 0x8000) != 0;
        if (dn && !(down >> g & 1)) {
            s_wd_mask ^= 1u << g;
            fprintf(stderr, "[WDEF] group %s now %s\n", k_wd_grp[g].name, (s_wd_mask >> g & 1) ? "ZERO" : "original");
            wd_apply(0);
        }
        down = dn ? (down | 1u << g) : (down & ~(1u << g));
    }
    if (keys) {
        static unsigned odown;
        int dn = (GetAsyncKeyState('O') & 0x8000) != 0;
        if (dn && !odown) {
            s_wd_ammo_test = !s_wd_ammo_test;
            wd_apply_ammo();
            fprintf(stderr, "[WDEF] ammo test (magazine 99 / max ammo 999 / starting ammo x5) now %s "
                    "-- reload or pick up a fresh weapon to see it\n", s_wd_ammo_test ? "ON" : "off");
        }
        odown = dn;
    }
    if (keys) {
        static unsigned pdown;
        int dn = (GetAsyncKeyState('P') & 0x8000) != 0;
        if (dn && !pdown) {
            s_wd_dmg_test = !s_wd_dmg_test;
            wd_apply_damage();
            fprintf(stderr, "[WDEF] damage test (candidate Damage field -> 500.0) now %s "
                    "-- shoot an enemy and see if it dies in one hit; weapons with no [WDEF-SCAN] lead "
                    "yet are unaffected\n", s_wd_dmg_test ? "ON" : "off");
        }
        pdown = dn;
    }
}

/* Super Accuracy (zero spread + steady recoil = groups 1 and 5), for the settings menu. */
int game_super_accuracy_get(void)
{
    return (s_wd_mask & 17u) == 17u;
}

void game_super_accuracy_set(int on)
{
    s_wd_mask = on ? (s_wd_mask | 17u) : (s_wd_mask & ~17u);
    wd_apply(1);
    fprintf(stderr, "[WDEF] Super Accuracy %s (settings menu)\n", on ? "on" : "off");
}

void game_wdef_enter(void)
{
    static int on = -1;
    static uint32_t prev_va;
    static uint32_t prev_idx, prev_hash;
    static uint32_t prev[WD_WORDS];
    uint32_t hash, arg;
    uint32_t this_, base, idx, va, i;
    const uint8_t *mem;

    if (on < 0) {
        const char *e = getenv("RECOMP_WDEF_LOG");
        const char *k = getenv("RECOMP_WDEF_KEYS");
        const char *nr = getenv("RECOMP_NO_RECOIL");
        (void)k; (void)nr;
        on = (e && *e == '1') | (1 << 1);          /* the definitions are always collected (Super Accuracy can be switched live) */
    }
    if (!on)
        return;
    mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    this_ = g_ecx;
    arg = *(const uint32_t *)(mem + g_esp + 0xC);   /* the third argument (edi after the prologue: esp+0x20) */
    hash = *(const uint32_t *)(mem + *(const uint32_t *)(mem + arg));
    base = *(const uint32_t *)(mem + this_ + 0x20C);
    idx = *(const uint32_t *)(mem + this_ + 0x214);
    va = base + idx * 0x164u;
    if (on & 1 && prev_va) {
        const uint32_t *cur = (const uint32_t *)(mem + prev_va);
        for (i = 0; i < WD_WORDS; i++) {
            if (cur[i] != prev[i]) {
                float f;
                memcpy(&f, &cur[i], 4);
                fprintf(stderr, "[WDEF] w%u h%08X +%03X = %08X (%g)\n", prev_idx, prev_hash, i * 4, cur[i], (double)f);
            }
        }
    }
    prev_va = va;
    prev_idx = idx;
    prev_hash = hash;
    memcpy(prev, mem + va, sizeof prev);
    if (on & 2) {
        uint64_t now = xbox_HostNowNs();
        int e;
        if (s_wd_last_ns && now - s_wd_last_ns > 5000000000ull)     /* a new load: forget the old entries */
            s_wd_n = 0;
        s_wd_last_ns = now;
        for (e = 0; e < s_wd_n; e++)
            if (s_wd[e].va == va)
                break;
        if (e == s_wd_n && s_wd_n < WD_MAX) {
            s_wd[e].va = va;
            s_wd[e].have = 0;
            s_wd_n++;
        } else if (e < s_wd_n) {
            s_wd[e].have = 0;                                       /* being filled again: capture later */
        }
    }
}


/* ---- Blood spray (BLOOD-SPRAY) ------------------------------------------------------------------------------
 * The bullet-impact effects are particle type definitions (.xdd, "Pls_impactBodyBlood01" / "Pls_impactHeadBlood01",
 * embedded in every level_a.str). They are loaded by sub_000B97F0 (ProcParticleTypeHandler::ProcessField): called once
 * per data field, this+4 = the entries, this+0xC = the entry being filled, 0x184 bytes each; entry+0x100 = the game's
 * name hash (sub_00194DF0: CRC32 over the upper-cased text, table at 0x3248E8) of the definition's FileName.
 * Field names are CRC32 of PascalCase words (SizeMinWidth, EmitRate, AlphaStart ...); the setters give the layout:
 *   +0x70 EmitRate (int, >= 1)       +0xBC SizeEndScale (float)
 *   +0xC0 +0xC4 +0xC8 +0xCC          SizeMin/Max Width/Height (floats, metres)
 *   +0x2C +0x30 AlphaStart (ints 0..255)   +0x34 AlphaEnd
 * game_vfx_enter() (hook at the top of sub_000B97F0, recomp_0022.c) remembers every entry; once loading has settled
 * the blood entries are captured and scaled from the captured originals (level 0 = exactly as the game).
 * RECOMP_VFX_LOG=1 prints the whole captured entry.  RECOMP_BLOOD=<0..3>, RECOMP_BLOOD_COUNT / _SIZE / _ALPHA = custom
 * multipliers (override the level). */
#define VX_ENT 0x184
#define VX_WORDS (VX_ENT / 4)
#define VX_MAX 512
static struct { uint32_t va; uint32_t mgr; uint32_t tbl; int have; int blood; int weather; uint32_t hash; uint32_t orig[VX_WORDS]; } s_vx[VX_MAX];
/* The entry table may be reallocated while loading: an address is only used while the manager still points at the
 * table it was taken from. */
static int vx_alive(int e)
{
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    return s_vx[e].mgr && *(const uint32_t *)(mem + s_vx[e].mgr + 4) == s_vx[e].tbl;
}
static int s_vx_n;
static uint64_t s_vx_last_ns;
static int s_blood_level = -1;
/* Recomp Settings "Blood duration": the particle life (+0x74) in seconds for every blood entry; 0 = the level's own. */
#define BLOOD_DUR_N 9
static const float k_blood_dur[BLOOD_DUR_N] = { 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f, 10.0f };
static int s_bl_dur_idx = -1;
static void vx_dur_init(void);
/* One level = a set of multipliers. life = ParticleLife (+0x74, seconds; the pool size follows it), endsc = SizeEndScale,
 * fade = FadeOutStart (+0xF0, fraction of the life at which the fade begins: lower = slower fade), grav = acceleration
 * (+0xA0, along the particle space's Z axis; WorldAccel +0x170 is switched on so it pulls the drops down), 0 = leave. */
static struct { float cnt, size, alpha, endsc, life, fade, grav, vel; } s_bl = { 1, 1, 1, 1, 1, 1, 0, 1 };
static const struct { const char *name; float cnt, size, alpha, endsc, life, fade, grav, vel; } k_blood[4] = {
    { "Normal",  1.0f, 1.0f, 1.0f, 1.0f,  1.0f, 1.0f,   0.0f,  1.0f },
    { "More",    2.0f, 1.5f, 2.0f, 1.0f,  4.0f, 0.35f, -0.3f,  0.6f },
    { "Lots",    2.5f, 2.2f, 3.0f, 1.0f,  7.0f, 0.25f, -0.45f, 0.4f },
    { "Extreme", 3.0f, 3.0f, 4.5f, 1.0f, 10.0f, 0.15f, -0.6f,  0.3f },
};

static uint32_t vx_name_hash(const char *s)
{
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    uint32_t h = 0xFFFFFFFFu;
    for (; *s; s++) {
        uint32_t c = (uint8_t)*s;
        if (c >= 'a' && c <= 'z')
            c -= 0x20;
        h = (h >> 8) ^ *(const uint32_t *)(mem + 0x3248E8u + ((h ^ c) & 0xFFu) * 4u);
    }
    return ~h;
}

/* SCREEN-FX dirt: every particle emitter placement (emitter vtable +0xEC -> sub_001329A0 stores the 4x4 matrix at
 * emitter+0x70, translation at +0xA0) is checked against the particle definitions of explosions (explosion_defs.h,
 * generated from the level files). An explosion within RECOMP_SCREENFX_EXPLODE_DIST (default 14) of the camera
 * ([0x3E8D08]+0x90) throws dirt on the screen, the closer the stronger; one trigger per 1.5 s. */
#include "explosion_defs.h"
extern void recomp_screenfx_trigger(int kind, float strength);
static uint32_t s_ex_hash[2 * (sizeof k_explosion_defs / sizeof k_explosion_defs[0])];
static int s_ex_n = -1;

static int ex_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

void game_fx_placed(uint32_t emitter)
{
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    static float range = 14.0f;
    static DWORD last;
    static int logged;
    uint32_t def, hash, cam;
    float dx, dy, dz, d;
    DWORD now;
    if (s_ex_n < 0) {
        char path[200], *c;
        const char *e = getenv("RECOMP_SCREENFX_EXPLODE_DIST");
        size_t i;
        if (e && atof(e) > 0.0)
            range = (float)atof(e);
        s_ex_n = 0;
        for (i = 0; i < sizeof k_explosion_defs / sizeof k_explosion_defs[0]; i++) {
            snprintf(path, sizeof path, "file:\\z:\\moh4\\data\\shared\\vfx\\%s", k_explosion_defs[i]);
            s_ex_hash[s_ex_n++] = vx_name_hash(path);
            for (c = path; *c; c++)
                if (*c == '\\')
                    *c = '/';
            s_ex_hash[s_ex_n++] = vx_name_hash(path);
        }
        qsort(s_ex_hash, (size_t)s_ex_n, sizeof s_ex_hash[0], ex_cmp);
        fprintf(stderr, "[SCREENFX] %d explosion definition hashes, range %.1f\n", s_ex_n, (double)range);
    }
    /* sub_001329A0 is a vtable slot shared by more than one class: only the particle emitter (vtable 0x310D20) has the
     * definition pointer at +0x1E0, and only guest addresses inside the 64 MB RAM windows may be dereferenced. */
#define FXP_OK(p) (((p) >= 0x10000u && (p) < 0x00800000u) || ((p) >= 0x80010000u && (p) < 0x83FF0000u))
    if (!FXP_OK(emitter) || *(const uint32_t *)(mem + emitter) != 0x310D20u)
        return;
    def = *(const uint32_t *)(mem + emitter + 0x1E0);
    if (!FXP_OK(def))
        return;
    hash = *(const uint32_t *)(mem + def + 0x100);
    if (!bsearch(&hash, s_ex_hash, (size_t)s_ex_n, sizeof s_ex_hash[0], ex_cmp))
        return;
    now = GetTickCount();
    if (now - last < 1500)
        return;
    cam = *(const uint32_t *)(mem + 0x3E8D08u);
    if (!FXP_OK(cam))
        return;
    dx = *(const float *)(mem + emitter + 0xA0) - *(const float *)(mem + cam + 0x90);
    dy = *(const float *)(mem + emitter + 0xA4) - *(const float *)(mem + cam + 0x94);
    dz = *(const float *)(mem + emitter + 0xA8) - *(const float *)(mem + cam + 0x98);
    d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (logged < 30) {
        logged++;
        fprintf(stderr, "[SCREENFX] explosion emitter (def %08X) distance %.1f (range %.1f)\n", hash, (double)d, (double)range);
    }
    if (!(d < range))
        return;
    last = now;
    recomp_screenfx_trigger(1, 1.0f - d / range);
}

/* SCREEN-FX rain: raindrops on the view while the player is out in the rain. The rain is a particle emitter built from a
 * weather prefab (weather_defs.h: St. Nazaire "STN_rain01" / "STN_weather01..04", generated from the level files; the
 * Battle of the Bulge ones are snow and the desert ones dust, both left out). Every emitter created (game_vfx_spawn, the
 * hook at the top of sub_000B8250) whose definition hash is in that set is remembered; game_rain_poll() (about every
 * 100 ms from draw_poll) re-validates them (address window, definition pointer, name hash still the weather one) and
 * keeps the raindrops going while at least one is alive. The WeatherExteriorTrigger volumes presumably create / remove
 * that emitter when the player goes in / out of a building: whatever the real switch is, the log line
 * "[SCREENFX] rain: ..." (every 5 s while a weather emitter is tracked) shows what we see.
 * RECOMP_SCREEN_RAIN_DIST=<m> adds a distance test (emitter to camera), RECOMP_SCREEN_RAIN_FORCE=1 = always raining,
 * RECOMP_SCREEN_RAIN=0..3 the amount. First 200 distinct emitter definition hashes are logged as "[SCREENFX] emitter def". */
#include "weather_defs.h"
extern void recomp_screenfx_rain(float strength);
extern int recomp_screenfx_rain_on(void);
extern int recomp_screenfx_get(int kind);
static uint32_t s_wx_hash[2 * (sizeof k_weather_defs / sizeof k_weather_defs[0])];
static int s_wx_n = -1;
#define WX_MAX 24
static struct { uint32_t em, hash; } s_wx[WX_MAX];
static int s_wx_cnt;
static DWORD s_wx_last_log;
static float s_wx_dist = -1.0f;

static void wx_init(void)
{
    char path[200], *c;
    const char *e;
    size_t i;
    s_wx_n = 0;
    for (i = 0; i < sizeof k_weather_defs / sizeof k_weather_defs[0]; i++) {
        snprintf(path, sizeof path, "file:\\z:\\moh4\\data\\shared\\vfx\\%s", k_weather_defs[i]);
        s_wx_hash[s_wx_n++] = vx_name_hash(path);
        for (c = path; *c; c++)
            if (*c == '\\')
                *c = '/';
        s_wx_hash[s_wx_n++] = vx_name_hash(path);
    }
    qsort(s_wx_hash, (size_t)s_wx_n, sizeof s_wx_hash[0], ex_cmp);
    e = getenv("RECOMP_SCREEN_RAIN_DIST");
    if (e && atof(e) > 0.0)
        s_wx_dist = (float)atof(e);
    fprintf(stderr, "[SCREENFX] %d weather definition hashes, rain distance test %s\n", s_wx_n, s_wx_dist > 0.0f ? "on" : "off");
}

static unsigned s_wx_spawns;                /* weather emitters created since the log started (summary line) */
/* The game's own rain (Recomp Settings: Rain amount / size / speed, 0..3 each). The definitions are rewritten at
 * capture time (vx_apply_weather), before the weather emitters are created, so their pools are sized from the amount in
 * force then; s_wr_cap = that amount once emitters exist: a live change may lower the amount, never raise it above the cap
 * (the pool would overflow). A raise takes effect from the next level load. */
static int s_wr_lv[3] = { -1, -1, -1 };
static float s_wr_applied = 1.0f;
static float s_wr_cap;

static void wx_reset(void)
{
    memset(s_wx, 0, sizeof s_wx);
    s_wx_cnt = 0;
    s_wr_cap = 0.0f;
}

static int wx_is_weather(uint32_t hash)
{
    if (s_wx_n < 0)
        wx_init();
    return bsearch(&hash, s_wx_hash, (size_t)s_wx_n, sizeof s_wx_hash[0], ex_cmp) != NULL;
}

/* called for every particle emitter that is created (emitter = this of sub_000B8250, hash = its definition's name hash) */
static void wx_spawned(uint32_t emitter, uint32_t hash)
{
    static uint32_t seen[200];
    static int nseen;
    int i, free_slot = -1;
    if (s_wx_n < 0)
        wx_init();
    if (nseen < 200) {
        for (i = 0; i < nseen; i++)
            if (seen[i] == hash)
                break;
        if (i == nseen) {
            seen[nseen++] = hash;
            fprintf(stderr, "[SCREENFX] emitter def %08X%s\n", hash, bsearch(&hash, s_wx_hash, (size_t)s_wx_n, sizeof s_wx_hash[0], ex_cmp) ? " (WEATHER)" : "");
        }
    }
    if (!bsearch(&hash, s_wx_hash, (size_t)s_wx_n, sizeof s_wx_hash[0], ex_cmp))
        return;
    s_wx_spawns++;
    for (i = 0; i < WX_MAX; i++) {
        if (s_wx[i].em == emitter) {
            s_wx[i].hash = hash;
            return;
        }
        if (!s_wx[i].em && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0)
        free_slot = s_wx_cnt++ % WX_MAX;
    s_wx[free_slot].em = emitter;
    s_wx[free_slot].hash = hash;
    if (s_wr_cap <= 0.0f)                   /* the first weather emitter of this level: the pools are sized now */
        s_wr_cap = s_wr_applied;
    if (s_wx_spawns <= 30)                 /* the rest only counts (the 5 s summary line): short-lived rain emitters would flood the log */
        fprintf(stderr, "[SCREENFX] weather emitter created: %08X def %08X (slot %d)\n", emitter, hash, free_slot);
}

/* 1 while a menu / the pause screen is up (the byte mkb.ini calls menu_flag, 0x378964 == 1); read by the screen-effects
 * thread, which then draws nothing */
static int wx_menu_up(void)
{
    const volatile uint8_t *mem = (const volatile uint8_t *)(uintptr_t)g_xbox_mem_offset;
    return mem[0x378964u] == 1;
}

/* End (or Pause) = dump the weather emitters' structs: press it once outdoors and once indoors and compare the lines
 * (which byte flips tells the game's own indoor / outdoor switch). */
static void wx_dump(void)
{
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    static unsigned n;
    int i, o;
    n++;
    fprintf(stderr, "[WXDUMP] #%u t=%u menu=%d\n", n, (unsigned)(GetTickCount64() / 1000), wx_menu_up());
    for (i = 0; i < WX_MAX; i++) {
        uint32_t em = s_wx[i].em;
        if (!em || !FXP_OK(em))
            continue;
        for (o = 0; o < 0x220; o += 16)
            fprintf(stderr, "[WXDUMP] #%u slot %d em %08X +%03X: %08X %08X %08X %08X\n", n, i, em, o,
                    *(const uint32_t *)(mem + em + o), *(const uint32_t *)(mem + em + o + 4),
                    *(const uint32_t *)(mem + em + o + 8), *(const uint32_t *)(mem + em + o + 12));
    }
}

int game_rain_on_get(void);
void game_rain_poll(void)
{
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    static DWORD last;
    static int was = -1, force = -1, registered, keydown;
    static uint8_t pf[WX_MAX][2];
    static float page[WX_MAX];
    static unsigned nchg;
    DWORD now = GetTickCount();
    int i, tracked = 0, alive = 0, active = 0, on;
    float nearest = 1e30f;
    uint32_t cam;
    {
        int k = game_dev_keys() && ((GetAsyncKeyState(VK_END) | GetAsyncKeyState(VK_PAUSE)) & 0x8000) != 0;
        if (k && !keydown)
            wx_dump();
        keydown = k;
    }
    if (!registered) {
        extern void recomp_screenfx_set_menu_cb(int (*cb)(void));
        recomp_screenfx_set_menu_cb(wx_menu_up);
        registered = 1;
    }
    if (now - last < 100)
        return;
    last = now;
    if (force < 0) {
        const char *e = getenv("RECOMP_SCREEN_RAIN_FORCE");
        force = e && *e == '1';
    }
    if (force) {                           /* drops are made by screen_fx itself, nothing to track */
        recomp_screenfx_rain(1.0f);
        return;
    }
    if (s_wx_n < 0)
        wx_init();
    if (recomp_screenfx_get(2) <= 0 || !game_rain_on_get()) {
        if (was != 0)
            recomp_screenfx_rain(0.0f);
        was = 0;
        return;
    }
    cam = *(const uint32_t *)(mem + 0x3E8D08u);
    for (i = 0; i < WX_MAX; i++) {
        uint32_t em = s_wx[i].em, def;
        if (!em)
            continue;
        tracked++;
        if (!FXP_OK(em))
            continue;
        def = *(const uint32_t *)(mem + em + 0x1E0);
        if (!FXP_OK(def) || *(const uint32_t *)(mem + def + 0x100) != s_wx[i].hash) {
            s_wx[i].em = 0;                /* gone (freed / reused for something else) */
            continue;
        }
        alive++;
        {   /* +0x20C bit 0 = the emitter is running (sub_000B7D30 skips its whole update while it is clear) */
            uint8_t b0 = mem[em + 0x20C], b1 = mem[em + 0x20D];
            if (b0 != pf[i][0] || b1 != pf[i][1]) {
                if (nchg++ < 80)
                    fprintf(stderr, "[SCREENFX] weather emitter %08X flags 20C %02X->%02X 20D %02X->%02X t=%u\n", em, pf[i][0], b0,
                            pf[i][1], b1, (unsigned)(GetTickCount64() / 1000));
                pf[i][0] = b0;
                pf[i][1] = b1;
            }
            if (b0 & 1)
                active++;
        }
        if (FXP_OK(cam)) {
            float dx = *(const float *)(mem + em + 0xA0) - *(const float *)(mem + cam + 0x90);
            float dy = *(const float *)(mem + em + 0xA4) - *(const float *)(mem + cam + 0x94);
            float dz = *(const float *)(mem + em + 0xA8) - *(const float *)(mem + cam + 0x98);
            float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d == d && d < nearest)
                nearest = d;
        }
    }
    on = active > 0 && (s_wx_dist <= 0.0f || nearest < s_wx_dist);
    recomp_screenfx_rain(on ? 1.0f : 0.0f);
    if (on != was || (tracked && now - s_wx_last_log > 5000)) {
        char age[WX_MAX * 14];
        int len = 0;
        age[0] = 0;
        for (i = 0; i < WX_MAX && len < (int)sizeof age - 16; i++) {      /* does the emitter's clock (+0x1F0) still run? */
            uint32_t em = s_wx[i].em;
            float a;
            if (!em || !FXP_OK(em))
                continue;
            memcpy(&a, mem + em + 0x1F0, 4);
            len += snprintf(age + len, sizeof age - (size_t)len, " %.2f(+%.2f)", (double)a, (double)(a - page[i]));
            page[i] = a;
        }
        s_wx_last_log = now;
        fprintf(stderr, "[SCREENFX] rain: %s (tracked %d, alive %d, running %d, nearest %.1f m, created so far %u, clocks%s)\n",
                on ? "ON" : "off", tracked, alive, active, nearest < 1e29f ? (double)nearest : -1.0, s_wx_spawns, age);
    }
    was = on;
}

/* ---- the game's own rain: Rain amount / Rain size / Rain speed ----
 * Fields of the weather particle definitions (same entry layout as blood): +0x70 EmitRate, +0x2C/+0x30 AlphaStart,
 * +0xC0..+0xCC sizes, +0xA4..+0xB8 velocity min/max, +0x98..+0xA0 acceleration. Life (+0x74) is never touched. */
static void vx_apply(void);
static float vx_f(uint32_t w);
static uint32_t vx_w(float f);
static const float k_wr_amount[4] = { 0.4f, 1.0f, 2.0f, 3.0f };
static const float k_wr_alpha[4]  = { 0.75f, 1.0f, 1.3f, 1.6f };
static const float k_wr_size[4]   = { 0.6f, 1.0f, 1.5f, 2.2f };
static const float k_wr_speed[4]  = { 0.6f, 1.0f, 1.5f, 2.2f };
static const char *const k_wr_name[3][4] = {
    { "Light", "Normal", "Heavy", "Storm" }, { "Small", "Normal", "Large", "Huge" }, { "Slow", "Normal", "Fast", "Very fast" } };

static void wr_init(void)
{
    static const char *const k_env[3] = { "RECOMP_RAIN_AMOUNT", "RECOMP_RAIN_SIZE", "RECOMP_RAIN_SPEED" };
    int i;
    for (i = 0; i < 3; i++)
        if (s_wr_lv[i] < 0) {
            const char *e = getenv(k_env[i]);
            s_wr_lv[i] = e && *e ? (atoi(e) < 0 ? 0 : atoi(e) > 3 ? 3 : atoi(e)) : 1;
        }
}

int game_rain_cfg_get(int which)
{
    wr_init();
    return s_wr_lv[which < 0 ? 0 : which > 2 ? 2 : which];
}

void game_rain_cfg_set(int which, int lv)
{
    wr_init();
    s_wr_lv[which < 0 ? 0 : which > 2 ? 2 : which] = lv < 0 ? 0 : lv > 3 ? 3 : lv;
    vx_apply();
}

const char *game_rain_cfg_name(int which, int lv)
{
    return k_wr_name[which < 0 ? 0 : which > 2 ? 2 : which][lv < 0 ? 0 : lv > 3 ? 3 : lv];
}

/* 1 while the amount asked for is above what this level's rain pools can take (it applies at the next level load) */
int game_rain_cfg_pending(void)
{
    wr_init();
    return s_wr_cap > 0.0f && k_wr_amount[s_wr_lv[0]] > s_wr_cap + 0.001f;
}

/* Recomp Settings "Rain" (default Off). Off makes the game's own rain invisible: the alpha of the weather
 * definitions goes to 0, while the emitters and their pools stay as they are, so turning it on is live. The
 * screen raindrops stop too (game_rain_poll). RECOMP_RAIN=1 turns it on at start-up (the settings file sets it). */
static int s_rain_on = -1;
static void rain_master_init(void)
{
    if (s_rain_on < 0) {
        const char *e = getenv("RECOMP_RAIN");
        s_rain_on = e && *e == '1';
    }
}
int game_rain_on_get(void)
{
    rain_master_init();
    return s_rain_on;
}
void game_rain_on_set(int on)
{
    rain_master_init();
    s_rain_on = on ? 1 : 0;
    vx_apply();
}

static void vx_apply_weather(int e)
{
    uint8_t *mem = (uint8_t *)(uintptr_t)g_xbox_mem_offset;
    uint32_t *p = (uint32_t *)(mem + s_vx[e].va);
    const uint32_t *o = s_vx[e].orig;
    static const uint32_t k_size[4] = { 0xC0, 0xC4, 0xC8, 0xCC };
    float am, al, sz, sp;
    int k, c, orate;
    wr_init();
    am = k_wr_amount[s_wr_lv[0]];
    if (s_wr_cap > 0.0f && am > s_wr_cap)
        am = s_wr_cap;
    al = k_wr_alpha[s_wr_lv[0]];
    if (!game_rain_on_get())
        al = 0.0f;                      /* Rain is Off: the drops are made but not drawn */
    sz = k_wr_size[s_wr_lv[1]];
    sp = k_wr_speed[s_wr_lv[2]];
    s_wr_applied = am;
    orate = (int)o[0x70 / 4];
    c = (int)((float)orate * am + 0.5f);
    p[0x70 / 4] = (uint32_t)(c < 1 ? 1 : c > 400 ? 400 : c);
    for (k = 0; k < 4; k++)
        p[k_size[k] / 4] = vx_w(vx_f(o[k_size[k] / 4]) * sz);
    for (k = 0x2C; k <= 0x30; k += 4) {
        int a = (int)o[k / 4];
        if (a > 0 && a <= 255) {
            a = (int)((float)a * al + 0.5f);
            p[k / 4] = (uint32_t)(a > 255 ? 255 : a);
        }
    }
    for (k = 0xA4; k <= 0xB8; k += 4)
        p[k / 4] = vx_w(vx_f(o[k / 4]) * sp);
    for (k = 0x98; k <= 0xA0; k += 4)
        p[k / 4] = vx_w(vx_f(o[k / 4]) * sp);
}

/* Recomp Settings "Blood colour": 0 = the game's own, 1 = crimson (default), 2 = dark red, 3 = custom (hue / saturation / brightness). */
#define BLOOD_COL_N 4
static int s_blood_col = 1;
static int s_blood_hue = 350, s_blood_sat = 95, s_blood_bright = 55;      /* own colour: degrees, %, % */
static int vx_clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}
static int vx_wrap360(int h)
{
    return ((h % 360) + 360) % 360;
}
/* hue in degrees, saturation and brightness in percent -> RGB 0..255 */
static void vx_hsv_rgb(int h, int s, int v, int rgb[3])
{
    float sat = (float)vx_clamp(s, 0, 100) / 100.0f, val = (float)vx_clamp(v, 0, 100) * 2.55f;
    float hf = (float)vx_wrap360(h) / 60.0f, f = hf - (float)(int)hf;
    float p = val * (1.0f - sat), q = val * (1.0f - sat * f), t = val * (1.0f - sat * (1.0f - f));
    float c[3];
    int k;

    switch ((int)hf) {
    case 0:  c[0] = val; c[1] = t;   c[2] = p;   break;
    case 1:  c[0] = q;   c[1] = val; c[2] = p;   break;
    case 2:  c[0] = p;   c[1] = val; c[2] = t;   break;
    case 3:  c[0] = p;   c[1] = q;   c[2] = val; break;
    case 4:  c[0] = t;   c[1] = p;   c[2] = val; break;
    default: c[0] = val; c[1] = p;   c[2] = q;   break;
    }
    for (k = 0; k < 3; k++)
        rgb[k] = vx_clamp((int)(c[k] + 0.5f), 0, 255);
}
static void vx_color_env(void)
{
    const char *c = getenv("RECOMP_BLOOD_COLOR"), *h = getenv("RECOMP_BLOOD_HUE");
    const char *s = getenv("RECOMP_BLOOD_SAT"), *b = getenv("RECOMP_BLOOD_BRIGHT");

    if (c && *c) {
        int v = atoi(c);
        s_blood_col = v < 0 ? 0 : v >= BLOOD_COL_N ? BLOOD_COL_N - 1 : v;
    }
    if (h && *h)
        s_blood_hue = vx_wrap360(atoi(h));
    if (s && *s)
        s_blood_sat = vx_clamp(atoi(s), 0, 100);
    if (b && *b)
        s_blood_bright = vx_clamp(atoi(b), 10, 100);
}
static void vx_colors(int out[4][3])
{
    static const int k_crimson[4][3] = { { 125, 8, 30 }, { 155, 16, 44 }, { 85, 4, 20 }, { 110, 8, 30 } };
    static const int k_dark[4][3] = { { 85, 4, 4 }, { 115, 8, 8 }, { 55, 2, 2 }, { 75, 4, 4 } };
    int k, rgb[3];

    if (s_blood_col == 1 || s_blood_col == 2) {
        memcpy(out, s_blood_col == 1 ? k_crimson : k_dark, sizeof k_crimson);
        return;
    }
    vx_hsv_rgb(s_blood_hue, s_blood_sat, s_blood_bright, rgb);
    for (k = 0; k < 3; k++) {
        int c = rgb[k];
        out[0][k] = c * 4 / 5;
        out[1][k] = c;
        out[2][k] = c / 2;
        out[3][k] = c * 7 / 10;
    }
}

static void vx_set_level(int l)
{
    s_blood_level = l < 0 ? 0 : l > 3 ? 3 : l;
    s_bl.cnt = k_blood[s_blood_level].cnt;
    s_bl.size = k_blood[s_blood_level].size;
    s_bl.alpha = k_blood[s_blood_level].alpha;
    s_bl.endsc = k_blood[s_blood_level].endsc;
    s_bl.life = k_blood[s_blood_level].life;
    s_bl.fade = k_blood[s_blood_level].fade;
    s_bl.grav = k_blood[s_blood_level].grav;
    s_bl.vel = k_blood[s_blood_level].vel;
}

static void vx_levels(void)
{
    if (s_blood_level < 0) {
        const char *e = getenv("RECOMP_BLOOD");
        static const struct { const char *name; float *dst; } k_env[] = {
            { "RECOMP_BLOOD_COUNT", &s_bl.cnt }, { "RECOMP_BLOOD_SIZE", &s_bl.size }, { "RECOMP_BLOOD_ALPHA", &s_bl.alpha },
            { "RECOMP_BLOOD_END", &s_bl.endsc }, { "RECOMP_BLOOD_LIFE", &s_bl.life }, { "RECOMP_BLOOD_FADE", &s_bl.fade },
            { "RECOMP_BLOOD_GRAVITY", &s_bl.grav }, { "RECOMP_BLOOD_VEL", &s_bl.vel },
        };
        int i;
        vx_set_level(e ? atoi(e) : 0);
        for (i = 0; i < (int)(sizeof k_env / sizeof k_env[0]); i++) {
            const char *v = getenv(k_env[i].name);
            if (v && *v)
                *k_env[i].dst = (float)atof(v);
        }
        vx_color_env();
    }
}

static float vx_f(uint32_t w)
{
    float f;
    memcpy(&f, &w, 4);
    return f;
}

static uint32_t vx_w(float f)
{
    uint32_t w;
    memcpy(&w, &f, 4);
    return w;
}

static void vx_apply(void)
{
    uint8_t *mem = (uint8_t *)(uintptr_t)g_xbox_mem_offset;
    static const uint32_t k_size[4] = { 0xC0, 0xC4, 0xC8, 0xCC };
    int e, k, t, col[4][3];
    vx_levels();
    if (s_blood_col)
        vx_colors(col);
    for (e = 0; e < s_vx_n; e++) {
        uint32_t *p;
        const uint32_t *o = s_vx[e].orig;
        if (s_vx[e].have && s_vx[e].weather) {        /* the game's rain: its own settings, not blood's */
            if (!vx_alive(e)) {
                s_vx[e].weather = 0;
            } else if (*(const uint32_t *)(mem + s_vx[e].va + 0x100) != s_vx[e].hash) {
                s_vx[e].have = 0;
            } else {
                vx_apply_weather(e);
            }
            continue;
        }
        if (!s_vx[e].have || !s_vx[e].blood)
            continue;
        if (!vx_alive(e)) {
            s_vx[e].have = 1;
            s_vx[e].blood = 0;
            continue;
        }
        p = (uint32_t *)(mem + s_vx[e].va);
        if (p[0x100 / 4] != s_vx[e].hash) {          /* not the definition we captured any more */
            s_vx[e].have = 0;
            continue;
        }
        {
            int c = (int)((float)(int)o[0x70 / 4] * s_bl.cnt + 0.5f);
            p[0x70 / 4] = (uint32_t)(c < 1 ? 1 : c > 64 ? 64 : c);
        }
        for (k = 0; k < 4; k++)
            p[k_size[k] / 4] = vx_w(vx_f(o[k_size[k] / 4]) * s_bl.size);
        p[0xBC / 4] = vx_w(vx_f(o[0xBC / 4]) * s_bl.endsc);
        p[0x74 / 4] = (vx_dur_init(), s_bl_dur_idx) > 0 ?vx_w(k_blood_dur[s_bl_dur_idx]) : vx_w(vx_f(o[0x74 / 4]) * s_bl.life);
        for (k = 0xA4; k <= 0xB8; k += 4)                  /* the velocity min / max floats: slower = the spray stays near */
            p[k / 4] = vx_w(vx_f(o[k / 4]) * s_bl.vel);
        for (k = 0x2C; k <= 0x30; k += 4) {
            int a = (int)o[k / 4];
            if (a > 0 && a <= 255) {
                a = (int)((float)a * s_bl.alpha + 0.5f);
                p[k / 4] = (uint32_t)(a > 255 ? 255 : a);
            }
        }
        /* R,G,B int triplets at +0x38..+0x64: start min / max, end min / max */
        for (t = 0; t < 4; t++)
            for (k = 0; k < 3; k++)
                p[(0x38 + t * 0xC + k * 4) / 4] = s_blood_col ? (uint32_t)col[t][k] : o[(0x38 + t * 0xC + k * 4) / 4];
        /* a gradual fade: the end alpha goes to 0 (the head blood stayed at 128 and vanished at once) and the fade starts
         * early; both only when the level changes something */
        p[0x34 / 4] = o[0x34 / 4];
        p[0xF0 / 4] = o[0xF0 / 4];
        if (s_bl.fade != 1.0f) {
            p[0x34 / 4] = 0;
            if (vx_f(o[0xF0 / 4]) > s_bl.fade)
                p[0xF0 / 4] = vx_w(s_bl.fade);
        }
        p[0xA0 / 4] = o[0xA0 / 4];
        p[0x170 / 4] = o[0x170 / 4];
        if (s_bl.grav != 0.0f) {
            p[0xA0 / 4] = vx_w(s_bl.grav);
            p[0x170 / 4] = (o[0x170 / 4] & ~0xFFu) | 1u;
        }
    }
}

int game_blood_get(void)
{
    vx_levels();
    return s_blood_level;
}

void game_blood_set(int l)
{
    vx_levels();
    vx_set_level(l);
    vx_apply();
    fprintf(stderr, "[BLOOD] %s (count x%g, size x%g, alpha x%g, end x%g, life x%g, fade %g, gravity %g, speed x%g)\n",
            k_blood[s_blood_level].name, (double)s_bl.cnt, (double)s_bl.size, (double)s_bl.alpha, (double)s_bl.endsc,
            (double)s_bl.life, (double)s_bl.fade, (double)s_bl.grav, (double)s_bl.vel);
}

static const char *const k_blood_col_name[BLOOD_COL_N] = { "Original", "Crimson", "Dark red", "Custom" };

int game_blood_color_get(void)
{
    vx_levels();
    return s_blood_col;
}

void game_blood_color_set(int i)
{
    vx_levels();
    s_blood_col = i < 0 ? 0 : i >= BLOOD_COL_N ? BLOOD_COL_N - 1 : i;
    vx_apply();
    fprintf(stderr, "[BLOOD] colour %s\n", k_blood_col_name[s_blood_col]);
}

const char *game_blood_color_name(int i)
{
    return k_blood_col_name[i < 0 ? 0 : i >= BLOOD_COL_N ? BLOOD_COL_N - 1 : i];
}

/* the custom colour: hue, saturation and brightness rows; the spray is re-tinted at once */
static void vx_custom_changed(void)
{
    int rgb[3];
    vx_hsv_rgb(s_blood_hue, s_blood_sat, s_blood_bright, rgb);
    vx_apply();
    fprintf(stderr, "[BLOOD] custom hue %d sat %d bright %d = rgb %d,%d,%d\n", s_blood_hue, s_blood_sat, s_blood_bright,
            rgb[0], rgb[1], rgb[2]);
}

int game_blood_hue_get(void)
{
    vx_levels();
    return s_blood_hue;
}

void game_blood_hue_set(int h)
{
    vx_levels();
    s_blood_hue = vx_wrap360(h);
    vx_custom_changed();
}

int game_blood_sat_get(void)
{
    vx_levels();
    return s_blood_sat;
}

void game_blood_sat_set(int s)
{
    vx_levels();
    s_blood_sat = vx_clamp(s, 0, 100);
    vx_custom_changed();
}

int game_blood_bright_get(void)
{
    vx_levels();
    return s_blood_bright;
}

void game_blood_bright_set(int v)
{
    vx_levels();
    s_blood_bright = vx_clamp(v, 10, 100);
    vx_custom_changed();
}

/* the RGB the custom settings give, for the swatch in Recomp Settings */
void game_blood_custom_rgb(int rgb[3])
{
    vx_levels();
    vx_hsv_rgb(s_blood_hue, s_blood_sat, s_blood_bright, rgb);
}

static void vx_dur_init(void)
{
    if (s_bl_dur_idx < 0) {
        const char *e = getenv("RECOMP_BLOOD_TIME");
        s_bl_dur_idx = e ? atoi(e) : 0;
        if (s_bl_dur_idx < 0 || s_bl_dur_idx >= BLOOD_DUR_N)
            s_bl_dur_idx = 0;
    }
}

/* RECOMP_VFX_SPAWN=1 (diagnostic): every particle emitter created (sub_000B8250, this = the emitter, this+0x1E0 = its
 * type definition whose +0x100 is the name hash) prints "[SPAWN] t=<ms> <definition name>". Answers which effect the
 * game picked for each hit (blood / dust / nothing). */
void game_vfx_spawn(void)
{
    static int on = -1, n;
    static struct { uint32_t h; const char *name; } tab[40];
    static int ntab;
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    uint32_t emitter = g_ecx, def, hash;
    int i;
    if (on < 0) {
        static const char *k_imp[] = { "Brick01", "Concrete01", "Dirt01", "Dirt02", "Dust02", "Glass04", "Pls_impactBodyBlood01",
                                       "Pls_impactBodyDust01", "Pls_impactBodyDust02", "Pls_impactBodyDust03",
                                       "Pls_impactHeadBlood01", "STNAZ_WoodMP01", "Sand01", "Spark02", "Wood01" };
        const char *e = getenv("RECOMP_VFX_SPAWN");
        on = e && *e == '1';
        if (on) {
            char path[200];
            for (i = 0; i < (int)(sizeof k_imp / sizeof k_imp[0]); i++) {
                snprintf(path, sizeof path, "file:\\z:\\moh4\\data\\shared\\vfx\\definitions\\impacts\\%s.xdd", k_imp[i]);
                tab[ntab].h = vx_name_hash(path);
                tab[ntab++].name = k_imp[i];
            }
            tab[ntab].h = vx_name_hash("file:\\z:\\moh4\\data\\shared\\vfx\\definitions\\MOH3\\Pls_impactConcrete01.xdd");
            tab[ntab++].name = "Pls_impactConcrete01";
        }
    }
    if (FXP_OK(emitter)) {                 /* SCREEN-FX rain: remember the weather emitters (cheap: two reads + a search) */
        uint32_t wdef = *(const uint32_t *)(mem + emitter + 0x1E0);
        if (FXP_OK(wdef))
            wx_spawned(emitter, *(const uint32_t *)(mem + wdef + 0x100));
    }
    if (!on || n > 4000 || emitter < 0x10000u || emitter >= 0xFD000000u)
        return;
    def = *(const uint32_t *)(mem + emitter + 0x1E0);
    if (def < 0x10000u || def >= 0xFD000000u)
        return;
    hash = *(const uint32_t *)(mem + def + 0x100);
    for (i = 0; i < ntab; i++)
        if (tab[i].h == hash)
            break;
    if (i < ntab) { n++; /* "other" emitters (level load, tracers) are not logged: they used up the 4000-line cap */
        extern uint32_t g_dbg_last_mat;
        char st[400];
        int len = 0, k;
        st[0] = 0;
        if (!strncmp(tab[i].name, "Pls_impact", 10)) /* who spawns body/head hit effects: guest stack code addresses */
            for (k = 0; k < 48 && len < (int)sizeof st - 12; k++) {
                uint32_t v = *(const uint32_t *)(mem + (uint32_t)g_esp + 4u * (uint32_t)k);
                if (v >= 0x11000u && v < 0x2D8000u)
                    len += snprintf(st + len, sizeof st - (size_t)len, " %X", (unsigned)v);
            }
        fprintf(stderr, "[SPAWN] t=%lu %s (%08X) lastmat=%u%s%s\n", (unsigned long)GetTickCount(), tab[i].name, hash,
                g_dbg_last_mat, st[0] ? " stack:" : "", st);
    }
}

int game_blood_time_get(void)
{
    vx_dur_init();
    return s_bl_dur_idx;
}

void game_blood_time_set(int i)
{
    vx_dur_init();
    s_bl_dur_idx = i < 0 ? 0 : i >= BLOOD_DUR_N ? BLOOD_DUR_N - 1 : i;
    vx_apply();
    fprintf(stderr, "[BLOOD] duration %g s (0 = by level)\n", (double)k_blood_dur[s_bl_dur_idx]);
}

float game_blood_time_seconds(void)
{
    vx_dur_init();
    return k_blood_dur[s_bl_dur_idx];
}

const char *game_blood_name(int l)
{
    return k_blood[l < 0 ? 0 : l > 3 ? 3 : l].name;
}

void game_vfx_enter(void)
{
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    uint32_t this_ = g_ecx, tbl, idx, va;
    int e;
    uint64_t now;

    tbl = *(const uint32_t *)(mem + this_ + 4);
    idx = *(const uint32_t *)(mem + this_ + 0xC);
    if (tbl < 0x10000u || tbl >= 0xFD000000u || idx > 4096u)
        return;
    va = tbl + idx * VX_ENT;
    now = xbox_HostNowNs();
    if (s_vx_last_ns && now - s_vx_last_ns > 5000000000ull) {  /* a new load: forget the old entries */
        s_vx_n = 0;
        wx_reset();                                            /* SCREEN-FX rain: weather emitters of the old level are gone */
    }
    s_vx_last_ns = now;
    for (e = 0; e < s_vx_n; e++)
        if (s_vx[e].va == va)
            break;
    if (e == s_vx_n && s_vx_n < VX_MAX) {
        s_vx[e].va = va;
        s_vx_n++;
    } else if (e == s_vx_n) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "[BLOOD] entry list full (%d)\n", VX_MAX);
    }
    if (e < s_vx_n) {
        s_vx[e].mgr = this_;
        s_vx[e].tbl = tbl;
        s_vx[e].have = 0;
    }                                       /* being filled: capture later */
}

void game_vfx_poll(void)
{
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    static uint32_t hb, hh;
    int e, got = 0;
    if (!s_vx_n || !s_vx_last_ns || xbox_HostNowNs() - s_vx_last_ns < 2000000000ull)
        return;                                                 /* loading is not finished */
    if (!hb) {
        hb = vx_name_hash("file:\\z:\\moh4\\data\\shared\\vfx\\definitions\\impacts\\Pls_impactBodyBlood01.xdd");
        hh = vx_name_hash("file:\\z:\\moh4\\data\\shared\\vfx\\definitions\\impacts\\Pls_impactHeadBlood01.xdd");
        fprintf(stderr, "[BLOOD] name hashes body %08X head %08X\n", hb, hh);
    }
    for (e = 0; e < s_vx_n; e++) {
        const uint32_t *p;
        if (s_vx[e].have)
            continue;
        if (!vx_alive(e)) {
            s_vx[e].have = 1;
            s_vx[e].blood = 0;
            s_vx[e].weather = 0;
            continue;
        }
        p = (const uint32_t *)(mem + s_vx[e].va);
        memcpy(s_vx[e].orig, p, sizeof s_vx[e].orig);
        s_vx[e].hash = p[0x100 / 4];
        s_vx[e].blood = s_vx[e].hash == hb || s_vx[e].hash == hh;
        s_vx[e].weather = wx_is_weather(s_vx[e].hash);
        s_vx[e].have = 1;
        got++;
        if (s_vx[e].weather) {                                  /* the game's rain: print what we are about to scale */
            float sz[4], vel[6], acc[3];
            memcpy(sz, &p[0xC0 / 4], 16);
            memcpy(vel, &p[0xA4 / 4], 24);
            memcpy(acc, &p[0x98 / 4], 12);
            fprintf(stderr, "[RAIN] weather def %08X at %08X: emit %d life %g size %g %g %g %g vel %g %g %g %g %g %g accel %g %g %g"
                    " alpha %d %d -> %d\n", s_vx[e].hash, s_vx[e].va, (int)p[0x70 / 4], (double)vx_f(p[0x74 / 4]), (double)sz[0],
                    (double)sz[1], (double)sz[2], (double)sz[3], (double)vel[0], (double)vel[1], (double)vel[2], (double)vel[3],
                    (double)vel[4], (double)vel[5], (double)acc[0], (double)acc[1], (double)acc[2], (int)p[0x2C / 4],
                    (int)p[0x30 / 4], (int)p[0x34 / 4]);
        }
        if (getenv("RECOMP_VFX_LOG")) {
            float sz0, sz1;
            memcpy(&sz0, &p[0xC8 / 4], 4);
            memcpy(&sz1, &p[0xCC / 4], 4);
            fprintf(stderr, "[VFX] entry %08X hash %08X emit %d size %g %g%s\n", s_vx[e].va, s_vx[e].hash, (int)p[0x70 / 4],
                    (double)sz0, (double)sz1, s_vx[e].blood ? "  <- blood" : "");
        }
        if (s_vx[e].blood) {
            int i;
            float sz[4], es;
            memcpy(sz, &p[0xC0 / 4], 16);
            memcpy(&es, &p[0xBC / 4], 4);
            fprintf(stderr, "[BLOOD] %s at %08X: emit %d, size %g %g %g %g, end scale %g, alpha %d %d -> %d\n",
                    s_vx[e].hash == hb ? "body" : "head", s_vx[e].va, (int)p[0x70 / 4], (double)sz[0], (double)sz[1],
                    (double)sz[2], (double)sz[3], (double)es, (int)p[0x2C / 4], (int)p[0x30 / 4], (int)p[0x34 / 4]);
            if (getenv("RECOMP_VFX_LOG")) {
                for (i = 0; i < VX_WORDS; i++)
                    if (p[i]) {
                        float f;
                        memcpy(&f, &p[i], 4);
                        fprintf(stderr, "[VFX] %s +%03X = %08X (%g)\n", s_vx[e].hash == hb ? "body" : "head", i * 4, p[i], (double)f);
                    }
            }
        }
    }
    if (got) {
        fprintf(stderr, "[BLOOD] %d particle entries ready\n", s_vx_n);
        vx_apply();
    }
}

/* The game's DirectSound voice start: sub_002F0FEA is the only place that writes the APU's VOICE_ON; it returns at
 * once when (voice+0x12) & 2 is already set. sub_002F128A, sub_002F13CB and sub_002F1A09 are its three callers
 * (the Play paths). Counted per 5 s and printed as "[DSV] ..." so a dead-sound session shows from the log alone
 * whether the game stopped calling Play, or Play was called and the start was skipped. RECOMP_DSV_LOG=0 turns it off. */
static struct { uint32_t c[3], n, tot; } rel_who[48];
static struct { uint32_t c[3]; DWORD tick; } s_last_stop[8];
static unsigned s_last_stop_i;

/* Ring keeper (2026-10-30). [0x5239B8] is EA's software mix bus: a looping 6-channel 48 kHz DirectSound buffer played by three
 * APU voices (handles at mcpx+0x0C/+0x0E/+0x10) that carries music, dialogue and ambience; its position also drives the sound
 * service (pump sub_001965CB -> sub_002F15FF). Seen live: a one-shot format landed on one of its voices, the voice ran off its
 * end, the buffer lost its started bit (state 0x0103 -> 0x0101), the service tick froze and those sounds were gone for good.
 * While it plays, the APU guards its voices (apu_ring_watch); if they end inside the APU anyway, they are restarted here
 * (apu_ring_rearm) and the buffer gets its started bit back. A stop the game made itself is left alone.
 * RECOMP_RING_CLOCK=0 turns the keeper off (RECOMP_RING_GUARD=0 the APU side). */
extern void apu_ring_watch(uint16_t v0, uint16_t v1, uint16_t v2);
extern int apu_ring_rearm(uint16_t v0, uint16_t v1, uint16_t v2);

void game_ring_pos_enter(void)
{
    static int on = -1;
    static uint32_t seen_active, dead_mcpx;
    uint32_t m = g_ecx, ring;
    unsigned i;

    if (on < 0) {
        const char *e = getenv("RECOMP_RING_CLOCK");
        on = !(e && *e == '0');
    }
    if (!on || !m)
        return;
    ring = GUEST32(0x5239B8);
    if (!ring || GUEST32(ring - 0x1C + 0x20) != m)
        return;
    if ((GUEST16(m + 0x12) & 3) == 3) {
        seen_active = m;
        dead_mcpx = 0;
        apu_ring_watch(GUEST16(m + 0x0C), GUEST16(m + 0x0E), GUEST16(m + 0x10));
        return;
    }
    if (seen_active != m)
        return;                                    /* not started yet */
    if (s_flush_mcpx == m && GetTickCount() - s_flush_tick < 5000u)
        return;                                    /* level-change teardown owns it */
    /* The ring's voices are the software mix bus: faking its clock keeps the service ticking but nothing is heard. If the
     * APU ended them (not a game stop), restart them and give the buffer its started bit and looping format back. */
    if (apu_ring_rearm(GUEST16(m + 0x0C), GUEST16(m + 0x0E), GUEST16(m + 0x10))) {
        GUEST32(m + 0x14) |= 0x02000000u;          /* the buffer's copy of the voice format: looping */
        GUEST16(m + 0x12) |= 3;
        fprintf(stderr, "[RING] t=%u ring buffer 0x%08X restarted (state now 0x%04X)\n",
                (unsigned)(GetTickCount() / 1000), m, GUEST16(m + 0x12));
        return;
    }
    /* The game stopped it itself: leave it, only say so once with who stopped voices just before. */
    if (dead_mcpx != m) {
        DWORD tk = GetTickCount();
        dead_mcpx = m;
        fprintf(stderr, "[RING] t=%u ring stream 0x%08X lost its started bit (state 0x%04X, tick 0x%X) by a game stop, left alone."
                " Recent voice stops:\n", (unsigned)(tk / 1000), m, GUEST16(m + 0x12), GUEST32(0x523B90));
        for (i = 0; i < 8; i++)
            if (s_last_stop[i].tick && tk - s_last_stop[i].tick < 3000u)
                fprintf(stderr, "[RING]   %u ms ago from %08X <- %08X <- %08X\n", (unsigned)(tk - s_last_stop[i].tick),
                        s_last_stop[i].c[0], s_last_stop[i].c[1], s_last_stop[i].c[2]);
    }
}

void game_dsv_enter(int id)
{
    static unsigned cnt[8], skip, tot[8], totskip;
    static uint64_t t0;
    static int on = -1;
    uint64_t now;

    if (on < 0) {
        const char *e = getenv("RECOMP_DSV_LOG");
        on = !(e && *e == '0');
    }
    if (!on)
        return;
    cnt[id]++;
    tot[id]++;
    if (id == 4) {   /* who stops voices: first three guest call sites found on the stack (return address preceded by an E8 call) */
        uint32_t chain[3] = {0, 0, 0}, w, k, n = 0;
        for (w = 0; w < 160 && n < 3; w++) {
            uint32_t v = *(const uint32_t *)((uintptr_t)((uint32_t)g_esp + w * 4) + g_xbox_mem_offset);
            if (v >= 0x11000u && v < 0x330000u && *(const uint8_t *)((uintptr_t)(v - 5) + g_xbox_mem_offset) == 0xE8)
                chain[n++] = v;
        }
        {   /* the last few stops, kept so game_ring_pos_enter can say who killed the ring */
            s_last_stop[s_last_stop_i & 7].c[0] = chain[0]; s_last_stop[s_last_stop_i & 7].c[1] = chain[1];
            s_last_stop[s_last_stop_i & 7].c[2] = chain[2]; s_last_stop[s_last_stop_i & 7].tick = GetTickCount();
            s_last_stop_i++;
        }
        for (k = 0; k < 48; k++)
            if (rel_who[k].n && rel_who[k].c[0] == chain[0] && rel_who[k].c[1] == chain[1] && rel_who[k].c[2] == chain[2])
                break;
        if (k == 48)
            for (k = 0; k < 48 && rel_who[k].n; k++)
                ;
        if (k < 48) {
            rel_who[k].c[0] = chain[0]; rel_who[k].c[1] = chain[1]; rel_who[k].c[2] = chain[2];
            rel_who[k].n++;
            rel_who[k].tot = rel_who[k].n;
        }
    }
    if (id == 0 && (*(const uint8_t *)((uintptr_t)(g_ecx + 0x12u) + g_xbox_mem_offset) & 2)) {
        skip++;
        totskip++;
    }
    now = xbox_HostNowNs();
    if (!t0)
        t0 = now;
    else if (now - t0 >= 5000000000ull) {
        fprintf(stderr, "[DSV] t=%u 5 s: voice start %u (skipped, already on: %u), Play paths 128A %u 13CB %u 1A09 %u,"
                " guest on-bit cleared (0CEC %u, 1C9B %u) |"
                " total start %u skipped %u 128A %u 13CB %u 1A09 %u cleared 0CEC %u 1C9B %u\n",
                (unsigned)(GetTickCount64() / 1000), cnt[0], skip, cnt[1], cnt[2], cnt[3], cnt[4], cnt[5],
                tot[0], totskip, tot[1], tot[2], tot[3], tot[4], tot[5]);
        {   /* top voice stoppers of this 5 s window (and their running totals) */
            int shown = 0, pass;
            for (pass = 0; pass < 6; pass++) {
                int best = -1, k;
                for (k = 0; k < 48; k++)
                    if (rel_who[k].n && (best < 0 || rel_who[k].n > rel_who[best].n))
                        best = k;
                if (best < 0)
                    break;
                fprintf(stderr, "[DSV-STOP] x%u (total %u) from %08X <- %08X <- %08X\n", rel_who[best].n, rel_who[best].tot,
                        rel_who[best].c[0], rel_who[best].c[1], rel_who[best].c[2]);
                rel_who[best].n = 0;
                shown++;
            }
            for (pass = 0; pass < 48; pass++)
                rel_who[pass].n = 0;
            (void)shown;
        }
        memset(cnt, 0, sizeof cnt);
        skip = 0;
        t0 = now;
    }
}

/* The game's per-emitter update (sub_001ADA50, EMUPD lines in recomp_0048.c) decides every emitter's fate with three tests:
 * backend object says busy (vtable +0x10), says pending (+0x14), or neither and the byte at +8 is set -> finished (+0x44 = 1).
 * Counted per 5 s as "[EMUPD] t= calls .. busy .. pending .. kept(+8==0) .. finished .. no-backend .." and the last outcome of
 * each emitter is kept in a small table (game_emupd_last) so the [EMHIST] groups can say which test holds an emitter. */
#define EMUPD_SLOTS 4096u
static struct { uint32_t em; uint8_t last; } s_emupd[EMUPD_SLOTS];

int game_emupd_last(uint32_t em)
{
    uint32_t h = (em >> 4) % EMUPD_SLOTS, i;

    for (i = 0; i < 8; i++) {
        uint32_t s = (h + i) % EMUPD_SLOTS;
        if (s_emupd[s].em == em)
            return s_emupd[s].last;
        if (!s_emupd[s].em)
            break;
    }
    return -1;
}

void game_emupd(int outcome, uint32_t em)
{
    static unsigned cnt[6], tot[6];
    static uint64_t t0;
    uint64_t now;
    uint32_t h = (em >> 4) % EMUPD_SLOTS, i;

    cnt[outcome]++;
    tot[outcome]++;
    for (i = 0; i < 8; i++) {
        uint32_t s = (h + i) % EMUPD_SLOTS;
        if (s_emupd[s].em == em || !s_emupd[s].em) {
            s_emupd[s].em = em;
            if (outcome)                        /* 0 only says "asked"; the answer follows */
                s_emupd[s].last = (uint8_t)outcome;
            break;
        }
    }
    now = GetTickCount64();
    if (!t0)
        t0 = now;
    else if (now - t0 >= 5000) {
        fprintf(stderr, "[EMUPD] t=%u 5 s: asked %u busy %u pending %u kept(+8==0) %u finished %u no-backend %u | total busy %u pending %u kept %u finished %u\n",
                (unsigned)(now / 1000), cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], cnt[5], tot[1], tot[2], tot[3], tot[4]);
        memset(cnt, 0, sizeof cnt);
        t0 = now;
    }
}

/* ---- Cutscenes: the movies play again, a button can end them -------------------
 *
 * The game's movie player (sub_00136DA0) calls a per-frame callback it was given;
 * a zero result ends the movie. The two callbacks (sub_00179100 for the logo and
 * menu movies, sub_0007F260 for the mission movies) start with
 * game_movie_skip_pressed() -- MOVIE-SKIP lines in recomp_0043.c / recomp_0014.c.
 *
 * Modes (Recomp Settings > Cutscenes, recomp_settings.ini cutscenes=):
 *   0 play, a button skips (default)   1 play, no skipping   2 never show
 * Mode 2 makes NtCreateFile report the .vp6 files as absent (kernel_bridge.c,
 * g_skip_movies), the way every run did before this option existed.
 */
extern volatile int g_skip_movies;
static volatile int s_cut_mode;

int game_cutscene_mode_get(void)
{
    return s_cut_mode;
}

void game_cutscene_mode_set(int m)
{
    s_cut_mode = (m < 0 || m > 2) ? 0 : m;
    g_skip_movies = (s_cut_mode == 2);
}

typedef struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } CutXiState;

/* Enter, Space, Esc, the left mouse button, or A/B/X/Y/Start/Back on a pad, while the game window is in front. */
static int cut_buttons_down(void)
{
    static HMODULE xi;
    static DWORD (WINAPI *get)(DWORD, CutXiState *);
    static int tried;
    DWORD pid = 0;
    CutXiState st;
    DWORD i;

    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId())
        return 0;
    if ((GetAsyncKeyState(VK_RETURN) | GetAsyncKeyState(VK_SPACE) | GetAsyncKeyState(VK_ESCAPE) |
         GetAsyncKeyState(VK_LBUTTON)) & 0x8000)
        return 1;
    if (!tried) {
        tried = 1;
        xi = LoadLibraryA("xinput1_4.dll");
        if (xi)
            get = (DWORD (WINAPI *)(DWORD, CutXiState *))GetProcAddress(xi, "XInputGetState");
    }
    for (i = 0; get && i < 4; i++)
        if (get(i, &st) == ERROR_SUCCESS && (st.buttons & 0xF030))
            return 1;
    return 0;
}

/* Called once per movie frame from the two callbacks; nonzero = end the movie now.
 * A press only counts after every skip button has been seen released since the movie
 * began (the key that started the game or picked the menu entry is still down), and
 * not in the first 0.4 s. */
int game_movie_skip_pressed(void)
{
    static DWORD last, first;
    static int armed;
    DWORD now = GetTickCount();

    if (s_cut_mode != 0)
        return 0;
    if (now - last > 1500u) {          /* a new movie */
        armed = 0;
        first = now;
    }
    last = now;
    if (!cut_buttons_down()) {
        armed = 1;
        return 0;
    }
    if (!armed || now - first < 400u)
        return 0;
    armed = 0;
    fprintf(stderr, "[CUTSCENE] ended with a button\n");
    return 1;
}
