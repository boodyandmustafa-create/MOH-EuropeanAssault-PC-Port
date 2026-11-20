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
    {   /* where it was called from: the return address the call site pushed just before the check (esp is g_esp in generated code) */
        static int site_lines;
        uint32_t sp = g_esp;
        if (site_lines < 12 && sp >= 0x10000u && sp < 0x84000000u - 0x40u) {
            site_lines++;
            fprintf(stderr, "[ICALL]   site ret=%08X stack %08X %08X %08X %08X eax %08X\n",
                    (unsigned)*(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + sp),
                    (unsigned)*(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + sp + 4u),
                    (unsigned)*(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + sp + 8u),
                    (unsigned)*(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + sp + 12u),
                    (unsigned)*(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + sp + 16u),
                    (unsigned)g_eax);
        }
    }
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

static float s_zoom_cur;            /* SCOPE ZOOM, defined below */
static int scope_up(void);

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
    if (s_zoom_cur != 1.0f && scope_up() && fabsf(r->to - s_fovrole[0].to) < 1.0f)
        a1 = FOV_ATAN(FOV_TAN(a1) * s_zoom_cur);        /* SCOPE ZOOM */
    if (s_want_fov != 0.0f) {
        /* keep the zoom ratio against the 70 degree view: aiming and scopes widen with it */
        a1 = FOV_ATAN(FOV_TAN(a1) / FOV_TAN(70.0) * FOV_TAN(s_want_fov));
    }
    return a1;
}

/* The game's own world fov (degrees) of the last view update: about 70 normally, much smaller in a scope. */
float game_view_fov_deg(void) { return s_fovrole[0].to; }

/* SCOPE ZOOM (new in 0.9.1): while a scope is up (the game's world fov under 45 degrees) the mouse wheel / numpad + and -
 * change the zoom on top of the game's own scope. s_zoom_tgt multiplies tan(half angle): 1 = the scope as the game made it,
 * smaller = closer. Reset when the scope closes. The mouse turns slower in proportion (game_zoom_scale), so the picture
 * moves the same on screen. Costs nothing: a few float operations per frame. */
extern volatile LONG g_mkb_zoom_steps;
static float s_zoom_tgt = 1.0f, s_zoom_cur = 1.0f;
extern volatile int g_mkb_scoped;
int game_held_weapon_scoped(void);
/* A scope is up when the game's fov is under 60, or (a level's first scope, before any zoom: the game opens it at the iron
 * sights' 68 degrees) when Aim is held with a sniper in the hands and the fov has settled under 70. */
static int scope_up(void)
{
    float to = s_fovrole[0].to;
    extern volatile int g_mkb_aim_held;
    if (to > 1.0f && to < 60.0f)
        return 1;
    return g_mkb_aim_held && to >= 60.0f && to < 70.0f && game_held_weapon_scoped();
}
int game_scope_up(void) { return scope_up(); }
float game_zoom_scale(void) { return scope_up() ? s_zoom_cur : 1.0f; }
extern volatile unsigned g_mkb_last_btn;
static void zoom_frame(void)
{
    LONG st = InterlockedExchange(&g_mkb_zoom_steps, 0);
    g_mkb_scoped = scope_up();
    {   extern volatile int g_mkb_gren_held;        /* GRENADE MACRO: is a grenade in the hands (1) / a gun (0) / not known (-1) */
        extern volatile unsigned g_mkb_held_weapon; /* ... and which weapon object is held (0 = not known), to bring the gun back after the throw */
        int game_held_weapon_grenade(void);
        unsigned game_held_weapon_obj(void);
        g_mkb_gren_held = game_held_weapon_grenade();
        g_mkb_held_weapon = game_held_weapon_obj();
    }
    {   /* ZOOMPROBE: while a scope is up, every change of the game's own fov with the pad / key state at that moment */
        static float last;
        static unsigned n;
        float to = s_fovrole[0].to;
        if (to > 1.0f && to < 45.0f && fabsf(to - last) > 0.05f && n < 300) {
            n++;
            fprintf(stderr, "[ZOOMPROBE] game fov %.2f -> %.2f buttons %06X\n", (double)last, (double)to, g_mkb_last_btn);
        }
        last = to;
    }
    {   /* SCOPEDIAG: the four fov roles while the Aim button is held (what the game's fov is when a scope opens, before any zoom) */
        extern volatile int g_mkb_aim_held;
        static float lr[FOV_ROLES];
        static int was_aim;
        static unsigned n;
        int aim = g_mkb_aim_held, k, ch = aim != was_aim;
        for (k = 0; k < FOV_ROLES; k++)
            if (fabsf(s_fovrole[k].to - lr[k]) > 0.05f)
                ch = 1;
        if ((aim || was_aim) && ch && n < 400) {
            n++;
            fprintf(stderr, "[SCOPEDIAG] aim=%d scope_up=%d roles %.2f %.2f %.2f %.2f\n", aim, scope_up(),
                    (double)s_fovrole[0].to, (double)s_fovrole[1].to, (double)s_fovrole[2].to, (double)s_fovrole[3].to);
        }
        was_aim = aim;
        for (k = 0; k < FOV_ROLES; k++)
            lr[k] = s_fovrole[k].to;
    }
    if (!scope_up()) {
        s_zoom_tgt = s_zoom_cur = 1.0f;
        return;
    }
    if (st) {
        s_zoom_tgt *= (float)pow(st > 0 ? 0.8 : 1.25, (double)(st > 0 ? st : -st));
        s_zoom_tgt = s_zoom_tgt < 0.3f ? 0.3f : s_zoom_tgt > 1.6f ? 1.6f : s_zoom_tgt;
    }
    {
        float k = (float)(s_frame_ms / 90.0);       /* about a tenth of a second to settle */
        s_zoom_cur += (s_zoom_tgt - s_zoom_cur) * (k > 1.0f ? 1.0f : k);
    }
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
        zoom_frame();
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

/* ULTRAWIDE (RECOMP_ASPECT, nv2a_gpu.c): how much wider the player's view is than the game's own 16:9 one, for a view whose
 * aspect (tan h / tan v) the game made 16:9. 1 for everything else, and while a menu is up (the picture is then 16:9).
 * Also tells the presenters the shape of the picture of this frame. */
extern float recomp_wide_target(void);
extern void recomp_wide_frame(float asp);
extern void recomp_wide_set_gate(int (*gate)(void));
extern int recomp_menu_up(void);
static int wide_gate(void) { return !recomp_menu_up(); }
static float wide_view_factor(float game_aspect)
{
    static int reg;
    float t;
    if (!reg) {
        reg = 1;
        recomp_wide_set_gate(wide_gate);
    }
    t = recomp_wide_target();
    if (recomp_menu_up() || game_aspect < 1.70f || game_aspect > 1.85f) {
        recomp_wide_frame(0.0f);
        return 1.0f;
    }
    if (t <= 0.0f) {                            /* plain widescreen gameplay: still marked, so the HUD proportions option knows */
        recomp_wide_frame(game_aspect);
        return 1.0f;
    }
    recomp_wide_frame(t);
    return t / game_aspect;
}

/* WEAPON LIGHTING: 1 while the game thread is between the weapon pass's view updates (0xF463F .. 0xF46DD, see gun_sway_view). */
static int s_wp_in_pass;
extern void nv2a_gpu_weapon_view(float tan_h, float tan_v);
extern void nv2a_gpu_weapon_clean(void);
extern void nv2a_gpu_weapon_blood_color(float r, float g, float b);
extern void nv2a_gpu_weapon_aim(float f);
static void game_blood_main_rgb(float rgb[3]);             /* the Blood colour setting's main colour, 0..1 (defined with the colour code) */

void sub_0018B480_enter(void)
{
    float h, v, tanh_, tanv, deg, wide, s;
    FovRole *r;
    double now;
    fov_poll();
    draw_poll();
    h = guest_f(g_esp + 4);
    v = guest_f(g_esp + 8);
    if (s_wp_in_pass && h > 0.01f && h < 1.45f && v > 0.01f && v < 1.45f)
        nv2a_gpu_weapon_view(tanf(h), tanf(v));  /* the weapon lighting needs the weapon pass's own view cone (widened below when it is) */
    if (s_fov_mode != 2)
        return;
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
    {   /* ULTRAWIDE: only the horizontal half angle grows (the vertical view stays); the camera's aspect (+0xB8 / +0xBC) and
         * so the projection and every visibility test follow from these two angles */
        float k = wide_view_factor(tanh_ / tanv);
        guest_set_f(g_esp + 4, (float)atan(tanh_ * s * k));
        if (s_wp_in_pass)
            nv2a_gpu_weapon_view(tanh_ * s * k, tanv * s);
    }
    guest_set_f(g_esp + 8, (float)atan(tanv * s));
}

/* Every guest dword outside [skip_lo, skip_hi) that equals target: who holds a pointer to an object (low memory + the heap
 * window). Used by the corpse research (RECOMP_HOLDERS=1) to see which lists / slots a soldier's delete clears. Slow
 * (about 64 MB), so diagnostic only. */
int game_scan_holders(uint32_t target, uint32_t skip_lo, uint32_t skip_hi, uint32_t *out, int max)
{
    static const uint32_t regions[][2] = { {0x00000000u, 0x00800000u}, {0x80000000u, 0x84000000u} };
    int hits = 0, r;
    for (r = 0; r < 2; r++) {
        uint32_t va = regions[r][0];
        while (va < regions[r][1] && hits < max) {
            MEMORY_BASIC_INFORMATION mbi;
            uintptr_t host = (uintptr_t)va + g_xbox_mem_offset;
            uint32_t n;
            if (!VirtualQuery((void *)host, &mbi, sizeof mbi))
                break;
            n = (uint32_t)((uintptr_t)mbi.BaseAddress + mbi.RegionSize - host);
            if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && mbi.Protect != 0) {
                const uint32_t *p = (const uint32_t *)host;
                uint32_t off, cnt = n / 4;
                for (off = 0; off < cnt && va + off * 4 < regions[r][1] && hits < max; off++)
                    if (p[off] == target) {
                        uint32_t at = va + off * 4;
                        if (at < skip_lo || at >= skip_hi)
                            out[hits++] = at;
                    }
            }
            if (n == 0)
                break;
            va += n;
        }
    }
    return hits;
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

/* GUN SWAY: the game fixes the weapon to the camera, so it turns with the view like a rigid stick. The weapon is drawn in the
 * view update's third call of a frame (return 0xF463F, a narrower fov), and the world again after the fourth (0xF46DD). The
 * camera transform sits at params + 0xD0: a 3x3 whose COLUMNS are the camera's right / up / forward axes, in rows of four floats,
 * and a translation row (0x100) = -eye * rotation. Each frame (first call, return 0x80D67) the turn since the last frame is read
 * from that matrix; a spring-damper chases it, and for the weapon call only the camera is turned on by the spring's lag
 * (R' = R * exp(lag), t' = t * exp(lag)), so the gun swings behind the view and catches up with a little overshoot. The
 * transform is put back before the fourth call. Costs a few dozen float operations a frame.
 * RECOMP_GUN_SWAY=0/1 (ini gun_sway), RECOMP_GUN_SWAY_STRENGTH=1..5 (ini gun_sway_strength), RECOMP_GUN_SWAY_LOG=1. */
typedef struct {
    uint32_t view;
    int have, applied;
    float m[9], saved[16];
    float scale;                                /* strength this frame, kept for the weapon call */
    float lag[2], vel[2];                       /* camera-space lag of the weapon behind the view (rad): about the right axis, about the up axis */
    double t;
} GunSway;
static GunSway s_gs[2];
static int s_gs_on = -1, s_gs_str = 3, s_gs_log;

static void gs_cfg(void)
{
    const char *e;
    if (s_gs_on >= 0)
        return;
    e = getenv("RECOMP_GUN_SWAY");
    s_gs_on = !(e && e[0] == '0');
    e = getenv("RECOMP_GUN_SWAY_STRENGTH");
    s_gs_str = e && e[0] ? atoi(e) : 3;
    s_gs_str = s_gs_str < 1 ? 1 : s_gs_str > 5 ? 5 : s_gs_str;
    e = getenv("RECOMP_GUN_SWAY_LOG");
    s_gs_log = e && e[0] == '1';
}
int  game_gunsway_get(void)         { gs_cfg(); return s_gs_on; }
void game_gunsway_set(int on)       { gs_cfg(); s_gs_on = on ? 1 : 0; }
int  game_gunswaystr_get(void)      { gs_cfg(); return s_gs_str; }
void game_gunswaystr_set(int v)     { gs_cfg(); s_gs_str = v < 1 ? 1 : v > 5 ? 5 : v; }

/* RUN POSE: while the player sprints the weapon is carried across the body (muzzle turned aside and lowered, a little roll,
 * like Vanguard) and swings with the steps. game_runpose_tick() is called every frame from sprint_apply with "is sprinting";
 * the pose is blended in/out and added to the weapon pass of gun_sway_view as extra camera rotation (rx about the right axis,
 * ry about the up axis, rz about the forward axis; the weapon is turned by v' = v * E, so ry > 0 swings the muzzle left,
 * rx < 0 lowers it). Only rotations are used, so no world-unit guesses: the arc moves the gun because it sits off the camera.
 * RECOMP_RUN_POSE=0 off; RECOMP_RUN_POSE_TUNE="ry,rx,rz,bobx,boby,bobr" (radians) overrides the pose and swing amplitudes. */
static float s_rp_blend, s_rp_phase;
static float s_rp_pose[3] = { 0.60f, -0.12f, -0.38f };   /* ry, rx, rz at full blend */
static float s_rp_bob[3]  = { 0.030f, 0.020f, 0.050f };  /* lateral yaw swing, vertical pitch bounce, roll rock */
static int s_rp_on = -1;

void game_runpose_tick(int running, float dt)
{
    extern volatile int g_mkb_fire_held, g_mkb_aim_held;
    static float s_rp_hold;                     /* seconds the pose stays off after the last fire / aim, so it does not flicker */
    float target, tc;
    if (g_mkb_fire_held || g_mkb_aim_held || scope_up())
        s_rp_hold = 0.25f;                      /* firing or aiming: the gun must be at the muzzle the bullets leave from */
    else if (s_rp_hold > 0.0f)
        s_rp_hold -= dt;
    if (s_rp_hold > 0.0f)
        running = 0;
    target = running ? 1.0f : 0.0f;
    tc = running ? 0.12f : (s_rp_hold > 0.0f ? 0.05f : 0.18f);    /* fast back to normal on fire / aim */
    if (s_rp_on < 0) {
        const char *e = getenv("RECOMP_RUN_POSE"), *t = getenv("RECOMP_RUN_POSE_TUNE");
        float v[6];
        s_rp_on = !(e && e[0] == '0');
        if (t && sscanf(t, "%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
            memcpy(s_rp_pose, v, sizeof s_rp_pose);
            memcpy(s_rp_bob, v + 3, sizeof s_rp_bob);
        }
    }
    if (dt <= 0.0f || dt > 0.25f)
        dt = 0.0f;
    s_rp_blend += (target - s_rp_blend) * (1.0f - expf(-dt / tc));
    if (s_rp_blend < 0.001f)
        s_rp_blend = 0.0f;
    if (s_rp_blend > 0.02f) {
        s_rp_phase += dt * 6.2831853f * 1.5f;               /* 1.5 full lateral swings a second = 3 steps */
        if (s_rp_phase > 6.2831853f)
            s_rp_phase -= 6.2831853f;
    }
}

/* THIRD PERSON (research step 1): the eye is moved straight back along the camera's forward axis by s_tp_dist metres (the world is in
 * metres: player feet z 0.12, eye 1.6). The camera transform at params + 0xD0 is rebuilt by the game each frame, but if the first call of a
 * frame still finds exactly what was written last time it is not taken as the true camera. The original is kept and every call of the frame
 * gets it plus the offset (translation row z, index 14: view z = world . forward + t, so +d = farther from the eye = camera behind). The
 * weapon pass (0xF463F) gets the original pushed 1000 behind the camera so the first-person hands and gun are clipped away. The player has no
 * body object in single player (BODY-PROBE), so for now you see an empty spot where you stand. Gameplay cameras only (fov above 0.3 rad), first
 * view only (split-screen: player 1). Off by default: RECOMP_THIRD=1 (RECOMP_THIRD_DIST=m); with RECOMP_DEV_KEYS=1 Numpad * toggles,
 * Numpad + / - changes the distance by 0.5. */
static int s_tp_on = -1;
static float s_tp_dist = 3.0f;
static float s_tp_orig[16], s_tp_wr[16];
static int s_tp_have;

static void tp_cfg(void)
{
    const char *e;
    if (s_tp_on >= 0)
        return;
    e = getenv("RECOMP_THIRD");
    s_tp_on = e && e[0] == '1';
    e = getenv("RECOMP_THIRD_DIST");
    if (e && e[0] && atof(e) > 0.0)
        s_tp_dist = (float)atof(e);
}

/* GHOST BODY (nv2a_pb_exec.c ghost_draw): the game side only says where the player stands and which way the camera looks.
 * RECOMP_GHOST=0 off. Dev key Numpad / turns the body by 90 degrees (the model's forward axis is not known: press until it
 * faces the way you look; RECOMP_GHOST_K=0..3 starts there). */
typedef struct { int on; uint32_t frame; float pos[3]; float yaw; int yawk; float speed; float eyeh; int cycle; float eye[3]; int fp; float back; } NvGhost;
extern NvGhost g_nv_ghost;
extern uint32_t game_pc_get(void);
static int s_gh_on = -1;

static void tp_keys(void)
{
    static int dn[5];
    static const int vk[5] = { VK_MULTIPLY, VK_ADD, VK_SUBTRACT, VK_DIVIDE, VK_DECIMAL };
    int i;
    if (s_gh_on < 0) {
        const char *e = getenv("RECOMP_GHOST"), *k = getenv("RECOMP_GHOST_K");
        s_gh_on = e && e[0] == '1';             /* off unless asked: the ghost is still broken (research build) */
        g_nv_ghost.yawk = k && k[0] ? atoi(k) & 3 : -1;     /* -1: found from the model itself (nv2a_pb_exec.c gh_analyse) */
    }
    if (!game_dev_keys())
        return;
    for (i = 0; i < 5; i++) {
        int k = (GetAsyncKeyState(vk[i]) & 0x8000) != 0;
        if (k && !dn[i]) {
            if (i == 0)
                s_tp_on = !s_tp_on;
            else if (i == 3)
                g_nv_ghost.yawk = (g_nv_ghost.yawk + 1) & 3;
            else if (i == 4)
                g_nv_ghost.cycle++;                    /* the next soldier type */
            else
                s_tp_dist += i == 1 ? 0.5f : -0.5f;
            if (s_tp_dist < 0.5f)
                s_tp_dist = 0.5f;
            fprintf(stderr, "[THIRD] %s, distance %.1f m, body turn %d x 90 deg\n", s_tp_on ? "ON" : "off", (double)s_tp_dist, g_nv_ghost.yawk);
        }
        dn[i] = k;
    }
}

/* The player's feet (player object + 0x90), his ground speed over the last quarter second (a ring of position samples), and the
 * frame counter the renderer keys on. */
static void gh_track(uint32_t pc)
{
    static double st_t[64];
    static float st_x[64], st_y[64];
    static int st_n, st_i;
    double now = fov_now_ms();
    int k, found = -1;
    g_nv_ghost.pos[0] = guest_f(pc + 0x90u);
    g_nv_ghost.pos[1] = guest_f(pc + 0x94u);
    g_nv_ghost.pos[2] = guest_f(pc + 0x98u);
    st_t[st_i] = now; st_x[st_i] = g_nv_ghost.pos[0]; st_y[st_i] = g_nv_ghost.pos[1];
    st_i = (st_i + 1) & 63;
    if (st_n < 64) st_n++;
    for (k = 2; k <= st_n; k++) {               /* newest first: the first sample at least 250 ms old */
        int j = (st_i - k) & 63;
        if (now - st_t[j] >= 250.0) { found = j; break; }
    }
    if (found >= 0) {
        float dx = g_nv_ghost.pos[0] - st_x[found], dy = g_nv_ghost.pos[1] - st_y[found], v;
        v = sqrtf(dx * dx + dy * dy) / (float)((now - st_t[found]) * 0.001);
        g_nv_ghost.speed = v > 12.0f ? 12.0f : v;
    }
    g_nv_ghost.frame++;
}

/* FIRST-PERSON BODY (RECOMP_FP_BODY=1; RECOMP_FP_BACK=m, default 0.2): the player has no body in single player, so while the third-person
 * camera is off the renderer draws the cached soldier body (nv2a_pb_exec.c) at his feet, a little behind the eye, without head and arms,
 * so looking down shows a chest and legs. The camera is not touched. The heading comes from the camera's RIGHT axis (column 0: it stays
 * horizontal when he looks at the floor, where the flattened forward axis shrinks to nothing and flips); the offset between the two is
 * measured while he looks ahead. Gameplay camera only, first view only. */
static int s_fpb_on = -1;
static float s_fpb_back = 0.2f;

static void fpb_cfg(void)
{
    const char *e;
    if (s_fpb_on >= 0)
        return;
    e = getenv("RECOMP_FP_BODY");
    s_fpb_on = e && e[0] == '1';
    e = getenv("RECOMP_FP_BACK");
    if (e && e[0])
        s_fpb_back = (float)atof(e);
}

static void fp_body(uint32_t p)
{
    static float delta;
    static int have_delta;
    float m[16], hf, hr, eye[3];
    uint32_t pc = game_pc_get();
    int i;
    g_nv_ghost.on = 0;
    g_nv_ghost.fp = 0;
    if (guest_f(p + 0xC0) < 0.3f || pc < 0x80000000u)   /* not a gameplay camera */
        return;
    for (i = 0; i < 16; i++)
        m[i] = guest_f(p + 0xD0u + 4u * (uint32_t)i);
    hf = sqrtf(m[2] * m[2] + m[6] * m[6]);
    hr = atan2f(m[4], m[0]);
    if (hf > 0.8f) {                            /* looking ahead: the forward heading, and how it relates to the right axis */
        float d = atan2f(m[6], m[2]) - hr;
        while (d > 3.14159265f) d -= 6.2831853f;
        while (d < -3.14159265f) d += 6.2831853f;
        delta = d;
        have_delta = 1;
    }
    g_nv_ghost.on = 1;
    g_nv_ghost.fp = 1;
    g_nv_ghost.back = s_fpb_back;
    gh_track(pc);
    if (have_delta) {
        g_nv_ghost.yaw = hr + delta;
    } else if (hf > 0.05f) {
        g_nv_ghost.yaw = atan2f(m[6], m[2]);
    }
    for (i = 0; i < 3; i++)                     /* eye = -(translation row) against the rows of the rotation (as in third_view) */
        eye[i] = -(m[i * 4 + 0] * m[12] + m[i * 4 + 1] * m[13] + m[i * 4 + 2] * m[14]);
    memcpy(g_nv_ghost.eye, eye, sizeof eye);
    g_nv_ghost.eyeh = eye[2] - g_nv_ghost.pos[2];
}

static int third_view(uint32_t p, uint32_t ret)
{
    float cur[16], w[16];
    int i;
    if (guest_f(p + 0xC0) < 0.3f) {             /* not a gameplay camera */
        g_nv_ghost.on = 0;
        return 0;
    }
    for (i = 0; i < 16; i++)
        cur[i] = guest_f(p + 0xD0u + 4u * (uint32_t)i);
    if (ret == 0x80D67u) {
        uint32_t pc = game_pc_get();
        if (!(s_tp_have && memcmp(cur, s_tp_wr, sizeof cur) == 0))
            memcpy(s_tp_orig, cur, sizeof cur);
        s_tp_have = 1;
        g_nv_ghost.on = s_gh_on > 0 && pc >= 0x80000000u;
        g_nv_ghost.fp = 0;
        if (g_nv_ghost.on) {                    /* feet at player + 0x90; heading = the camera's forward axis (column 2) flattened */
            gh_track(pc);
            g_nv_ghost.yaw = atan2f(s_tp_orig[6], s_tp_orig[2]);
            g_nv_ghost.eyeh = -(s_tp_orig[8] * s_tp_orig[12] + s_tp_orig[9] * s_tp_orig[13] + s_tp_orig[10] * s_tp_orig[14]) - g_nv_ghost.pos[2];
        }
    } else if (!s_tp_have)
        return 0;
    memcpy(w, s_tp_orig, sizeof w);
    w[14] += ret == 0xF463Fu ? -1000.0f : s_tp_dist;
    for (i = 12; i < 16; i++)
        guest_set_f(p + 0xD0u + 4u * (uint32_t)i, w[i]);
    if (ret == 0x80D67u && g_nv_ghost.on) {     /* the camera's eye after the pull-back: eye = -M * t (rows of M against the translation row) */
        g_nv_ghost.eye[0] = -(cur[0] * w[12] + cur[1] * w[13] + cur[2] * w[14]);
        g_nv_ghost.eye[1] = -(cur[4] * w[12] + cur[5] * w[13] + cur[6] * w[14]);
        g_nv_ghost.eye[2] = -(cur[8] * w[12] + cur[9] * w[13] + cur[10] * w[14]);
    }
    memcpy(s_tp_wr, cur, sizeof cur);           /* what the matrix holds now: the rotation as it was, the translation row replaced */
    for (i = 12; i < 16; i++)
        s_tp_wr[i] = w[i];
    return 1;
}

/* FREECAM: a camera that flies free of the player. Key (default Insert; RECOMP_FREECAM_KEY = a virtual-key number) or the F1 row
 * "Freecam" toggles it. The camera transform at params + 0xD0 (see gun_sway_view) is written in full on every view-update call: the
 * eye, yaw and pitch are kept here. The weapon pass gets the camera 1000 behind so the first-person gun is not drawn. Mouse (taken from
 * the look hook, game_patches.c aim_inject_yaw, and here once a frame) and the right stick turn; WASD / the left stick fly along the view;
 * Space or the right trigger up, C or the left trigger down, Shift or the left-stick click fast, Ctrl slow. The input filter
 * (g_xbox_input_filter) empties the pad the game sees while it is on, except Start and Back, so the soldier stands still and the
 * pause menu still works. World frozen (default): the frame time handed to the game is scaled down to almost nothing
 * (game_patch_000137C0), so enemies, physics and scripts stand still; world running: only the camera moves.
 * ini freecam_world (0 frozen, 1 running), freecam_speed (1..5). RECOMP_FREECAM_LOG=1 logs the toggles. */
typedef struct { DWORD pkt; WORD btn; BYTE an[8]; SHORT lx, ly, rx, ry; } FcPad;   /* XBOX_INPUT_STATE (xinput_xbox.h) */
extern void (*g_xbox_input_filter)(void *pState);
extern volatile long g_input_block;
int mkb_native_aim_take(float *yaw, float *pitch);

static int s_fc_on;                             /* flying now */
static int s_fc_want;                           /* asked for (key or menu) */
static int s_fc_cfgd, s_fc_world, s_fc_speed = 3, s_fc_log;
static int s_fc_key = VK_INSERT;
static int s_fc_pos_ok;                         /* params + 0x60 is the eye position: write it too */
static int s_fc_hand = 1;                      /* +1 right-handed (right x up = forward), -1 mirrored */
static uint32_t s_fc_main;                      /* the view that may fly (first player's) */
static double s_fc_main_ms;                     /* last time it was seen */
static uint32_t s_fc_view0, s_fc_seen;         /* the view flown from, and the last game frame it was seen */
static float s_fc_eye[3], s_fc_yaw, s_fc_pitch, s_fc_mdy, s_fc_mdp;
static volatile int s_fc_lx, s_fc_ly, s_fc_rx, s_fc_ry, s_fc_up, s_fc_dn, s_fc_fast;
static double s_fc_t;

static void fc_filter(void *ps)
{
    FcPad *s = (FcPad *)ps;
    if (!s_fc_on || GUEST8(0x378964) == 1)      /* not flying, or a menu is up (the pause menu keeps its pad) */
        return;
    s_fc_lx = s->lx;
    s_fc_ly = s->ly;
    s_fc_rx = s->rx;
    s_fc_ry = s->ry;
    s_fc_up = s->an[7] > 100;
    s_fc_dn = s->an[6] > 100;
    s_fc_fast = (s->btn & 0x0040) != 0;
    s->btn &= 0x0030;                           /* Start and Back stay */
    memset(s->an, 0, sizeof s->an);
    s->lx = s->ly = s->rx = s->ry = 0;
}

static void fc_cfg(void)
{
    const char *e;
    if (s_fc_cfgd)
        return;
    s_fc_cfgd = 1;
    e = getenv("RECOMP_FREECAM_WORLD");
    s_fc_world = e && e[0] == '1';
    e = getenv("RECOMP_FREECAM_SPEED");
    if (e && e[0])
        s_fc_speed = atoi(e) < 1 ? 1 : atoi(e) > 5 ? 5 : atoi(e);
    e = getenv("RECOMP_FREECAM_KEY");
    if (e && e[0] && atoi(e) > 0 && atoi(e) < 256)
        s_fc_key = atoi(e);
    e = getenv("RECOMP_FREECAM_LOG");
    s_fc_log = e && e[0] == '1';
    g_xbox_input_filter = fc_filter;
}

int  game_freecam_get(void)         { fc_cfg(); return s_fc_want; }
void game_freecam_set(int on)       { fc_cfg(); s_fc_want = on ? 1 : 0; }
int  game_freecamworld_get(void)    { fc_cfg(); return s_fc_world; }
void game_freecamworld_set(int v)   { fc_cfg(); s_fc_world = v ? 1 : 0; }
int  game_freecamspeed_get(void)    { fc_cfg(); return s_fc_speed; }
void game_freecamspeed_set(int v)   { fc_cfg(); s_fc_speed = v < 1 ? 1 : v > 5 ? 5 : v; }
int  game_freecam_active(void)      { return s_fc_on; }
int  game_freecam_frozen(void)      { return s_fc_on && !s_fc_world; }
void game_freecam_mouse(float dyaw, float dpitch) { s_fc_mdy += dyaw; s_fc_mdp += dpitch; }

/* A level starts loading (open of loading.viv / level_a.str, from rs_open_hook): end the flight, or the frozen world and the
 * blocked pad carry over into the next level. The view object can be the same one, so no view test can see this. */
void game_freecam_levelload(void)
{
    nv2a_gpu_weapon_clean();                    /* a new level: a clean weapon */
    s_fc_main = 0;                              /* the next level has its own view object */
    if (s_fc_on || s_fc_want) {
        s_fc_on = 0;
        s_fc_want = 0;
        if (s_fc_log)
            fprintf(stderr, "[FREECAM] off (level load)\n");
    }
}

static int fc_fg(void)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static float fc_dz(int v)                       /* a stick value, -1..1 with a dead zone */
{
    float f = (float)v / 32767.0f;
    if (f > -0.15f && f < 0.15f)
        return 0.0f;
    return f > 1.0f ? 1.0f : f < -1.0f ? -1.0f : f;
}

static int fc_view(uint32_t view, uint32_t p, uint32_t ret)
{
    static int keydn;
    float m[16], f[3], r[3], u[3], cp;
    int i;
    fc_cfg();
    if (s_fc_on) {  /* a level change (quit from the pause menu, next mission) must end the flight: the old view object is gone, so
                     * this function would never see it again and the world would stay frozen with the pad blocked */
        if (view == s_fc_view0) {
            s_fc_seen = g_game_frames;
        } else if (g_game_frames < s_fc_seen || g_game_frames - s_fc_seen > 30u) {
            s_fc_on = 0;
            s_fc_want = 0;
            if (s_fc_log)
                fprintf(stderr, "[FREECAM] off (level changed, old view %08X now %08X)\n", s_fc_view0, view);
        }
    }
    if (view != s_fc_main)                      /* only the main (first player's) view flies, see fc_track_main */
        return 0;
    if (ret == 0x80D67u) {
        int k = fc_fg() && (GetAsyncKeyState(s_fc_key) & 0x8000) != 0;
        {   /* IN-GAME EDITOR (game_patches.c): one call a frame; returns at once unless RECOMP_EDITOR_LOG=1 */
            extern void game_editor_tick(const float *eye, const float *fwd, int fc_on);
            extern void game_editor_view(const float *right, const float *up, float px, float py);
            float fwd[3], cpe = cosf(s_fc_pitch), er[3], eu[3];
            fwd[0] = cpe * cosf(s_fc_yaw);
            fwd[1] = cpe * sinf(s_fc_yaw);
            fwd[2] = sinf(s_fc_pitch);
            er[0] = -(float)s_fc_hand * sinf(s_fc_yaw);             /* the camera basis as written to the view below */
            er[1] = (float)s_fc_hand * cosf(s_fc_yaw);
            er[2] = 0.0f;
            eu[0] = (float)s_fc_hand * (fwd[1] * er[2] - fwd[2] * er[1]);
            eu[1] = (float)s_fc_hand * (fwd[2] * er[0] - fwd[0] * er[2]);
            eu[2] = (float)s_fc_hand * (fwd[0] * er[1] - fwd[1] * er[0]);
            if (s_fc_on) {                      /* the mouse ray of the editor window: 1 / tan of the half angles */
                float tH = guest_f(p + 0xB8u), tV = guest_f(p + 0xBCu), m0 = guest_f(p + 0x110u), m5 = guest_f(p + 0x110u + 20u), px, py;
                static unsigned nlog;
                if (tH > 0.1f && tH < 10.0f && tV > 0.1f && tV < 10.0f) {
                    px = 1.0f / tH;
                    py = 1.0f / tV;
                } else if (m0 > 0.0f && m5 > 0.0f) {    /* the matrix at +0x110 held a scaled copy in the log: only its x : y ratio is believed (vertical view 70 degrees) */
                    py = 1.0f / tanf(35.0f * 3.14159265f / 180.0f);
                    px = py * (m0 / m5);
                } else {
                    px = m0;
                    py = m5;
                }
                if (s_fc_log && nlog < 6 && (++nlog)) {
                    fprintf(stderr, "[FREECAM] editor view: tan half angles %.4f %.4f, matrix %.3f %.3f -> proj %.3f %.3f\n", (double)tH, (double)tV, (double)m0, (double)m5, (double)px, (double)py);
                }
                game_editor_view(er, eu, px, py);
            }
            game_editor_tick(s_fc_eye, fwd, s_fc_on);
        }
        int play = guest_f(p + 0xC0) >= 0.3f && game_pc_get() >= 0x80000000u;
        if (k && !keydn)
            s_fc_want = !s_fc_want;
        keydn = k;
        if (s_fc_on && !play)
            s_fc_want = 0;                      /* a cut scene or a level change ends it */
        if (s_fc_want && !s_fc_on && play && GUEST8(0x378964) != 1) {
            float fw[3];
            for (i = 0; i < 16; i++)
                m[i] = guest_f(p + 0xD0u + 4u * (uint32_t)i);
            for (i = 0; i < 3; i++) {
                s_fc_eye[i] = -(m[i * 4 + 0] * m[12] + m[i * 4 + 1] * m[13] + m[i * 4 + 2] * m[14]);
                fw[i] = m[i * 4 + 2];
            }
            {   /* handedness: (right x up) . forward */
                float cx = m[4] * m[9] - m[8] * m[5], cy = m[8] * m[1] - m[0] * m[9], cz = m[0] * m[5] - m[4] * m[1];
                s_fc_hand = cx * fw[0] + cy * fw[1] + cz * fw[2] >= 0.0f ? 1 : -1;
            }
            {   /* the game's own culling and LOD read the eye position at params + 0x60 (sub_00130010: distance from it, not from the
                 * matrix): if it holds the eye now, it is moved with the camera, or things far from the soldier vanish */
                float dx = guest_f(p + 0x60) - s_fc_eye[0], dy = guest_f(p + 0x64) - s_fc_eye[1], dz = guest_f(p + 0x68) - s_fc_eye[2];
                s_fc_pos_ok = dx * dx + dy * dy + dz * dz < 0.25f;
                if (s_fc_log)
                    fprintf(stderr, "[FREECAM] params+0x60 %.2f %.2f %.2f vs eye %.2f %.2f %.2f -> %s\n", (double)guest_f(p + 0x60),
                            (double)guest_f(p + 0x64), (double)guest_f(p + 0x68), (double)s_fc_eye[0], (double)s_fc_eye[1],
                            (double)s_fc_eye[2], s_fc_pos_ok ? "moved with the camera" : "left alone");
            }
            s_fc_yaw = atan2f(fw[1], fw[0]);
            s_fc_pitch = asinf(fw[2] > 1.0f ? 1.0f : fw[2] < -1.0f ? -1.0f : fw[2]);
            s_fc_mdy = s_fc_mdp = 0.0f;
            s_fc_t = fov_now_ms();
            s_fc_view0 = view;
            s_fc_seen = g_game_frames;
            s_fc_on = 1;
            if (s_fc_log)
                fprintf(stderr, "[FREECAM] ON eye %.2f %.2f %.2f yaw %.3f pitch %.3f hand %d world %s\n", (double)s_fc_eye[0],
                        (double)s_fc_eye[1], (double)s_fc_eye[2], (double)s_fc_yaw, (double)s_fc_pitch, s_fc_hand,
                        s_fc_world ? "running" : "frozen");
        } else if (!s_fc_want && s_fc_on) {
            s_fc_on = 0;
            if (s_fc_log)
                fprintf(stderr, "[FREECAM] off\n");
        } else if (s_fc_want && !s_fc_on && !play) {
            s_fc_want = 0;
        }
        if (!s_fc_on)
            return 0;
        {   /* once a frame: turn and fly */
            double now = fov_now_ms(), dt = (now - s_fc_t) * 0.001;
            float my = 0.0f, mp = 0.0f, tx, ty, sp, mvf, mvr, mvu;
            static const float k_speed[5] = { 2.0f, 4.0f, 7.0f, 12.0f, 20.0f };
            s_fc_t = now;
            if (dt < 0.0f || dt > 0.1)
                dt = 0.1;
            if (!g_input_block && GUEST8(0x378964) != 1) {
                if (mkb_native_aim_take(&my, &mp)) {    /* always called: it keeps the mouse deltas off the stick */
                    extern int game_editor_look_ok(void);
                    if (game_editor_look_ok()) {        /* the editor window frees the pointer: the mouse turns the camera only while the right button is held */
                        s_fc_mdy += my;
                        s_fc_mdp += mp;
                    }
                }
                tx = s_fc_mdy + fc_dz(s_fc_rx) * 2.4f * (float)dt;
                ty = s_fc_mdp + fc_dz(s_fc_ry) * 1.8f * (float)dt;
                s_fc_mdy = s_fc_mdp = 0.0f;
                s_fc_yaw += (float)s_fc_hand * tx;
                s_fc_pitch += ty;
                if (s_fc_pitch > 1.5f) s_fc_pitch = 1.5f;
                if (s_fc_pitch < -1.5f) s_fc_pitch = -1.5f;
                sp = k_speed[s_fc_speed - 1];
                if (fc_fg()) {
                    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) || s_fc_fast) sp *= 4.0f;
                    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) sp *= 0.25f;
                    mvu = ((GetAsyncKeyState(VK_SPACE) & 0x8000) ? 1.0f : 0.0f) - ((GetAsyncKeyState('C') & 0x8000) ? 1.0f : 0.0f);
                } else {
                    mvu = 0.0f;
                }
                {
                    extern int game_editor_window_up(void);
                    if (!game_editor_window_up())       /* the editor window uses the mouse buttons: they (the pad triggers they press) do not fly up / down then */
                        mvu += (s_fc_up ? 1.0f : 0.0f) - (s_fc_dn ? 1.0f : 0.0f);
                }
                mvf = fc_dz(s_fc_ly);
                mvr = fc_dz(s_fc_lx);
                {
                    float cpp = cosf(s_fc_pitch), fx = cpp * cosf(s_fc_yaw), fy = cpp * sinf(s_fc_yaw), fz = sinf(s_fc_pitch);
                    float rx = -(float)s_fc_hand * sinf(s_fc_yaw), ry = (float)s_fc_hand * cosf(s_fc_yaw);
                    float step = sp * (float)dt;
                    s_fc_eye[0] += (fx * mvf + rx * mvr) * step;
                    s_fc_eye[1] += (fy * mvf + ry * mvr) * step;
                    s_fc_eye[2] += (fz * mvf + mvu) * step;
                }
            }
        }
    }
    if (!s_fc_on)
        return 0;
    cp = cosf(s_fc_pitch);
    f[0] = cp * cosf(s_fc_yaw);
    f[1] = cp * sinf(s_fc_yaw);
    f[2] = sinf(s_fc_pitch);
    r[0] = -(float)s_fc_hand * sinf(s_fc_yaw);      /* right = hand * (Z x forward), normalised */
    r[1] = (float)s_fc_hand * cosf(s_fc_yaw);
    r[2] = 0.0f;
    u[0] = (float)s_fc_hand * (f[1] * r[2] - f[2] * r[1]);          /* up = hand * (forward x right) */
    u[1] = (float)s_fc_hand * (f[2] * r[0] - f[0] * r[2]);
    u[2] = (float)s_fc_hand * (f[0] * r[1] - f[1] * r[0]);
    for (i = 0; i < 3; i++) {
        guest_set_f(p + 0xD0u + 16u * (uint32_t)i + 0u, r[i]);
        guest_set_f(p + 0xD0u + 16u * (uint32_t)i + 4u, u[i]);
        guest_set_f(p + 0xD0u + 16u * (uint32_t)i + 8u, f[i]);
    }
    if (s_fc_pos_ok) {
        guest_set_f(p + 0x60u, s_fc_eye[0]);
        guest_set_f(p + 0x64u, s_fc_eye[1]);
        guest_set_f(p + 0x68u, s_fc_eye[2]);
    }
    guest_set_f(p + 0x100u, -(s_fc_eye[0] * r[0] + s_fc_eye[1] * r[1] + s_fc_eye[2] * r[2]));
    guest_set_f(p + 0x104u, -(s_fc_eye[0] * u[0] + s_fc_eye[1] * u[1] + s_fc_eye[2] * u[2]));
    guest_set_f(p + 0x108u, -(s_fc_eye[0] * f[0] + s_fc_eye[1] * f[1] + s_fc_eye[2] * f[2]) - (ret == 0xF463Fu ? 1000.0f : 0.0f));
    {   /* cached view*projection (p+0x150 = p+0xD0 x p+0x110, same math as sub_00014560): the
           game only rebuilds it when byte p+0x193 is 0, so refresh it ourselves, else the cull
           and sort code keep using the soldier's own camera */
        float A[16], B[16];
        int a, b, k;
        for (i = 0; i < 16; i++) {
            A[i] = guest_f(p + 0xD0u + 4u * (uint32_t)i);
            B[i] = guest_f(p + 0x110u + 4u * (uint32_t)i);
        }
        for (a = 0; a < 4; a++)
            for (b = 0; b < 4; b++) {
                float s = 0.0f;
                for (k = 0; k < 4; k++)
                    s += A[a * 4 + k] * B[k * 4 + b];
                guest_set_f(p + 0x150u + 4u * (uint32_t)(a * 4 + b), s);
            }
    }
    return 1;
}

/* Called by hand from sub_0018CE80 (gen/recomp_0045.c, loc_0018CE96), the game's camera setter: it copies the camera block at
 * params + 0x30..0x6F (rows right / forward / up / position), applies the shake, hands the copy to the renderer (vtable + 0xEC,
 * frustum planes) and rebuilds the view matrix at +0xD0 from it (sub_0018B540: +0xD0 row k = (row0[k], row2[k], row1[k]),
 * translation = -(pos . row0, pos . row2, pos . row1)). Writing the free camera into the block itself makes all of those agree. */
void game_fc_setter(uint32_t p)
{
    static unsigned n;
    float cp, f[3], r[3], u[3];
    int i;
    if (!s_fc_on || !p)
        return;
    n++;
    if (s_fc_log && (n % 120u) == 1u)
        fprintf(stderr, "[FREECAM] camera setter hit %u (eye %.1f %.1f %.1f)\n", n, (double)s_fc_eye[0], (double)s_fc_eye[1], (double)s_fc_eye[2]);
    cp = cosf(s_fc_pitch);
    f[0] = cp * cosf(s_fc_yaw);
    f[1] = cp * sinf(s_fc_yaw);
    f[2] = sinf(s_fc_pitch);
    r[0] = -(float)s_fc_hand * sinf(s_fc_yaw);
    r[1] = (float)s_fc_hand * cosf(s_fc_yaw);
    r[2] = 0.0f;
    u[0] = (float)s_fc_hand * (f[1] * r[2] - f[2] * r[1]);
    u[1] = (float)s_fc_hand * (f[2] * r[0] - f[0] * r[2]);
    u[2] = (float)s_fc_hand * (f[0] * r[1] - f[1] * r[0]);
    for (i = 0; i < 3; i++) {
        guest_set_f(p + 0x30u + 4u * (uint32_t)i, r[i]);
        guest_set_f(p + 0x40u + 4u * (uint32_t)i, f[i]);
        guest_set_f(p + 0x50u + 4u * (uint32_t)i, u[i]);
        guest_set_f(p + 0x60u + 4u * (uint32_t)i, s_fc_eye[i]);
    }
}

/* The main (first player's) view: the first one seen, and a new one once the old one has been silent for 1.5 s (a level loaded:
 * every level has its own view object). game_freecam_levelload clears it too. Returns 1 for the main view. */
static int fc_track_main(uint32_t view)
{
    double nowms = fov_now_ms();
    if (!s_fc_main || (view != s_fc_main && nowms - s_fc_main_ms > 1500.0))
        s_fc_main = view;
    if (view != s_fc_main)
        return 0;
    s_fc_main_ms = nowms;
    return 1;
}

static double s_gs_seen_ms[2];                  /* when each gun-sway slot's view was last seen */

/* WEAPON LIGHTING level (0 Off, 1 Low, 2 Normal, 3 High; nv2a_gpu.c does the work): ini weapon_light, env RECOMP_WEAPON_LIGHT. Read once;
 * the setting row changes it live. */
extern int nv2a_gpu_weapon_level(void);
extern void nv2a_gpu_weapon_config(int level);
static int s_wl_init;
static void wl_cfg(void)
{
    const char *e;
    if (s_wl_init)
        return;
    s_wl_init = 1;
    e = getenv("RECOMP_WEAPON_LIGHT");
    nv2a_gpu_weapon_config(e && e[0] ? atoi(e) : 2);
}
int  game_weaponlight_get(void)     { wl_cfg(); return nv2a_gpu_weapon_level(); }
void game_weaponlight_set(int v)    { wl_cfg(); nv2a_gpu_weapon_config(v); }

/* WEAPON LIGHTING: a begin / end marker in the GPU command stream around the weapon pass, so the executor (which runs the commands
 * a little later, on its own thread) knows which draws are the first-person weapon's. The marker is what D3D pushes for its own
 * software methods (sub_002DE0C0 writes 0x40100, 5: NO_OPERATION with one parameter) with a magic value instead. It is written the
 * way D3D writes a push (sub_002DA350): at the device's put pointer [dev], only while it is below the threshold [dev+4], then [dev]
 * moves on. No room: skipped (the draws of that frame are lit as ordinary ones) and counted. The end marker is always written when a
 * begin was. RECOMP_WPN_LOG=1 prints the counts once a second. */
static int s_wm_open;
static unsigned s_wm_sent, s_wm_skipped, s_wm_made;       /* made = times room was made first (sub_002DE370) */
extern void sub_002DE370(void);
extern __declspec(thread) uint32_t g_edx, g_ebx, g_esi, g_edi, g_ebp;
static unsigned s_wm_skip_be[2];                   /* skipped begins / ends */
static int s_wm_last_gap, s_wm_last_end;           /* [dev+4] - put and [dev+0x28] - put at the last skip */
static void wpn_mark(int on)
{
    uint32_t dev, put, thr;
    static double last;
    static int log_on = -1;
    s_wp_in_pass = on;
    if (on && nv2a_gpu_weapon_level() > 0) {   /* always on, once per 10 s: begin calls reached vs markers written */
        static unsigned calls;
        static double last10;
        double t10 = fov_now_ms();
        calls++;
        if (t10 - last10 > 10000.0) {
            last10 = t10;
            fprintf(stderr, "[WPNMARK] 10 s: begin calls %u sent %u skipped %u made-room %u\n", calls, s_wm_sent, s_wm_skipped, s_wm_made);
        }
    }
    if (log_on < 0) {
        const char *e = getenv("RECOMP_WPN_LOG");
        log_on = e && e[0] == '1';
    }
    if (on ? nv2a_gpu_weapon_level() <= 0 : !s_wm_open)
        return;
    if (on) {                                   /* the blood colour follows the setting; aiming down the sights moves the gun to the middle (the light's shading follows) */
        float c[3], f = (1.0f - game_view_fov_deg() / 70.0f) / 0.3f;
        nv2a_gpu_weapon_aim(f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f);
        game_blood_main_rgb(c);
        nv2a_gpu_weapon_blood_color(c[0], c[1], c[2]);
    }
    dev = GUEST32(GAME_D3D_DEVICE_PTR);
    if (!dev || (dev & 3u))
        goto done;
    put = GUEST32(dev);
    thr = GUEST32(dev + 4u);
    if (!(thr > put)) {
        /* D3D's own rule (sub_002DA350): write while put < threshold (16 bytes there, 8 here), and only when put has reached it call
         * sub_002DE370, which returns the new put pointer in eax. A skipped begin leaves this frame's weapon unlit (a flash), a lost end
         * leaves the pass open. The game's registers and stack pointer are kept, as this runs in front of sub_0018E150. */
        uint32_t sv[8] = { g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_ebp, g_esp }, np;
        g_esp -= 4u;
        GUEST32(g_esp) = 0x002DA399u;
        sub_002DE370();
        np = g_eax;
        g_eax = sv[0]; g_ecx = sv[1]; g_edx = sv[2]; g_ebx = sv[3]; g_esi = sv[4]; g_edi = sv[5]; g_ebp = sv[6]; g_esp = sv[7];
        s_wm_made++;
        if (np >= 0x80000000u && np < 0x84000000u && !(np & 3u)) {
            put = np;
            thr = GUEST32(dev + 4u);
        }
    }
    if (!(put >= 0x80000000u && put < 0x84000000u && !(put & 3u)) || !(thr > put)) {   /* the push buffer's ring runs from 0x80005000 up: a bound of 0x80010000 skipped ~4% of the markers (the blink) */
        s_wm_skipped++;
        if (on)
            s_wm_open = 0;
        s_wm_skip_be[on ? 0 : 1]++;
        s_wm_last_gap = (int)(thr - put);
        s_wm_last_end = (int)(GUEST32(dev + 0x28u) - put);
        if (s_wm_skipped <= 40u)                /* always on: the blink hunt (a skipped marker = an unlit frame) */
            fprintf(stderr, "[WPNMARK] SKIP %s #%u: put %08x thr %08x end %08x made-room %u\n", on ? "begin" : "end", s_wm_skipped, put, thr,
                    GUEST32(dev + 0x28u), s_wm_made);
        goto done;
    }
    GUEST32(put) = 0x00040100u;
    GUEST32(put + 4u) = on ? 0x57504C12u : 0x57504C10u;
    GUEST32(dev) = put + 8u;
    s_wm_open = on;
    s_wm_sent++;
done:
    if (log_on) {
        double t = fov_now_ms();
        if (t - last > 1000.0) {
            last = t;
            fprintf(stderr, "[WPNMARK] game side: sent %u made-room %u skipped %u (begin %u end %u; last skip thr-put %d, [dev+0x28]-put %d) level %d\n",
                    s_wm_sent, s_wm_made, s_wm_skipped, s_wm_skip_be[0], s_wm_skip_be[1], s_wm_last_gap, s_wm_last_end, nv2a_gpu_weapon_level());
        }
    }
}

static void gun_sway_view(uint32_t view)
{
    uint32_t ret, p;
    GunSway *g = NULL;
    int i, is_main;
    gs_cfg();
    tp_cfg();
    fpb_cfg();
    fc_cfg();
    wl_cfg();
    if (!view)
        return;
    ret = GUEST32(g_esp);
    if (ret != 0x80D67u && ret != 0xBEAC5u && ret != 0xF463Fu && ret != 0xF46DDu)
        return;
    if (ret == 0xF463Fu || ret == 0xF46DDu)    /* before any of the early returns below (freecam, third person): an end must follow a begin */
        wpn_mark(ret == 0xF463Fu);
    p = GUEST32(view + 0xE4);
    if (!p)
        return;
    is_main = fc_track_main(view);
    if (ret == 0x80D67u)
        tp_keys();
    if (s_tp_on <= 0 && fc_view(view, p, ret))
        return;                                 /* flying: the gun sway and the run pose do not apply */
    if (s_tp_on <= 0) {
        if (s_fpb_on > 0) {                     /* first-person body: set once a frame, kept for the other calls of the frame */
            if (ret == 0x80D67u && is_main)
                fp_body(p);
        } else
            g_nv_ghost.on = 0;
    }
    if (s_tp_on > 0 && is_main) {
        if (third_view(p, ret))
            return;                             /* the weapon sway and run pose belong to the first-person gun */
    }
    if (ret == 0xBEAC5u)
        return;
    {
        double nowms = fov_now_ms();
        for (i = 0; i < 2; i++)                 /* a slot whose view is gone (an earlier level) is free again */
            if (s_gs[i].view && s_gs[i].view != view && nowms - s_gs_seen_ms[i] > 1500.0)
                memset(&s_gs[i], 0, sizeof s_gs[i]);
        for (i = 0; i < 2; i++)
            if (s_gs[i].view == view) { g = &s_gs[i]; break; }
        if (!g) {
            for (i = 0; i < 2; i++)
                if (!s_gs[i].view) { g = &s_gs[i]; g->view = view; break; }
            if (!g)
                return;
        }
        s_gs_seen_ms[g - s_gs] = nowms;
    }
    if (ret == 0xF46DDu) {                      /* the weapon pass is over: the true camera back */
        if (g->applied) {
            for (i = 0; i < 16; i++)
                guest_set_f(p + 0xD0u + 4u * (uint32_t)i, g->saved[i]);
            g->applied = 0;
        }
        return;
    }
    if (guest_f(p + 0xC0) < 0.3f)               /* not a gameplay camera (menus, cut scenes) */
        return;
    if (ret == 0x80D67u) {                      /* once a frame: how far the camera turned since the last one */
        float cur[9], rel[9], px, py;
        double now = fov_now_ms(), dt = (now - g->t) * 0.001;
        float w, k, d, a, scale;
        int r, c, kk;
        for (r = 0; r < 3; r++)
            for (c = 0; c < 3; c++)
                cur[r * 3 + c] = guest_f(p + 0xD0u + 16u * (uint32_t)r + 4u * (uint32_t)c);
        g->t = now;
        if (dt < 0.0005 || dt > 0.25)
            g->have = 0;                        /* first frame after a pause or a cut */
        if (!g->have || !s_gs_on) {
            memcpy(g->m, cur, sizeof cur);
            g->have = 1;
            g->lag[0] = g->lag[1] = g->vel[0] = g->vel[1] = 0.0f;
            return;
        }
        for (r = 0; r < 3; r++)                 /* rel = prev^T * cur: the turn in the camera's own axes */
            for (c = 0; c < 3; c++) {
                float s = 0.0f;
                for (kk = 0; kk < 3; kk++)
                    s += g->m[kk * 3 + r] * cur[kk * 3 + c];
                rel[r * 3 + c] = s;
            }
        memcpy(g->m, cur, sizeof cur);
        px = 0.5f * (rel[7] - rel[5]);          /* about the right axis (pitch) */
        py = 0.5f * (rel[2] - rel[6]);          /* about the up axis (yaw) */
        if (fabsf(px) > 0.6f || fabsf(py) > 0.6f) {
            g->lag[0] = g->lag[1] = g->vel[0] = g->vel[1] = 0.0f;
            return;                             /* a jump (respawn, cut): no swing */
        }
        /* heavier = lower spring frequency; a bit under critical damping so the gun settles with a small overshoot */
        w = 19.0f - 2.7f * (float)s_gs_str;     /* 16.3 .. 5.5 rad/s */
        k = w * w;
        d = 2.0f * 0.62f * w;
        if (dt > 0.05)
            dt = 0.05;
        g->lag[0] += px;
        g->lag[1] += py;
        for (i = 0; i < 2; i++) {               /* the weapon follows the view like a mass on a spring */
            a = k * g->lag[i] - d * g->vel[i];
            g->vel[i] += a * (float)dt;
            g->lag[i] -= g->vel[i] * (float)dt;
        }
        scale = 0.30f + 0.10f * (float)s_gs_str;
        {   /* aiming down the sights (the game's own fov falls from 70) steadies the gun */
            float f = game_view_fov_deg() / 70.0f;
            scale *= f < 0.3f ? 0.3f : f > 1.0f ? 1.0f : f;
        }
        g->scale = scale;
        if (s_gs_log) {
            static int n;
            if ((n++ % 30) == 0)
                fprintf(stderr, "[GUNSWAY] frame %u turn %.4f %.4f lag %.4f %.4f vel %.3f %.3f scale %.2f dt %.4f\n", g_game_frames,
                        (double)px, (double)py, (double)g->lag[0], (double)g->lag[1], (double)g->vel[0], (double)g->vel[1],
                        (double)scale, dt);
        }
        return;
    }
    /* ret == 0xF463F: the weapon pass */
    if (!g->have || (!s_gs_on && !(s_rp_on > 0 && s_rp_blend > 0.0f)))
        return;
    {
        float scale = g->scale > 0.0f ? g->scale : 0.5f, lx = 0.0f, ly = 0.0f, lz = 0.0f, lim = 0.14f, ang, s, c1, ox, oy, oz, E[9];
        float M[9], t[3];
        int r, c, kk;
        if (s_gs_on) {
            lx = g->lag[0] * scale;
            ly = g->lag[1] * scale;
            lx = lim * tanhf(lx / lim);         /* soft stop: a hard flick cannot throw the gun off screen */
            ly = lim * tanhf(ly / lim);
        }
        if (s_rp_on > 0 && s_rp_blend > 0.0f) { /* sprint: gun carried across the body, swinging with the steps */
            float b = s_rp_blend * s_rp_blend * (3.0f - 2.0f * s_rp_blend), ph = s_rp_phase;
            ly += b * (s_rp_pose[0] + s_rp_bob[0] * sinf(ph));
            lx += b * (s_rp_pose[1] + s_rp_bob[1] * cosf(2.0f * ph));
            lz += b * (s_rp_pose[2] + s_rp_bob[2] * sinf(ph));
        }
        ang =sqrtf(lx * lx + ly * ly + lz * lz);
        if (ang < 1e-5f)
            return;
        for (i = 0; i < 16; i++)
            g->saved[i] = guest_f(p + 0xD0u + 4u * (uint32_t)i);
        ox = lx / ang;                          /* rotation vector (lx, ly, lz): axis, angle */
        oy = ly / ang;
        oz = lz / ang;
        s = sinf(ang);
        c1 = 1.0f - cosf(ang);
        /* Rodrigues: E = I + s [a]x + (1 - cos) [a]x^2, a = (ox, oy, oz) */
        E[0] = 1.0f - c1 * (oy * oy + oz * oz);  E[1] = c1 * ox * oy - s * oz;  E[2] = c1 * ox * oz + s * oy;
        E[3] = c1 * ox * oy + s * oz;  E[4] = 1.0f - c1 * (ox * ox + oz * oz);  E[5] = c1 * oy * oz - s * ox;
        E[6] = c1 * ox * oz - s * oy;  E[7] = c1 * oy * oz + s * ox;  E[8] = 1.0f - c1 * (ox * ox + oy * oy);
        for (r = 0; r < 3; r++)
            for (c = 0; c < 3; c++)
                M[r * 3 + c] = g->saved[r * 4 + c];
        t[0] = g->saved[12]; t[1] = g->saved[13]; t[2] = g->saved[14];
        for (r = 0; r < 3; r++)
            for (c = 0; c < 3; c++) {
                float sum = 0.0f;
                for (kk = 0; kk < 3; kk++)
                    sum += M[r * 3 + kk] * E[kk * 3 + c];
                guest_set_f(p + 0xD0u + 16u * (uint32_t)r + 4u * (uint32_t)c, sum);
            }
        for (c = 0; c < 3; c++)
            guest_set_f(p + 0x100u + 4u * (uint32_t)c, t[0] * E[0 * 3 + c] + t[1] * E[1 * 3 + c] + t[2] * E[2 * 3 + c]);
        g->applied = 1;
    }
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
    gun_sway_view(view);
    {   /* RECOMP_VIEW_LOG=1: every call in three short windows of the player's view updates (who calls, arg, fov, camera transform) */
        static int on = -1;
        static uint32_t n;
        if (on < 0) {
            const char *e = getenv("RECOMP_VIEW_LOG");
            on = e && *e == '1';
        }
        if (on && view && GUEST32(view + 0xE4)) {
            uint32_t p = GUEST32(view + 0xE4);
            if (guest_f(p + 0xC0) > 0.3f) {
                n++;
                if ((n >= 600 && n < 680) || (n >= 1500 && n < 1580) || (n >= 2400 && n < 2480))
                    fprintf(stderr, "[VCALL] n %u frame %u ret %08X arg %08X view %08X fov %.2f r0 %.3f %.3f %.3f r1 %.3f %.3f %.3f r2 %.3f %.3f %.3f pos %.2f %.2f %.2f f190 %u\n",
                            n, g_game_frames, GUEST32(g_esp), GUEST32(g_esp + 4), view, (double)(guest_f(p + 0xC0) * 57.29578f),
                            (double)guest_f(p + 0xD0), (double)guest_f(p + 0xD4), (double)guest_f(p + 0xD8),
                            (double)guest_f(p + 0xE0), (double)guest_f(p + 0xE4), (double)guest_f(p + 0xE8),
                            (double)guest_f(p + 0xF0), (double)guest_f(p + 0xF4), (double)guest_f(p + 0xF8),
                            (double)guest_f(p + 0x100), (double)guest_f(p + 0x104), (double)guest_f(p + 0x108), (unsigned)GUEST8(p + 0x190));
            }
        }
    }
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

/* GUN SWAY research: sub_0018E150 builds the view matrix (4x4 floats at mat) of one view (this = view) just
 * before it hands it to the camera. RECOMP_VIEW_LOG=1 logs, per view object, every time that matrix or the
 * camera params' transform (params + 0xD0) changes, so the weapon's view (fixed to the camera?) can be told apart. */
void game_view_matrix_hook(uint32_t mat, uint32_t view)
{
    static int on = -1, lines, nseen;
    static uint32_t seen[8], seen_frame, calls_in_frame;
    static float last[8][16];
    float m[16], p[16];
    uint32_t params;
    int k, i, changed = 0;
    if (on < 0) {
        const char *e = getenv("RECOMP_VIEW_LOG");
        on = e && *e == '1';
    }
    if (g_game_frames != seen_frame) {
        seen_frame = g_game_frames;
        calls_in_frame = 0;
    }
    calls_in_frame++;
    if (!on || lines >= 400)
        return;
    for (k = 0; k < nseen && seen[k] != view; k++) {}
    if (k == nseen) {
        if (nseen >= 8)
            return;
        seen[nseen++] = view;
        memset(last[k], 0, sizeof last[k]);
        changed = 1;
    }
    for (i = 0; i < 16; i++)
        m[i] = guest_f(mat + 4u * (uint32_t)i);
    for (i = 0; i < 16; i++)
        if (fabsf(m[i] - last[k][i]) > 1e-4f)
            changed = 1;
    if (!changed)
        return;
    memcpy(last[k], m, sizeof m);
    params = GUEST32(view + 0xE4);
    for (i = 0; i < 16; i++)
        p[i] = params ? guest_f(params + 0xD0u + 4u * (uint32_t)i) : 0.0f;
    lines++;
    fprintf(stderr, "[VIEW] frame %u call %u view %08X params %08X cam %08X fov %.2f\n", g_game_frames, calls_in_frame, view,
            params, GUEST32(view + 0x8C4), params ? (double)(guest_f(params + 0xC0) * 57.29578f) : 0.0);
    fprintf(stderr, "[VIEW]   mat %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f\n",
            (double)m[0], (double)m[1], (double)m[2], (double)m[3], (double)m[4], (double)m[5], (double)m[6], (double)m[7],
            (double)m[8], (double)m[9], (double)m[10], (double)m[11], (double)m[12], (double)m[13], (double)m[14], (double)m[15]);
    fprintf(stderr, "[VIEW]   d0  %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f\n",
            (double)p[0], (double)p[1], (double)p[2], (double)p[3], (double)p[4], (double)p[5], (double)p[6], (double)p[7],
            (double)p[8], (double)p[9], (double)p[10], (double)p[11], (double)p[12], (double)p[13], (double)p[14], (double)p[15]);
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

/* Damage mode (ai_tuning.c): is the weapon whose ballistics row starts with this id (= weapon entry +0x124) one of the
 * single-shot heavy guns (bolt-action / scoped / semi-auto rifles)? Names come from the entries the weapon loader
 * captured; a name that cannot be read counts as light. name_out (optional) gets the weapon's name for the log. */
int game_weapon_is_heavy(uint32_t row_id, char *name_out, size_t name_sz)
{
    int e;
    for (e = 0; e < s_wd_n; e++) {
        char *n;
        if (!s_wd[e].have || s_wd[e].orig[0x124 / 4] != row_id)
            continue;
        if (!s_wd[e].name_have)
            s_wd[e].name_have = wd_read_name(s_wd[e].orig[0x0E0 / 4], s_wd[e].name, sizeof s_wd[e].name) ? 2 : 1;
        if (s_wd[e].name_have != 2)
            return 0;
        n = s_wd[e].name;
        if (name_out && name_sz) {
            strncpy(name_out, n, name_sz - 1);
            name_out[name_sz - 1] = 0;
        }
        return !_stricmp(n, "M1") || strstr(n, "Karabiner") || strstr(n, "Scoped") || strstr(n, "scoped") ||
               strstr(n, "Sniper") || strstr(n, "sniper") || strstr(n, "Springfield") || strstr(n, "Enfield") ||
               strstr(n, "enfield") || strstr(n, "Kar98") || strstr(n, "Gewehr") || strstr(n, "gewehr") ||
               strstr(n, "Mosin") || strstr(n, "Garand") || strstr(n, "MN1891") || strstr(n, "MNVP") || strstr(n, "SVT") || strstr(n, "Tokarev");
    }
    return 0;
}

/* SCOPE ZOOM: is the weapon in the player's hands one with a real scope (a sniper)? At the start of a level the game's scope
 * opens at the same fov as iron sights (68 degrees) until the first D-pad press sets its zoom, so the fov alone cannot tell
 * a scope from aiming down the sights. The held weapon object (soldier + 0x1128) points at its definition entry somewhere in
 * its first 0x400 bytes; the entry's name says if it has a scope. Cached per weapon object. */
#define SW_MEM32(a) (*(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + (uint32_t)(a)))
int game_held_weapon_scoped(void)
{
    extern uint32_t game_pc_get(void);
    static uint32_t cw, cfail;
    static int cres;
    uint32_t pc = game_pc_get(), sol, w, i;
    int e;
    if (pc < 0x80000000u || !wd_guest_readable(pc, 0x1D0))     /* the player object is freed / rebuilt while a level loads: never trust the pointers blindly */
        return 0;
    sol = SW_MEM32(pc + 0x1CC);
    if (sol < 0x80000000u || !wd_guest_readable(sol, 0x1130))
        return 0;
    w = SW_MEM32(sol + 0x1128);
    if (w < 0x80000000u || !wd_guest_readable(w, 0x400))
        return 0;
    if (w == cw)
        return cres;
    /* the object holds the weapon's id (entry + 0x124), a pointer to its entry, or a pointer to the ballistics row whose first
     * word is that id: any of the three names the entry */
    for (e = 0; e < s_wd_n; e++) {
        uint32_t id = s_wd[e].orig[0x124 / 4], va = s_wd[e].va;
        char n[32];
        if (!id || !va)
            continue;
        for (i = 0; i < 0x400; i += 4) {
            uint32_t v = SW_MEM32(w + i);
            int hit = v == id || (v >= va && v < va + 0x164u) ||
                      (v >= 0x80000000u && v < 0x84000000u && wd_guest_readable(v, 4) && SW_MEM32(v) == id);
            if (!hit)
                continue;
            if (!wd_read_name(s_wd[e].orig[0x0E0 / 4], n, sizeof n))
                break;
            cw = w;
            cres = strstr(n, "Scoped") || strstr(n, "scoped") || strstr(n, "Karabiner") || strstr(n, "Sniper") ||
                   strstr(n, "sniper") || strstr(n, "MN1891") || strstr(n, "MNVP") || strstr(n, "Springfield");
            fprintf(stderr, "[SCOPEWPN] weapon object %08X entry %d '%s' (word +0x%X = %08X) scoped=%d\n", w, e, n, i, v, cres);
            return cres;
        }
    }
    if (cfail != w) {
        cfail = w;
        fprintf(stderr, "[SCOPEWPN] weapon object %08X: no entry found (%d entries); first words:", w, s_wd_n);
        for (i = 0; i < 0x80; i += 4)
            fprintf(stderr, " %08X", SW_MEM32(w + i));
        fprintf(stderr, "\n");
    }
    return 0;
}

/* The weapon object in the player's hands (guest address), 0 when there is no player yet. Used by the grenade macro. */
unsigned game_held_weapon_obj(void)
{
    extern uint32_t game_pc_get(void);
    uint32_t pc = game_pc_get(), sol, w;
    if (pc < 0x80000000u || !wd_guest_readable(pc, 0x1D0))
        return 0;
    sol = SW_MEM32(pc + 0x1CC);
    if (sol < 0x80000000u || !wd_guest_readable(sol, 0x1130))
        return 0;
    w = SW_MEM32(sol + 0x1128);
    return w >= 0x80000000u && w < 0x84000000u ? w : 0;
}

/* Is the weapon in the player's hands a grenade (Stick, Brit, Frag, anything named *renade)? 1 yes, 0 a gun, -1 not known
 * (no player yet, or no definition entry matches the held weapon object). Found like game_held_weapon_scoped; cached per
 * weapon object, so a frame costs one compare. Used by the grenade countdown. */
int game_held_weapon_grenade(void)
{
    extern uint32_t game_pc_get(void);
    static uint32_t cw;
    static int cres = -1;
    uint32_t pc = game_pc_get(), sol, w, i;
    int e;
    if (pc < 0x80000000u || !wd_guest_readable(pc, 0x1D0))     /* the player object is freed / rebuilt while a level loads (crash 2026-11-20 at 1_11) */
        return -1;
    sol = SW_MEM32(pc + 0x1CC);
    if (sol < 0x80000000u || !wd_guest_readable(sol, 0x1130))
        return -1;
    w = SW_MEM32(sol + 0x1128);
    if (w < 0x80000000u || !wd_guest_readable(w, 0x400))
        return -1;
    if (w == cw) {
        static int retry;
        if (cres >= 0 || ++retry < 240)             /* a weapon not found yet is looked for again now and then (the table may load late) */
            return cres;
        retry = 0;
    }
    cw = w;
    cres = -1;
    for (e = 0; e < s_wd_n && cres < 0; e++) {
        uint32_t id = s_wd[e].orig[0x124 / 4], va = s_wd[e].va;
        char n[32];
        if (!id || !va)
            continue;
        for (i = 0; i < 0x400; i += 4) {
            uint32_t v = SW_MEM32(w + i);
            int hit = v == id || (v >= va && v < va + 0x164u) ||
                      (v >= 0x80000000u && v < 0x84000000u && wd_guest_readable(v, 4) && SW_MEM32(v) == id);
            if (!hit)
                continue;
            if (!wd_read_name(s_wd[e].orig[0x0E0 / 4], n, sizeof n))
                break;
            cres = !_stricmp(n, "Stick") || !_stricmp(n, "Brit") || !_stricmp(n, "Frag") || strstr(n, "renade") ? 1 : 0;
            fprintf(stderr, "[GRENWPN] weapon object %08X entry %d '%s' grenade=%d\n", w, e, n, cres);
            break;
        }
    }
    if (cres < 0) {
        static uint32_t said_w;
        if (said_w != w) {
            said_w = w;
            fprintf(stderr, "[GRENWPN] weapon object %08X: no entry found (%d entries)\n", w, s_wd_n);
        }
    }
    return cres;
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

/* ARR-DTOR-DBG (cross-level weapon splice): sub_000631D0 is the destructor of the component container (vtable 0x3055E4,
 * u16 count at +8, entries [object, x] stride 8 from +0x10; each live entry gets object->vtbl[+0x10](1)). A spliced
 * weapon leaves one entry as heap garbage. This logs every entry whose object or vtable slot is not code, so the log
 * shows which slot / which neighbour entries are good. Always on, cheap: only walks the entries and prints bad ones. */
#define DT_M32(a) (*(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + (uint32_t)(a)))
#define DT_M16(a) (*(volatile uint16_t *)((uintptr_t)g_xbox_mem_offset + (uint32_t)(a)))
void game_arr_dtor_dbg(uint32_t self)
{
    static int lines, calls;
    uint32_t cnt, i;
    int bad = 0;

    if (self < 0x10000u || self >= 0x84000000u - 0x40u || (self & 3u))
        return;
    cnt = DT_M16(self + 8u);
    if (self < 0x600000u || cnt > 2048u) {          /* static container or absurd count: print its raw words (the crash run had this = 0x466960) */
        static int big;
        if (big < 12) {
            uint32_t k;
            big++;
            fprintf(stderr, "[ARRDTOR] RAW self %08X cnt %u words:", (unsigned)self, (unsigned)cnt);
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %X", (unsigned)DT_M32(self + k * 4u));
            fprintf(stderr, "\n");
        }
    }
    if (cnt == 0 || cnt > 2048u || self + 0x10u + cnt * 8u >= 0x84000000u)
        return;
    calls++;
    for (i = 0; i < cnt; i++) {
        uint32_t p = DT_M32(self + 0x10u + i * 8u), x = DT_M32(self + 0x14u + i * 8u), v = 0, slot = 0;
        int ok = 1;
        if (!p)
            continue;
        if (p < 0x10000u || p >= 0x84000000u - 0x40u || (p & 3u)) {
            ok = 0;
        } else {
            v = DT_M32(p);
            if (v < 0x10000u || v >= 0x84000000u - 0x40u) {
                ok = 0;
            } else {
                slot = DT_M32(v + 0x10u);
                if (slot < 0x11000u || slot >= 0x540000u)
                    ok = 0;
            }
        }
        if (!ok && lines < 80) {
            uint32_t k;
            bad = 1;
            lines++;
            fprintf(stderr, "[ARRDTOR] call#%d self %08X vtbl %08X cnt %u entry %u obj %08X x %08X vt %08X slot %08X\n",
                    calls, (unsigned)self, (unsigned)DT_M32(self), (unsigned)cnt, (unsigned)i, (unsigned)p, (unsigned)x,
                    (unsigned)v, (unsigned)slot);
            if (p >= 0x10000u && p < 0x84000000u - 0x40u) {
                fprintf(stderr, "[ARRDTOR]   obj dwords:");
                for (k = 0; k < 12; k++)
                    fprintf(stderr, " %X", (unsigned)DT_M32(p + k * 4u));
                fprintf(stderr, "\n");
            }
        }
    }
    if (bad) {                                  /* the whole container once: which neighbours are good (object, vtable) */
        fprintf(stderr, "[ARRDTOR]   entries:");
        for (i = 0; i < cnt && i < 40u; i++)
            fprintf(stderr, " %X/%X", (unsigned)DT_M32(self + 0x10u + i * 8u), (unsigned)DT_M32(self + 0x14u + i * 8u));
        fprintf(stderr, "\n");
    }
}

/* SAMPLER-DBG (cross-level weapon splice): sub_00062E40 samples an animation: for each track index edi of the clip it calls
 * channel[edi]->vtbl+0x10 on two 80-entry object arrays at this+0x14 and this+0x154. A spliced weapon's clip leaves a slot as
 * uninitialised heap (target 0x824A1600/1680 moved with the file size). This logs, for every bad slot, which index / how many
 * slots are built, and once for each of the first 6 distinct good animation objects (baseline). */
static int dt_slot_ok(uint32_t p)
{
    uint32_t vt, sl;
    if (p < 0x10000u || p >= 0x84000000u - 0x40u || (p & 3u))
        return 0;
    vt = DT_M32(p);
    if (vt < 0x10000u || vt >= 0x84000000u - 0x40u)
        return 0;
    sl = DT_M32(vt + 0x10u);
    return sl >= 0x11000u && sl < 0x540000u;
}
static void dt_sampler_dump(const char *tag, uint32_t self, uint32_t idx)
{
    uint32_t j, a = 0, b = 0, first_bad_a = 0xFFFFu, last_good_a = 0xFFFFu;
    uint32_t o8 = DT_M32(self + 8u);
    for (j = 0; j < 80; j++) {
        int oa = dt_slot_ok(DT_M32(self + 0x14u + j * 4u)), ob = dt_slot_ok(DT_M32(self + 0x154u + j * 4u));
        a += (uint32_t)oa;
        b += (uint32_t)ob;
        if (oa)
            last_good_a = j;
        else if (first_bad_a == 0xFFFFu && DT_M32(self + 0x14u + j * 4u))
            first_bad_a = j;
    }
    fprintf(stderr, "[SAMPLER] %s this %08X idx %u validA %u validB %u lastGoodA %u firstBadNonNullA %u +8 %08X +10 %08X +294 %08X\n",
            tag, (unsigned)self, (unsigned)idx, (unsigned)a, (unsigned)b, (unsigned)last_good_a, (unsigned)first_bad_a,
            (unsigned)o8, (unsigned)DT_M32(self + 0x10u), (unsigned)DT_M32(self + 0x294u));
    fprintf(stderr, "[SAMPLER]   slotsA:");
    for (j = 0; j < 12; j++)
        fprintf(stderr, " %X", (unsigned)DT_M32(self + 0x14u + j * 4u));
    fprintf(stderr, "\n[SAMPLER]   slotsB:");
    for (j = 0; j < 12; j++)
        fprintf(stderr, " %X", (unsigned)DT_M32(self + 0x154u + j * 4u));
    fprintf(stderr, "\n[SAMPLER]   obj8:");
    if (o8 >= 0x10000u && o8 < 0x84000000u - 0x60u)
        for (j = 0; j < 12; j++)
            fprintf(stderr, " %X", (unsigned)DT_M32(o8 + j * 4u));
    fprintf(stderr, "\n");
}
void game_sampler_dbg(uint32_t ebx_arrA, uint32_t ebp_arrB, uint32_t idx)
{
    static uint32_t seen[8];
    static int nseen, nbad;
    uint32_t self = ebx_arrA - 0x14u;
    int i;
    if (ebx_arrA < 0x10000u || ebx_arrA >= 0x84000000u - 0x400u) {
        static int odd;
        if (odd++ < 4)
            fprintf(stderr, "[SAMPLER] odd this %08X idx %u\n", (unsigned)self, (unsigned)idx);
        return;
    }
    if (idx >= 80u) {                                   /* track index past the 80-entry channel arrays: the BAD case of the crash dumps (edi=0x140) */
        if (nbad < 6) {
            nbad++;
            dt_sampler_dump("BAD-idx>=80", self, idx);
        }
        return;
    }
    if (!dt_slot_ok(DT_M32(ebx_arrA + idx * 4u)) || !dt_slot_ok(DT_M32(ebp_arrB + idx * 4u))) {
        if (nbad < 6) {
            nbad++;
            dt_sampler_dump("BAD", self, idx);
        }
        return;
    }
    for (i = 0; i < nseen; i++)
        if (seen[i] == self)
            return;
    if (nseen < 6) {
        seen[nseen++] = self;
        dt_sampler_dump("good", self, idx);
    }
}

/* SAMPLER2-DBG: the all-channels loop of sub_00062E40 (loc_00062F27): count = MEM32(this+0x10) channels, objects in the arrays at this+0x14 / this+0x154.
 * A bad run (merged animation bank) crashed here at load with 10000+ wild icalls: log this / count / how many slots are valid. */
void game_sampler2_dbg(uint32_t arrA, uint32_t arrB, uint32_t count, uint32_t esp_)
{
    static int lines;
    static uint32_t seen[8];
    static int nseen;
    uint32_t self = arrA - 0x14u, j, okA = 0, okB = 0, firstBad = 0xFFFFu;
    int i, isnew = 1;
    (void)arrB;
    if (arrA < 0x10000u || arrA >= 0x84000000u - 0x400u)
        return;
    for (i = 0; i < nseen; i++)
        if (seen[i] == self)
            isnew = 0;
    if (count <= 80u && !isnew)
        return;
    for (j = 0; j < count && j < 80u; j++) {
        int a = dt_slot_ok(DT_M32(arrA + j * 4u)), b = dt_slot_ok(DT_M32(arrB + j * 4u));
        okA += (uint32_t)a;
        okB += (uint32_t)b;
        if ((!a || !b) && firstBad == 0xFFFFu)
            firstBad = j;
    }
    if (lines >= 14)
        return;
    if (count <= 80u && firstBad == 0xFFFFu) {                       /* a good new object: baseline, small ones only */
        if (nseen < 8 && count <= 16u)
            seen[nseen++] = self;
        else
            return;
    } else if (isnew && nseen < 8)
        seen[nseen++] = self;
    lines++;
    fprintf(stderr, "[SAMPLER2] this %08X count %u okA %u okB %u firstBadSlot %u +8 %08X ret %08X args %08X %08X %08X\n",
            (unsigned)self, (unsigned)count, (unsigned)okA, (unsigned)okB, (unsigned)firstBad, (unsigned)DT_M32(self + 8u),
            (unsigned)DT_M32(esp_), (unsigned)DT_M32(esp_ + 0x34u), (unsigned)DT_M32(esp_ + 0x38u), (unsigned)DT_M32(esp_ + 0x3Cu));
    if (firstBad != 0xFFFFu) {
        fprintf(stderr, "[SAMPLER2]   slots around first bad A:");
        for (j = firstBad > 3u ? firstBad - 3u : 0u; j < firstBad + 6u && j < 90u; j++)
            fprintf(stderr, " [%u]%X", (unsigned)j, (unsigned)DT_M32(arrA + j * 4u));
        fprintf(stderr, "\n");
    }
}

/* ARR-WATCH: log every change of the 0x466960 global container (this of the crashing destructor) as seen from the load pump / sampler. */
void game_arr_watch(void)
{
    static uint32_t snap[8];
    static int have, lines;
    uint32_t k;
    int ch = 0;
    for (k = 0; k < 8; k++) {
        uint32_t w = DT_M32(0x466960u + k * 4u);
        if (!have || w != snap[k]) {
            ch = 1;
            snap[k] = w;
        }
    }
    if (ch && lines < 40) {
        lines++;
        fprintf(stderr, "[ARRWATCH] #%d 466960:", lines);
        for (k = 0; k < 8; k++)
            fprintf(stderr, " %X", (unsigned)snap[k]);
        fprintf(stderr, "\n");
    }
    have = 1;
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
            s_wd[e].name_have = 0;                                  /* a slot reused by a later level must not keep the old weapon's name */
            s_wd[e].wmod_ballist_va = 0;
            s_wd[e].wmod_ballist_tried = 0;
            s_wd_n++;
        } else if (e < s_wd_n) {
            s_wd[e].have = 0;                                       /* being filled again: capture later */
            s_wd[e].name_have = 0;
            s_wd[e].wmod_ballist_va = 0;
            s_wd[e].wmod_ballist_tried = 0;
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
#define VX_MAX 1024                     /* blood + rain + every explosion definition of a level */
static struct { uint32_t va; uint32_t mgr; uint32_t tbl; int have; int blood; int weather; int expl; uint32_t hash; uint32_t orig[VX_WORDS]; } s_vx[VX_MAX];
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

/* the sorted name hashes of the explosion particle definitions (built once) */
static void ex_build(void)
{
    char path[200], *c;
    size_t i;
    if (s_ex_n >= 0)
        return;
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
}

static int ex_is_explosion(uint32_t hash)
{
    ex_build();
    return bsearch(&hash, s_ex_hash, (size_t)s_ex_n, sizeof s_ex_hash[0], ex_cmp) != NULL;
}

void game_fx_placed(uint32_t emitter)
{
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    static float range = 14.0f;
    static DWORD last;
    static int logged, inited;
    uint32_t def, hash, cam;
    float dx, dy, dz, d;
    DWORD now;
    if (!inited) {
        const char *e = getenv("RECOMP_SCREENFX_EXPLODE_DIST");
        inited = 1;
        if (e && atof(e) > 0.0)
            range = (float)atof(e);
        ex_build();
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

int recomp_menu_up(void)
{
    return wx_menu_up();
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
    if ((recomp_screenfx_get(2) <= 0 && game_weaponlight_get() <= 0) || !game_rain_on_get()) {   /* the weapon gets wet even with Screen rain off */
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

/* Recomp Settings "Explosion size": the particle definitions of every explosion (grenades, rockets and tank shells, barrels,
 * fuel tanks, demolition charges, AA guns: explosion_defs.h) are rewritten from the captured originals, like the blood and
 * the rain: the four size floats (+0xC0..+0xCC) and the six velocity floats (+0xA4..+0xB8) times the level's factor, so the
 * fireball, the smoke and the flying debris all grow together. The emit count and the life stay as the game made them (the
 * particle pool is sized from them). A change reaches the next explosion; nothing runs per frame. Level 0 = the game's own.
 * The damage and the blast radius of the game are NOT touched. RECOMP_EXPLOSION_SIZE=0..4 */
static const float k_ex_size[5] = { 1.0f, 1.5f, 2.0f, 3.0f, 4.0f };
static const char *const k_ex_name[5] = { "Normal", "x1.5", "x2", "x3", "x4" };
static int s_ex_lv = -1;

static void ex_level_init(void)
{
    const char *e;
    if (s_ex_lv >= 0)
        return;
    e = getenv("RECOMP_EXPLOSION_SIZE");
    s_ex_lv = e && e[0] ? atoi(e) : 0;
    s_ex_lv = s_ex_lv < 0 ? 0 : s_ex_lv > 4 ? 4 : s_ex_lv;
}

static void vx_apply_explosion(int e)
{
    uint8_t *mem = (uint8_t *)(uintptr_t)g_xbox_mem_offset;
    uint32_t *p = (uint32_t *)(mem + s_vx[e].va);
    const uint32_t *o = s_vx[e].orig;
    float k;
    int i;
    ex_level_init();
    k = k_ex_size[s_ex_lv];
    for (i = 0xC0; i <= 0xCC; i += 4)
        p[i / 4] = vx_w(vx_f(o[i / 4]) * k);
    for (i = 0xA4; i <= 0xB8; i += 4)
        p[i / 4] = vx_w(vx_f(o[i / 4]) * k);
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

static void game_blood_main_rgb(float rgb[3])
{
    int c[4][3], k;
    vx_colors(c);
    for (k = 0; k < 3; k++)
        rgb[k] = (float)c[1][k] / 255.0f;
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
        if (s_vx[e].have && s_vx[e].expl) {            /* an explosion: Explosion size */
            if (!vx_alive(e))
                s_vx[e].expl = 0;
            else if (*(const uint32_t *)(mem + s_vx[e].va + 0x100) != s_vx[e].hash)
                s_vx[e].have = 0;
            else
                vx_apply_explosion(e);
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

/* the colour of the blood stains on walls and floor (BLOOD-STAIN): the spray's main colour, 0..255 */
void game_blood_stain_rgb(int rgb[3])
{
    float f[3];
    int k;
    vx_levels();
    game_blood_main_rgb(f);
    for (k = 0; k < 3; k++)
        rgb[k] = vx_clamp((int)(f[k] * 255.0f + 0.5f), 0, 255);
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

int game_expsize_get(void)
{
    ex_level_init();
    return s_ex_lv;
}

void game_expsize_set(int l)
{
    ex_level_init();
    s_ex_lv = l < 0 ? 0 : l > 4 ? 4 : l;
    vx_apply();
}

const char *game_expsize_name(int l)
{
    return k_ex_name[l < 0 ? 0 : l > 4 ? 4 : l];
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
            s_vx[e].expl = 0;
            continue;
        }
        p = (const uint32_t *)(mem + s_vx[e].va);
        memcpy(s_vx[e].orig, p, sizeof s_vx[e].orig);
        s_vx[e].hash = p[0x100 / 4];
        s_vx[e].blood = s_vx[e].hash == hb || s_vx[e].hash == hh;
        s_vx[e].weather = wx_is_weather(s_vx[e].hash);
        s_vx[e].expl = !s_vx[e].blood && !s_vx[e].weather && ex_is_explosion(s_vx[e].hash);
        if (s_vx[e].expl && getenv("RECOMP_VFX_LOG")) {
            float sz[4];
            memcpy(sz, &p[0xC0 / 4], 16);
            fprintf(stderr, "[EXPLOSION] def %08X at %08X: emit %d life %g size %g %g %g %g\n", s_vx[e].hash, s_vx[e].va,
                    (int)p[0x70 / 4], (double)vx_f(p[0x74 / 4]), (double)sz[0], (double)sz[1], (double)sz[2], (double)sz[3]);
        }
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

/* ── PAN TRACE (RECOMP_PAN_TRACE=1) ─────────────────────────────
 * Left-side sound fade research (2026-11-19). Three hooks inserted at the top of the generated functions:
 *   sub_001AEFA0  emitter SetPan(angle int, 65536 = 360 deg): who sets the pan angle (caller), to what
 *   sub_00196C66  per-voice pan: angle16 at voice+0x1C and the five speaker gains the game's table gives
 *   sub_002B02FE  SetVolume(handle, channel, float)
 * Every line carries the player's camera yaw (character +0x110, radians, larger = further left), so a turn can be
 * read against what the sound did. Off unless the env var is 1: one flag test per call. */
extern uint32_t game_pan_player(void);

static float pan_cam_deg(void);
extern float (*g_apu_cam_cb)(void);     /* apu_vp.c: lets its [RMS] line carry the camera yaw */

static int pan_trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_PAN_TRACE");
        on = e && *e == '1';
        if (on) g_apu_cam_cb = pan_cam_deg;
    }
    return on;
}

static int pan_rate(unsigned per_sec)
{
    static DWORD win;
    static unsigned n;
    DWORD now = GetTickCount();
    if (now - win >= 1000u) { win = now; n = 0; }
    return n++ < per_sec;
}

static float pan_cam_deg(void)
{
    uint32_t p = game_pan_player();
    return p ? guest_f(p + 0x110) * 57.29578f : 9999.0f;
}

void sub_001AEFA0_enter(void)
{
    static int32_t last[512];
    static uint32_t who[512];
    uint32_t em, h;
    int32_t arg, d;
    if (!pan_trace_on())
        return;
    em = (uint32_t)g_ecx;
    arg = (int32_t)GUEST32(g_esp + 4);
    h = (em >> 4) & 511u;
    d = (arg - last[h]) & 0xFFFF;
    if (d > 0x8000) d -= 0x10000;
    if (who[h] == em && d > -0x0400 && d < 0x0400)
        return;
    who[h] = em;
    last[h] = arg;
    if (pan_rate(150))
        fprintf(stderr, "[PANSET] t=%u cam=%.1f em=%08X ret=%08X arg=%d (%.1f deg) handle=%08X cls=%08X\n",
                (unsigned)GetTickCount(), (double)pan_cam_deg(), em, GUEST32(g_esp), arg,
                (double)(arg & 0xFFFF) * 360.0 / 65536.0, GUEST32(em + 0xC4), GUEST32(em + 0x48) ? GUEST32(GUEST32(em + 0x48)) : 0u);
}

void sub_00196C66_enter(void)
{
    static uint16_t last[512];
    static DWORD lastt[512];
    uint32_t idx = GUEST32(g_esp + 4), vs, nch, ch;
    uint16_t ang;
    int d;
    DWORD now;
    char g[64];
    if (!pan_trace_on() || idx >= 512u)
        return;
    vs = GUEST32(0x523BF4) + idx * 0x90u;
    ang = GUEST16(vs + 0x1C);
    now = GetTickCount();
    d = (int)(uint16_t)(ang - last[idx]);
    if (d > 0x8000) d -= 0x10000;
    if (lastt[idx] && d > -0x0500 && d < 0x0500 && now - lastt[idx] < 3000u)
        return;
    last[idx] = ang;
    lastt[idx] = now;
    if (!pan_rate(150))
        return;
    nch = GUEST8(0x523A5F);
    if (nch > 6) nch = 6;
    g[0] = 0;
    for (ch = 0; ch < nch; ch++) {
        uint32_t tab = GUEST32(0x523C84 + 4u * ch);
        char b[16];
        snprintf(b, sizeof b, " %3u", tab ? (unsigned)GUEST8(tab + (ang >> 8)) : 0u);
        strcat(g, b);
    }
    fprintf(stderr, "[PANVOICE] t=%u cam=%.1f v=%u ang=%04X (%.1f deg) gains(FC FR RR RL FL)=%s f4e=%u f25=%02X vol38=%.3f vol48=%.3f\n",
            (unsigned)now, (double)pan_cam_deg(), idx, ang, (double)ang * 360.0 / 65536.0, g,
            (unsigned)GUEST8(vs + 0x4E), (unsigned)GUEST8(vs + 0x25), (double)guest_f(vs + 0x38), (double)guest_f(vs + 0x48));
}

void sub_002B02FE_enter(void)
{
    static float last[512];
    static uint32_t who[512];
    uint32_t handle = GUEST32(g_esp + 4), h = (handle >> 2) & 511u;
    int32_t ch = (int32_t)GUEST32(g_esp + 8);
    float v = guest_f(g_esp + 12);
    if (!pan_trace_on())
        return;
    if (who[h] == handle && fabsf(v - last[h]) < 0.04f)
        return;
    who[h] = handle;
    last[h] = v;
    if (pan_rate(150))
        fprintf(stderr, "[PANVOL] t=%u cam=%.1f handle=%08X ch=%d vol=%.3f ret=%08X\n", (unsigned)GetTickCount(),
                (double)pan_cam_deg(), handle, ch, (double)v, GUEST32(g_esp));
}

/* PAN TRACE, part 2: sub_002ECC67 = SetMixBinVolumes(handle, {count, pairs*}); each pair is (bin, volume in 1/100 dB). This is
 * exactly what the game gives the voice. Per handle it logs the pairs and the level each ear gets from them with the stereo
 * downmix (centre and rears at 0.7071, LFE 0.5), in dB re full scale, whenever that level moves by 1 dB or every 2 s, so the
 * game's own gain for one voice can be followed through a turn without guessing the content level. */
void sub_002ECC67_enter(void)
{
    static uint32_t hnd[256];
    static float lastL[256], lastR[256];
    static DWORD lastt[256];
    uint32_t handle = GUEST32(g_esp + 4), st = GUEST32(g_esp + 8), cnt, arr, i, h = (handle >> 2) & 255u;
    double a[8] = { 0 }, L, R, dl, dr;
    char b[160];
    DWORD now;
    if (!pan_trace_on() || !st)
        return;
    cnt = GUEST32(st);
    arr = GUEST32(st + 4);
    if (cnt == 0 || cnt > 8u || !arr)
        return;
    b[0] = 0;
    for (i = 0; i < cnt; i++) {
        uint32_t bin = GUEST32(arr + 8u * i);
        int32_t mb = (int32_t)GUEST32(arr + 8u * i + 4);
        char t[24];
        if (bin < 8u)
            a[bin] += mb <= -10000 ? 0.0 : pow(10.0, mb / 2000.0);
        snprintf(t, sizeof t, " %u:%d", (unsigned)bin, (int)mb);
        if (strlen(b) < 130) strcat(b, t);
    }
    L = a[0] + 0.7071 * a[2] + 0.5 * a[3] + 0.7071 * a[4];
    R = a[1] + 0.7071 * a[2] + 0.5 * a[3] + 0.7071 * a[5];
    dl = L > 1e-6 ? 20.0 * log10(L) : -120.0;
    dr = R > 1e-6 ? 20.0 * log10(R) : -120.0;
    now = GetTickCount();
    if (hnd[h] == handle && lastt[h] && now - lastt[h] < 2000u && fabs(dl - lastL[h]) < 1.0 && fabs(dr - lastR[h]) < 1.0)
        return;
    hnd[h] = handle; lastL[h] = (float)dl; lastR[h] = (float)dr; lastt[h] = now;
    if (pan_rate(400))
        fprintf(stderr, "[PANPAIR] t=%u cam=%.1f handle=%08X ret=%08X pairs(bin:mB)%s earL=%.1f earR=%.1f dB\n", (unsigned)now,
                (double)pan_cam_deg(), handle, GUEST32(g_esp), b, dl, dr);
}
