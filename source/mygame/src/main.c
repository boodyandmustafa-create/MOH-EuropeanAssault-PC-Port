/**
 * YOUR_GAME_NAME - Recompiled Game Entry Point
 *
 * This is the Windows executable that hosts the recompiled game code.
 * It performs the following initialization sequence:
 *
 * 1. Load the original XBE file from disk
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize the kernel bridge (thunk table in Xbox memory)
 * 5. Set up game file paths for I/O redirection
 * 6. Initialize the stack pointer
 * 7. Install VEH crash handler for diagnostics
 * 8. Call the game's original entry point (recompiled)
 *
 * Customize this file for your game:
 *   - Set YOUR_GAME_ENTRY_POINT to the XBE entry point address
 *   - Set YOUR_GAME_XBE_PATH to where the XBE file lives
 *   - Set YOUR_GAME_DIR to the game data directory
 *   - Add any CRT global pre-initialization your game needs
 *   - Customize the VEH handler for game-specific crash diagnosis
 *
 * XBE Details (fill in from xbe_parser output):
 *   Title:       YOUR_GAME_NAME
 *   Title ID:    0x00000000
 *   Base addr:   0x00010000
 *   Entry point: 0x00000000
 *   Code size:   ~??? KB (.text)
 *   Sections:    ?? (list them)
 *   Kernel imports: ??
 */

#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

int game_save_root_w(WCHAR *out, DWORD n);
static void migrate_saves_from_appdata(const WCHAR *root);

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>

/* getenv / _putenv for every file of the game and the runtime (rc_env.h is force-included): the CRT's getenv
 * walks the whole environment block on each call and showed as ~1.5% of the game thread, so lookups are
 * answered from a small per-thread cache; any _putenv bumps a generation that drops every cache. Lives here,
 * not in a file of its own, so the profile-guided link does not see a new object. */
#undef getenv
#undef _putenv
#define RC_ENV_SLOTS 128
typedef struct {
    unsigned gen;
    int klen;
    char *val;                   /* what getenv returned (NULL = unset), valid until the next _putenv */
    char key[56];
} RcEnvSlot;
static volatile LONG s_env_gen = 1;
static __declspec(thread) RcEnvSlot t_env_slot[RC_ENV_SLOTS];
char *rc_getenv(const char *name)
{
    size_t n = strlen(name);
    size_t h = ((size_t)(uintptr_t)name >> 3) * 2654435761u;
    RcEnvSlot *s = &t_env_slot[(h >> 7) & (RC_ENV_SLOTS - 1)];
    unsigned g = (unsigned)s_env_gen;
    if (n < sizeof(s->key) && s->gen == g && s->klen == (int)n && !memcmp(s->key, name, n))
        return s->val;
    {
        char *v = getenv(name);
        if (n < sizeof(s->key)) {
            memcpy(s->key, name, n + 1);
            s->klen = (int)n;
            s->val = v;
            s->gen = g;
        }
        return v;
    }
}
int rc_putenv(const char *str)
{
    int r = _putenv(str);
    InterlockedIncrement(&s_env_gen);
    return r;
}
#define getenv rc_getenv
#define _putenv rc_putenv

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* ── Global register state (defined in xbox_memory_layout.c) ── */

/* RECOMP_TLS is not optional here. The runtime defines these thread-local, and
 * a plain `extern` referencing a __declspec(thread) variable does not resolve
 * to the calling thread's copy -- it resolves to the image's TLS template. The
 * host side then writes g_esp somewhere the generated code never reads, so the
 * guest starts with every register at zero and faults immediately, having
 * apparently ignored the setup that visibly ran. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;
/* x87 and SSE state. Global for the same reason the volatile GPRs are: one
 * guest routine can lift to several C functions, so a value written in one
 * body is read in the next. Defined in xbox_memory_layout.c like the rest of
 * the register file. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS uint32_t g_mxcsr;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern ptrdiff_t g_xbox_mem_offset;

/* ── XBE Constants ─────────────────────────────────────────── */

/*
 * TODO: Set these from your xbe_parser output.
 * Run: py -3 -m tools.xbe_parser game/default.xbe
 */
#define YOUR_GAME_ENTRY_POINT   0x001EF095  /* XBE entry point VA */
#define YOUR_GAME_XBE_PATH      "game\\default.xbe"
#define YOUR_GAME_DIR            "game"

/* ── Forward declarations ──────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);

/* Recompiled entry point (generated by recomp pipeline) */
extern void xbe_entry_point(void);

/* ── Game-thread sampler (RECOMP_SAMPLE=<hz>) ─────────────────
 *
 * Why the game's frames drop where the executor is not busy: this reads the
 * instruction pointer of the thread that runs the game loop (the one that
 * calls the frame limiter, sub_000137C0), a few hundred times a second, from
 * a thread of its own (suspend, read, resume: ~30 us each), and files every
 * sample under two tags:
 *   - how long the game's frames were taking at that moment: 0 under 9 ms
 *     (a steady 110+ fps), 1 up to 11.5 ms, 2 slower;
 *   - the draw-distance multiplier: 0 up to x1.5, 1 in between, 2 from x4.
 * Frames and their time are counted per class too, so the report can say
 * milliseconds per frame (a share of the samples alone hides that the fast
 * class contains the limiter's idle time). Written to samples_game.txt every
 * 15 s; mygame/tools_dbg/symsamples.py turns the offsets into function names.
 * The sampler waits by spinning on the performance counter: it changes no
 * timer resolution, which the game's pacing depends on. */
extern volatile uint32_t g_game_frames;
extern float game_draw_mult_now(void);
#define SAMP_MAX 300000u
#define SAMP_CLASSES 9
static HANDLE s_samp_target;
static uint64_t *s_samp_rip[SAMP_CLASSES];
static uint32_t s_samp_n[SAMP_CLASSES];
static uint64_t s_samp_frames[SAMP_CLASSES];
static double s_samp_ms[SAMP_CLASSES];
static DWORD s_samp_tid, s_samp_start_tick;

/* Who called: the guest frame chain. Generated code publishes its frame to
 * g_ebp (thread-local) before every call, and each frame holds [ebp] = the
 * caller's ebp and [ebp+4] = the return address, so walking from the game
 * thread's g_ebp names the callers of whatever was running (all but the
 * innermost, which has not published its frame). Every distinct return
 * address of a sample is counted once per class; symsamples.py maps them to
 * functions. */
extern __declspec(thread) uint32_t g_ebp;
extern ptrdiff_t g_xbox_mem_offset;
#define SAMP_RET_SLOTS 32768u
#define SAMP_CHAIN 24
static volatile uint32_t *s_samp_ebp_ptr;
static struct { uint32_t key, cnt; } *s_samp_ret[SAMP_CLASSES];

static void samp_count_ret(int cls, uint32_t key)
{
    uint32_t h = (key * 2654435761u) >> 17, n;
    for (n = 0; n < 64; n++, h = (h + 1) & (SAMP_RET_SLOTS - 1)) {
        if (s_samp_ret[cls][h].key == key) {
            s_samp_ret[cls][h].cnt++;
            return;
        }
        if (!s_samp_ret[cls][h].key) {
            s_samp_ret[cls][h].key = key;
            s_samp_ret[cls][h].cnt = 1;
            return;
        }
    }
}

/* The frame in progress: its samples wait here until the frame's own time is
 * known, then all go to the class of that time (a sample filed by the class
 * of the frame before it put every isolated slow frame under "fast"). */
#define SAMP_FRAME_MAX 256
static struct { uint64_t rip; uint32_t n, ret[SAMP_CHAIN]; } s_samp_fr[SAMP_FRAME_MAX];
static uint32_t s_samp_nfr;

static void samp_walk(uint32_t *seen, uint32_t *nseen)
{
    uint32_t ebp = *s_samp_ebp_ptr, ns = 0, i, k;
    for (i = 0; i < SAMP_CHAIN; i++) {
        uint32_t ret, next;
        if (ebp < 0x10000u || ebp >= 0x08000000u || (ebp & 3))
            break;
        __try {
            next = *(volatile uint32_t *)((uintptr_t)ebp + (uintptr_t)g_xbox_mem_offset);
            ret = *(volatile uint32_t *)((uintptr_t)ebp + 4 + (uintptr_t)g_xbox_mem_offset);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        if (ret < 0x10000u || ret >= 0x00400000u)
            break;
        for (k = 0; k < ns && seen[k] != ret; k++)
            ;
        if (k == ns) {
            seen[ns++] = ret;
        }
        if (next <= ebp)
            break;
        ebp = next;
    }
    *nseen = ns;
}

static int samp_cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void samp_dump(void)
{
    FILE *f = fopen("samples_game.txt", "w");
    HMODULE exe = GetModuleHandleA(NULL);
    uint64_t total = 0;
    int c;
    if (!f)
        return;
    for (c = 0; c < SAMP_CLASSES; c++)
        total += s_samp_n[c];
    fprintf(f, "exe_base %llx\nthread %lu samples %llu seconds %.1f\n",
            (unsigned long long)(uintptr_t)exe, (unsigned long)s_samp_tid,
            (unsigned long long)total,
            (double)(GetTickCount() - s_samp_start_tick) / 1000.0);
    for (c = 0; c < SAMP_CLASSES; c++) {
        uint64_t *s = (uint64_t *)malloc((size_t)s_samp_n[c] * 8 + 8);
        uint32_t i = 0, n = s_samp_n[c];
        if (!s)
            continue;
        memcpy(s, s_samp_rip[c], (size_t)n * 8);
        qsort(s, n, 8, samp_cmp);
        fprintf(f, "class %d samples %u frames %llu ms %.1f\n", c, n,
                (unsigned long long)s_samp_frames[c], s_samp_ms[c]);
        {
            uint32_t r;
            for (r = 0; r < SAMP_RET_SLOTS; r++)
                if (s_samp_ret[c][r].key)
                    fprintf(f, "R %x %u\n", s_samp_ret[c][r].key, s_samp_ret[c][r].cnt);
        }
        while (i < n) {
            uint32_t j = i;
            while (j < n && s[j] == s[i])
                j++;
            if (s[i] - (uint64_t)(uintptr_t)exe < 0x4000000ull) {
                fprintf(f, "E %llx %u\n", (unsigned long long)(s[i] - (uint64_t)(uintptr_t)exe),
                        j - i);
            } else {
                HMODULE m = NULL;
                char nm[MAX_PATH] = "?";
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                       | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCSTR)(uintptr_t)s[i], &m) && m) {
                    char *b;
                    GetModuleFileNameA(m, nm, MAX_PATH);
                    b = strrchr(nm, '\\');
                    memmove(nm, b ? b + 1 : nm, strlen(b ? b + 1 : nm) + 1);
                    fprintf(f, "X %s %llx %u\n", nm,
                            (unsigned long long)(s[i] - (uint64_t)(uintptr_t)m), j - i);
                } else {
                    fprintf(f, "X ? %llx %u\n", (unsigned long long)s[i], j - i);
                }
            }
            i = j;
        }
        free(s);
    }
    fclose(f);
}

static DWORD WINAPI samp_thread(LPVOID arg)
{
    LONGLONG step;
    LARGE_INTEGER qf, t0, now, next;
    uint32_t last_frames = g_game_frames;
    double frame_ms = 10.0;
    DWORD last_dump = GetTickCount();
    QueryPerformanceFrequency(&qf);
    step = qf.QuadPart / (int)(intptr_t)arg;
    QueryPerformanceCounter(&t0);
    next = t0;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    for (;;) {
        CONTEXT cx;
        uint32_t g;
        float dm;
        int cls;
        next.QuadPart += step;
        do {
            YieldProcessor();
            QueryPerformanceCounter(&now);
        } while (now.QuadPart < next.QuadPart);
        if (!s_samp_target)
            continue;
        g = g_game_frames;
        if (g != last_frames) {
            uint32_t q, r;
            frame_ms = (double)(now.QuadPart - t0.QuadPart) * 1000.0 / (double)qf.QuadPart
                       / (double)(g - last_frames);
            dm = game_draw_mult_now();
            cls = (dm <= 1.5f ? 0 : dm >= 4.0f ? 2 : 1) * 3
                  + (frame_ms < 9.0 ? 0 : frame_ms < 13.0 ? 1 : 2);
            s_samp_frames[cls] += g - last_frames;
            s_samp_ms[cls] += frame_ms * (double)(g - last_frames);
            for (q = 0; q < s_samp_nfr && s_samp_n[cls] < SAMP_MAX; q++) {
                s_samp_rip[cls][s_samp_n[cls]++] = s_samp_fr[q].rip;
                for (r = 0; r < s_samp_fr[q].n; r++)
                    samp_count_ret(cls, s_samp_fr[q].ret[r]);
            }
            s_samp_nfr = 0;
            last_frames = g;
            t0 = now;
        }
        if (s_samp_nfr >= SAMP_FRAME_MAX)
            continue;
        if (SuspendThread(s_samp_target) == (DWORD)-1)
            continue;
        memset(&cx, 0, sizeof cx);
        cx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(s_samp_target, &cx)) {
            s_samp_fr[s_samp_nfr].rip = cx.Rip;
            s_samp_fr[s_samp_nfr].n = 0;
            if (s_samp_ebp_ptr)
                samp_walk(s_samp_fr[s_samp_nfr].ret, &s_samp_fr[s_samp_nfr].n);
            s_samp_nfr++;
        }
        ResumeThread(s_samp_target);
        if (GetTickCount() - last_dump > 15000) {
            last_dump = GetTickCount();
            samp_dump();
        }
    }
    return 0;
}

/* Called on the thread that runs the guest, just before it starts. */
static void game_sampler_start(void)
{
    const char *e = getenv("RECOMP_SAMPLE");
    int hz = e ? atoi(e) : 0, c;
    if (hz <= 0)
        return;
    if (hz > 1000)
        hz = 1000;
    for (c = 0; c < SAMP_CLASSES; c++) {
        s_samp_rip[c] = (uint64_t *)malloc((size_t)SAMP_MAX * 8);
        s_samp_ret[c] = calloc(SAMP_RET_SLOTS, sizeof *s_samp_ret[c]);
        if (!s_samp_rip[c] || !s_samp_ret[c])
            return;
    }
    s_samp_start_tick = GetTickCount();
    CloseHandle(CreateThread(NULL, 0, samp_thread, (LPVOID)(intptr_t)hz, 0, NULL));
    fprintf(stderr, "[SAMPLE] waiting for the game loop, then %d samples a second"
            " -> samples_game.txt\n", hz);
}

/* RECOMP_PIN_GAME=<n>: the game loop runs alone on physical core n (counting from 0; both its hardware
 * threads), every other thread of the process on the remaining cores. The recompiled game is one thread
 * and its speed is the frame rate; the executor, the flush-ack thread and the vertex pool spin, and a
 * spinning thread on the game thread's SMT sibling (or one that takes its core over for a moment) costs it
 * a third of its speed. Uses CPU sets (Windows 10 1703+): the process default excludes the core, the game
 * thread selects it. */
static int s_pin_core = 2, s_pin_on;

/* Pins (on) or releases (off) the calling thread, the game loop. */
static void game_pin_set(int on)
{
    ULONG len = 0, off, n = 0, ng = 0;
    unsigned char *buf;
    ULONG *others, *mine;
    int core = s_pin_core;
    if (!on) {
        SetThreadSelectedCpuSets(GetCurrentThread(), NULL, 0);
        SetProcessDefaultCpuSets(GetCurrentProcess(), NULL, 0);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        s_pin_on = 0;
        return;
    }
    GetSystemCpuSetInformation(NULL, 0, &len, GetCurrentProcess(), 0);
    if (!len || !(buf = (unsigned char *)malloc(len)))
        return;
    if (!GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf, len, &len, GetCurrentProcess(), 0)) {
        free(buf);
        return;
    }
    others = (ULONG *)malloc(256 * sizeof *others);
    mine = (ULONG *)malloc(16 * sizeof *mine);
    for (off = 0; others && mine && off < len;) {
        PSYSTEM_CPU_SET_INFORMATION p = (PSYSTEM_CPU_SET_INFORMATION)(buf + off);
        if (!p->Size)
            break;
        if (p->Type == CpuSetInformation) {
            if (p->CpuSet.CoreIndex == (BYTE)core) {
                if (ng < 16)
                    mine[ng++] = p->CpuSet.Id;
            } else if (n < 256) {
                others[n++] = p->CpuSet.Id;
            }
        }
        off += p->Size;
    }
    if (others && mine && ng && n) {
        SetProcessDefaultCpuSets(GetCurrentProcess(), others, n);
        SetThreadSelectedCpuSets(GetCurrentThread(), mine, 1);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        s_pin_on = 1;
    } else {
        fprintf(stderr, "[PIN] core %d not found; not pinned\n", core);
    }
    free(buf);
    free(others);
    free(mine);
}

/* Hitch watchdog (always on, RECOMP_HITCH=0 disables): the game loop stamps a heartbeat once per frame;
 * a helper thread checks it every 20 ms and, when the loop has been silent for 200 ms or more, freezes the
 * loop's thread for a moment to read where it is (exe offset + guest callers), then resumes it. One
 * [HITCH] line per stall, one more when the loop is running again, at most 400 lines. */
static volatile LONGLONG s_hw_beat;
static HANDLE s_hw_target;
static LONGLONG s_hw_qf;

static LONGLONG hw_now(void)
{
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return n.QuadPart;
}

/* During a stall: which threads of the process are busy, and where. Two cycle-count reads 25 ms apart rank
 * the threads; the busiest few are frozen for a moment to read their instruction pointer, named as
 * module+offset (our exe, d3d11, the GPU driver, ...). One [HITCH-THREADS] line each. */
#include <tlhelp32.h>
static void hitch_scan(void)
{
    struct { DWORD id; HANDLE h; ULONG64 c0, d; } th[160];
    int n = 0, i, j, shown = 0;
    DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    char out[10][420];
    if (snap == INVALID_HANDLE_VALUE)
        return;
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me || n >= 160)
                continue;
            th[n].h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                                 FALSE, te.th32ThreadID);
            if (!th[n].h)
                continue;
            th[n].id = te.th32ThreadID;
            th[n].c0 = 0;
            QueryThreadCycleTime(th[n].h, &th[n].c0);
            n++;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    Sleep(25);
    for (i = 0; i < n; i++) {
        ULONG64 c1 = 0;
        QueryThreadCycleTime(th[i].h, &c1);
        th[i].d = c1 - th[i].c0;
    }
    for (j = 0; j < 10; j++) {
        int best = -1, nf = 0, f;
        unsigned long long fr[10];
        for (i = 0; i < n; i++)
            if (th[i].id && (best < 0 || th[i].d > th[best].d))
                best = i;
        if (best < 0)
            break;
        {
            CONTEXT cx;
            char line[420];
            int len;
            memset(&cx, 0, sizeof cx);
            cx.ContextFlags = CONTEXT_FULL;
            if (SuspendThread(th[best].h) != (DWORD)-1) {
                if (GetThreadContext(th[best].h, &cx)) {
                    for (nf = 0; nf < 10 && cx.Rip; nf++) {
                        DWORD64 base = 0, hdata = 0;
                        PVOID ectx = NULL;
                        PRUNTIME_FUNCTION fe;
                        fr[nf] = cx.Rip;
                        fe = RtlLookupFunctionEntry(cx.Rip, &base, NULL);
                        if (!fe) {
                            cx.Rip = *(DWORD64 *)cx.Rsp;
                            cx.Rsp += 8;
                        } else
                            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, cx.Rip, fe, &cx, &ectx, &hdata, NULL);
                    }
                }
                ResumeThread(th[best].h);
            }
            len = _snprintf(line, sizeof line - 1, "[HITCH-THREADS] tid %lu busy %llu Mcycles in 25 ms:",
                            (unsigned long)th[best].id, th[best].d / 1000000ull);
            for (f = 0; f < nf && len > 0 && len < 330; f++) {
                HMODULE mod = NULL;
                char name[MAX_PATH] = "?", *b2;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)fr[f], &mod)
                    && GetModuleFileNameA(mod, name, sizeof name)) {
                    b2 = strrchr(name, '\\');
                    if (b2)
                        memmove(name, b2 + 1, strlen(b2));
                    if (strlen(name) > 12)
                        name[12] = 0;
                }
                len += _snprintf(line + len, sizeof line - 1 - len, " %s+%llx", name,
                                 fr[f] - (unsigned long long)(uintptr_t)mod);
            }
            line[sizeof line - 1] = 0;
            strncpy(out[shown], line, sizeof out[0] - 1);
            out[shown][sizeof out[0] - 1] = 0;
            shown++;
            th[best].id = 0;          /* done; the handle stays for closing */
        }
    }
    for (i = 0; i < n; i++)
        CloseHandle(th[i].h);
    for (i = 0; i < shown; i++)
        fprintf(stderr, "%s\n", out[i]);
}

static DWORD WINAPI hitch_thread(LPVOID unused)
{
    int stalled = 0, lines = 0;
    LONGLONG t0 = 0;
    (void)unused;
    for (;;) {
        LONGLONG beat, now, ms;
        Sleep(20);
        beat = s_hw_beat;
        now = hw_now();
        ms = (now - beat) * 1000 / s_hw_qf;
        if (!stalled && ms >= 200 && lines < 600) {
            CONTEXT cx;
            uint32_t chain[SAMP_CHAIN], n = 0, i;
            char buf[512];
            int len;
            unsigned long long rip = 0;
            stalled = 1;
            t0 = beat;
            memset(&cx, 0, sizeof cx);
            cx.ContextFlags = CONTEXT_CONTROL;
            if (SuspendThread(s_hw_target) != (DWORD)-1) {
                if (GetThreadContext(s_hw_target, &cx))
                    rip = cx.Rip;
                if (s_samp_ebp_ptr)
                    samp_walk(chain, &n);
                ResumeThread(s_hw_target);
            }
            len = _snprintf(buf, sizeof buf - 1, "[HITCH] t=%lu game loop silent for %lld ms, rip exe+0x%llx, guest callers:",
                            (unsigned long)GetTickCount(), ms,
                            rip - (unsigned long long)(uintptr_t)GetModuleHandleA(NULL));
            for (i = 0; i < n && len > 0 && len < 440; i++)
                len += _snprintf(buf + len, sizeof buf - 1 - len, " %05X", chain[i]);
            fprintf(stderr, "%.*s\n", len > 0 ? len : 0, buf);
            {
                char kd[300];
                extern void xbox_Nv2aKickDiag(char *, size_t);
                xbox_Nv2aKickDiag(kd, sizeof kd);
                fprintf(stderr, "[HITCH-KICK] %s\n", kd);
            }
            lines++;
            hitch_scan();
        } else if (stalled && ms < 100) {
            stalled = 0;
            fprintf(stderr, "[HITCH] t=%lu running again after about %lld ms\n", (unsigned long)GetTickCount(),
                    (beat - t0) * 1000 / s_hw_qf);
            lines++;
        }
    }
    return 0;
}

static void hitch_note(void)
{
    static int started;
    LARGE_INTEGER q;
    if (!started) {
        const char *e = getenv("RECOMP_HITCH");
        started = 1;
        if (e && *e == '0')
            return;
        QueryPerformanceFrequency(&q);
        s_hw_qf = q.QuadPart;
        if (!s_samp_ebp_ptr)
            s_samp_ebp_ptr = &g_ebp;
        s_hw_beat = hw_now();
        if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                            &s_hw_target, 0, FALSE, DUPLICATE_SAME_ACCESS))
            CloseHandle(CreateThread(NULL, 0, hitch_thread, NULL, 0, NULL));
        return;
    }
    if (s_hw_qf)
        s_hw_beat = hw_now();
}

extern volatile LONG g_mkb_pin_toggles, g_mkb_cap_toggles;      /* F8 / F12 (mkb_input.c) */
extern int game_cap_state(int toggle);
extern void game_draw_adapt(double ema_ms);
extern float game_draw_eff_now(void);

/* Called by the frame limiter (sub_000137C0) once per game frame, on the game loop's thread.
 *  - the first call: RECOMP_PIN_GAME=<n> (physical core of the game loop; 0 or unset = off), the sampler's
 *    thread handle;
 *  - F8 / F12 switch the pinning / the small-object distance cap on and off while playing, so one busy spot
 *    gives a clean before-and-after;
 *  - the frame times: every 120 frames one [FRAMES] line with the average fps, the 1% low (the slowest
 *    frame of each hundred), how many frames took over 16.7 ms (under 60 fps) and over 25 ms, and the
 *    longest, with the two switches' state. Half-second averages hid dips that last a few frames. */
extern void xbox_GuestCpuSetGameThread(unsigned long tid);
extern void xbox_GuestCpuGameWaitGet(double *total_ms, long *count, double *max_ms);
void game_sampler_note_thread(void)
{
    static int started;
    static LARGE_INTEGER last, qf;
    static double dt[120];
    static int nd;
    LARGE_INTEGER now;
    LONG t;
    hitch_note();
    if (!started) {
        const char *e = getenv("RECOMP_PIN_GAME");
        started = 1;
        xbox_GuestCpuSetGameThread(GetCurrentThreadId());
        if (e && *e) {
            int c = atoi(e);
            if (c > 0) {
                s_pin_core = c;
                game_pin_set(1);
                if (s_pin_on)
                    fprintf(stderr, "[PIN] game loop on physical core %d, the rest of the process elsewhere"
                            " (F8 switches it)\n", c);
            }
        }
        QueryPerformanceFrequency(&qf);
        QueryPerformanceCounter(&last);
        if (!s_samp_target && s_samp_rip[0]
            && DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                               &s_samp_target, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            s_samp_tid = GetCurrentThreadId();
            s_samp_ebp_ptr = &g_ebp;
            fprintf(stderr, "[SAMPLE] sampling the game loop's thread %lu\n",
                    (unsigned long)s_samp_tid);
        }
        return;
    }
    if ((t = InterlockedExchange(&g_mkb_pin_toggles, 0)) & 1) {
        game_pin_set(!s_pin_on);
        fprintf(stderr, "[PIN] game loop pinned to its own core: %s\n", s_pin_on ? "ON" : "off");
    }
    if (InterlockedExchange(&g_mkb_cap_toggles, 0) & 1)
        fprintf(stderr, "[DRAW] small-object distance cap: %s\n", game_cap_state(1) ? "ON" : "off");
    QueryPerformanceCounter(&now);
    dt[nd++] = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)qf.QuadPart;
    last = now;
    {
        static double ema = 8.5;
        ema += 0.15 * ((dt[nd - 1] < 50.0 ? dt[nd - 1] : 50.0) - ema);
        game_draw_adapt(ema);              /* RECOMP_DRAW_ADAPT (recomp_manual.c) */
    }
    if (nd == 120) {
        double sum = 0, mx = 0, tmp[120];
        int i, j, o60 = 0, o40 = 0;
        for (i = 0; i < 120; i++) {
            sum += dt[i];
            if (dt[i] > mx)
                mx = dt[i];
            if (dt[i] > 16.7)
                o60++;
            if (dt[i] > 25.0)
                o40++;
            tmp[i] = dt[i];
        }
        for (i = 1; i < 120; i++) {                      /* insertion sort, ascending */
            double v = tmp[i];
            for (j = i - 1; j >= 0 && tmp[j] > v; j--)
                tmp[j + 1] = tmp[j];
            tmp[j + 1] = v;
        }
        {
            double gw_ms, gw_max;
            long gw_n;
            xbox_GuestCpuGameWaitGet(&gw_ms, &gw_n, &gw_max);
            fprintf(stderr, "[FRAMES] 120: avg %.0f fps, 1%% low %.0f fps, over 16.7 ms: %d, over 25 ms: %d,"
                    " longest %.1f ms | pin %s cap %s draw x%.1f (in use x%.1f) | cpu-lock wait %.2f ms/frame"
                    " (%ld waits, longest %.1f ms)\n",
                    120000.0 / sum, 1000.0 / tmp[118], o60, o40, mx, s_pin_on ? "ON" : "off",
                    game_cap_state(0) ? "ON" : "off", game_draw_mult_now(), game_draw_eff_now(),
                    gw_ms / 120.0, gw_n, gw_max);
        }
        nd = 0;
    }
}

/* ── VEH crash handler ─────────────────────────────────────── */

/*
 * Vectored Exception Handler for crash diagnostics.
 *
 * When the recompiled game hits an access violation, this handler prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
/* Name the guest function a fault happened in, and recover the call chain.
 *
 * Recompiled code faults as ordinary native code, so the exception record
 * carries a host RIP and nothing else -- there is no guest program counter to
 * report, and the host address changes every build. Two things recover the
 * guest view:
 *
 *   - every generated function is a real symbol in the image (sub_005A03C0
 *     and so on), so the linker's PDB already maps host address back to guest
 *     function. dbghelp turns an anonymous RIP into that name.
 *
 *   - every lifted call pushes its guest return address onto the guest stack
 *     before jumping, so the stack still holds the chain. Scanning up from esp
 *     for values inside the code sections recovers it.
 *
 * ponytail: the stack scan is a scan, not a frame walk -- these are FPO frames
 * with no reliable ebp chain, so there is nothing to walk. It over-reports,
 * since addresses from returned-from calls linger below esp, but naming the
 * guest function is the whole question at a fault.
 *
 * Requires linking dbghelp and keeping the .pdb beside the .exe.
 */
/* One host address by symbol, "label name+0xoff", or the bare address. */
static void crash_name(const char *label, uintptr_t a)
{
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)a, &disp, sym))
        fprintf(stderr, "%s %s+0x%llX\n", label, sym->Name,
                (unsigned long long)disp);
    else
        fprintf(stderr, "%s 0x%llX\n", label, (unsigned long long)a);
}

static void print_guest_context(void *rip)
{
    /* SYMBOL_INFO is variable-length: the name is written past the struct, so
     * it must be over-allocated with MaxNameLen set to the slack. */
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n",
                sym->Name, (unsigned long long)disp);

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        /* When the frame is gone, say so, and fall back on what is left.
         *
         * A fault inside a block copy leaves no return address in range: the
         * copy is a host memmove and its caller's frame is further up than
         * this reaches. Worse, a copy that ran past its buffer overwrites the
         * stack itself, so the list comes out empty exactly when it is most
         * needed -- and an empty list under a promising heading reads like
         * "nothing to see" rather than "the evidence is destroyed". */
        if (!shown)
            fprintf(stderr, "    (no return addresses in range)\n");

        /* Everything below only pays for itself when the scan came back
         * empty. A crash where the chain was recovered already says what
         * happened, and burying that under another twenty lines makes the
         * common report worse to read. */
        if (!shown) {
            uint32_t k;

            /* The indirect-call history, which the stack cannot overwrite.
             * recomp_manual.c prints this too, but only from
             * recomp_icall_fail_log -- a page fault inside a block copy
             * never reaches that path, and this is the same information by
             * the route the fault actually took.
             *
             * Declared here rather than included: main.c does not pull in
             * recomp_types.h, and recomp_manual.c spells it the same way. */
            extern volatile uint32_t g_icall_trace[16];
            extern volatile uint32_t g_icall_trace_idx;

            fprintf(stderr, "  recent ICALL targets:");
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx + k) & 15]);
            fprintf(stderr, "\n");

            /* The frame unfiltered. The pointers and lengths handed to
             * whatever faulted live here and look nothing like code, so the
             * filter above drops exactly what is wanted. On the crash this
             * was written for, one of these words was the end of the mapped
             * RAM mirror -- which is what identified the fault as a copy
             * running off its buffer rather than a stray pointer. */
            for (i = 0; i < 20; i++)
                fprintf(stderr, "    raw[esp+%-4d] 0x%08X\n", i * 4, sp[i]);
        }
    }
}

/* The emulated APU.
 *
 * Two entry points that had no callers anywhere in the tree. apu_mmio_hook.c
 * documents apu_hook_handle_mmio as "called from VEH in main.c" and no main.c
 * called it; apu_decode_and_handle's first line is `if (!g_apu_state) return
 * false` and nothing assigned g_apu_state. So with RECOMP_AC97_READY set the
 * APU's registers were unmapped to be trapped, and every trap was then
 * declined and surfaced as an access violation on the first register
 * DirectSound touched. The VEH below now reaches apu_hook_handle_mmio
 * through the toolkit's xbox_VehMmioFault.
 *
 * Declared rather than included so this does not depend on src/apu being on
 * the include path. */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;

/* A crash report somewhere it survives. Started from Explorer, stderr goes
 * nowhere and a crash left no trace at all; then the report goes to
 * crash.txt beside the exe instead. Redirected to a file, it stays there. */
static void crash_to_file(void)
{
    static int done;
    char path[MAX_PATH];
    DWORD n;
    char *slash;

    if (done)
        return;
    done = 1;
    if (GetFileType(GetStdHandle(STD_ERROR_HANDLE)) == FILE_TYPE_DISK)
        return;
    n = GetModuleFileNameA(NULL, path, MAX_PATH - 16);
    if (!n || n >= MAX_PATH - 16)
        return;
    slash = strrchr(path, '\\');
    strcpy(slash ? slash + 1 : path, "crash.txt");
    freopen(path, "w", stderr);
}

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO) {
        crash_to_file();
        fprintf(stderr, "[CRASH] Integer divide by zero at RIP=0x%llX\n",
            (unsigned long long)ep->ContextRecord->Rip);
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        print_guest_context((void *)ep->ContextRecord->Rip);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

        /*
         * GPU register probe at 0xFD000000 range.
         * Some games probe NV2A registers directly. On real hardware this
         * returns GPU state; here we just skip the instruction.
         * TODO: Implement mini x86-64 decoder for instruction skipping,
         * or connect to the xbox_nv2a library for proper handling.
         */
        if (fault_addr >= 0xFD000000 && fault_addr < 0xFE000000) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        /* Trapped hardware registers (APU, USB OHCI) -> their models.
         *
         * The other half of RECOMP_AC97_READY: reporting the codec ready is
         * what lets DirectSoundCreate past its first gate, and from there it
         * drives the APU directly, so the registers have to fault to be seen.
         * "Handled" means the access was decoded and the instruction stepped
         * over, so execution resumes instead of unwinding. */
        if (xbox_VehMmioFault(ep))
            return EXCEPTION_CONTINUE_EXECUTION;

        crash_to_file();
        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)fault_addr,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        print_guest_context((void *)ep->ContextRecord->Rip);

        /*
         * TODO: Add game-specific diagnostics here. Examples:
         *
         * Dump CRT heap handle:
         *   uint32_t heap = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + HEAP_HANDLE_VA);
         *   fprintf(stderr, "  CRT heap handle: 0x%08X\n", heap);
         *
         * Dump game state:
         *   uint32_t state = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + GAME_STATE_VA);
         *   fprintf(stderr, "  Game state: %u\n", state);
         */

        /* The faulting function and the return addresses on the native
         * stack that point into this image, by name (the PDB beside the
         * exe). The image is ASLR-placed, so its range comes from its own
         * header rather than the fixed 0x140000000 the old filter assumed,
         * which never matched and printed nothing. */
        fflush(stderr);
        {
            HMODULE self = GetModuleHandleA(NULL);
            const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)
                ((const BYTE *)self + ((const IMAGE_DOS_HEADER *)self)->e_lfanew);
            uintptr_t lo = (uintptr_t)self, hi = lo + nt->OptionalHeader.SizeOfImage;
            uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
            int shown = 0;
            crash_name("  at", (uintptr_t)ep->ContextRecord->Rip);
            for (int i = 0; i < 512 && shown < 12; i++) {
                if (sp[i] >= lo && sp[i] < hi) {
                    crash_name("  from", sp[i]);
                    shown++;
                }
            }
        }
        fflush(stderr);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/* ── WinMain ───────────────────────────────────────────────── */

static DWORD WINAPI stderr_flush_thread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        Sleep(500);
        fflush(stderr);
    }
    return 0;
}

void rs_startup(void);
void rs_install(void);

#ifdef RECOMP_PGI
/* The instrumented (profile-guided) build: closing the window ends the process with TerminateProcess, which
 * skips the runtime's own write-out, so the exit hook writes the recorded profile first. */
extern void recomp_exit_hook_set2(void (*fn)(const char *));
static DWORD WINAPI pgo_kill_thread(LPVOID unused)
{
    (void)unused;
    Sleep(25000);                       /* exit() hung on a lock a guest thread holds: end it anyway */
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

static void pgo_sweep_at_exit(const char *why)
{
    (void)why;
    CloseHandle(CreateThread(NULL, 0, pgo_kill_thread, NULL, 0, NULL));
    fprintf(stderr, "[PGO] exit() to write the profile\n");
    fflush(stderr);
    exit(0);                            /* the profile runtime writes its .pgc from its exit handler */
}
#endif

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;
#ifdef RECOMP_PGI
    recomp_exit_hook_set2(pgo_sweep_at_exit);
#endif

    rs_startup();      /* recomp_settings.ini -> the environment (frame rate, resolution, fullscreen, Super Accuracy) */
    rs_install();      /* the Credits item of Options opens the Recomp Settings screen */

    /* Unbuffered output for immediate visibility during debugging */
    setvbuf(stdout, NULL, _IONBF, 0);
    /* stderr buffered, flushed twice a second by its own thread. Unbuffered,
     * every line was a WriteFile of its own; into a redirected file under
     * real-time scanning that ran at ~12 KB/s, and an F9 frame capture (a few
     * MB of [FTRACE]) froze the game for minutes. The window's close path
     * and the crash handler flush before they end the process. */
    setvbuf(stderr, NULL, _IOFBF, 1u << 20);
    CloseHandle(CreateThread(NULL, 0, stderr_flush_thread, NULL, 0, NULL));

    /* This title builds KEVENTs itself (no KeInitializeEvent); without host
     * shadows its KeWaitForMultipleObjects fails at once and spins forever. */
    if (!getenv("RECOMP_TITLE_KEVENTS"))
        _putenv("RECOMP_TITLE_KEVENTS=1");
    /* D3D's fence waits need the pushbuffer executed (see the
     * sub_002DE0C0_enter hook in recomp_manual.c). */
    if (!getenv("RECOMP_PB_EXEC"))
        _putenv("RECOMP_PB_EXEC=1");
    /* D3D's mode set waits on the vblank event at device+0x1DBC. */
    if (!getenv("RECOMP_VBLANK"))
        _putenv("RECOMP_VBLANK=paced");
    /* Without the codec DirectSound divides by zero in its init. */
    if (!getenv("RECOMP_AC97_READY"))
        _putenv("RECOMP_AC97_READY=1");
    /* One guest CPU, as on the console: without it the loader threads race
     * and boot crashes in a different place every run. Needs the code lifted
     * with --back-edge-yield so a spinning loop can be preempted. */
    if (!getenv("RECOMP_GUEST_LOCK"))
        _putenv("RECOMP_GUEST_LOCK=1");
    /* DirectSound guards its voice lists by raising to DISPATCH_LEVEL; only
     * the one-CPU gate keeps its DPC and interrupt routine out of a list a
     * thread is relinking (the routine crashed on a NULL node entering the
     * campaign without it). */
    if (!getenv("RECOMP_DISPATCH_GATE"))
        _putenv("RECOMP_DISPATCH_GATE=1");
    /* Emulated USB controllers, so the title sees a gamepad. */
    if (!getenv("RECOMP_USB"))
        _putenv("RECOMP_USB=1");
    /* Split-screen pads: the emulated bus plugs in ONE controller unless told
     * otherwise; plug in four when the split-screen option is on. */
    { const char *sp = getenv("RECOMP_SPLIT_PADS");
      if (sp && *sp && *sp != '0' && !getenv("RECOMP_USB_PADS")) _putenv("RECOMP_USB_PADS=4"); }
    /* The movies play again; Recomp Settings > Cutscenes lets a button skip them
     * (recomp_manual.c, game_movie_skip_pressed) or hides them (the ini's
     * cutscenes=off, or RECOMP_SKIP_MOVIES=1 when the ini has no choice). */
    /* Show the framebuffer in a window. */
    if (!getenv("RECOMP_FB_WINDOW"))
        _putenv("RECOMP_FB_WINDOW=1");
    /* Pixels on the host GPU (src/kernel/nv2a_gpu.c); RECOMP_GPU=0 goes back
     * to the software rasteriser. */
    if (!getenv("RECOMP_GPU"))
        _putenv("RECOMP_GPU=1");
    /* RECOMP_FAST_FLUSH_ACK=1 lets the title build the next frame while the
     * executor draws this one (xbox_memory_layout.c's nv2a_flush_ack_thread).
     * Off: at 60 fps it drew stray shapes in fixed spots -- the title rewrote
     * something the executor had not read yet -- and with the executor as
     * fast as it is now it gained nothing measurable. */
    /* The bounded version: the kick is answered at once while the executor
     * is within 256 KB of commands of the title, so the two overlap a little
     * without the title ever getting frames ahead (no stray shapes, and the
     * title's half-time spin in KickOff mostly gone). =0 turns it off. */
    if (!getenv("RECOMP_FLUSH_AHEAD"))
        _putenv("RECOMP_FLUSH_AHEAD=262144");
    /* RECOMP_KICK_STATS=1 prints one [KICK] line every 5 s (how the kick was answered): a diagnostic, off by default. */
    /* The music and speech stream (a 2400-sample loop the title refills just
     * ahead of the position it reads, voices with buffer base 0x1E70..78)
     * is read 20 ms behind that position (apu_vp.c, RECOMP_APU_LAG): the
     * title's late refills had it whistle and crackle over every song, in
     * the menu and in the levels (6.3 discontinuities a second -> 0).
     * RECOMP_APU_LAG=0 turns it off. */
    if (!getenv("RECOMP_APU_LAG"))
        _putenv("RECOMP_APU_LAG=960");
    if (!getenv("RECOMP_APU_LAG_BA"))
        _putenv("RECOMP_APU_LAG_BA=00001E70-00001E7C");
    /* ...and wherever else the stream's 2400-sample loop is placed: its
     * buffer base is DirectSound's choice and need not stay 0x1E70. */
    if (!getenv("RECOMP_APU_LAG_LOOP"))
        _putenv("RECOMP_APU_LAG_LOOP=2400");

    printf("=== YOUR_GAME_NAME - Static Recompilation ===\n");
    printf("Loading XBE...\n");

    /* Install VEH handler (first handler in chain) */
    /* Load symbols up front rather than from inside the handler: at fault
     * time the process is already in a bad way, and SymInitialize
     * allocates. Failure is not fatal -- the handler prints no name. */
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);

    /* Step 1: Load XBE */
    if (!load_xbe(YOUR_GAME_XBE_PATH, &xbe_data, &xbe_size)) {
        MessageBoxA(NULL, "Failed to load default.xbe.\n"
                    "Place the game files in the 'game' subdirectory.",
                    "Recomp", MB_ICONERROR);
        return 1;
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing Xbox memory layout...\n");
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        MessageBoxA(NULL, "Failed to initialize Xbox memory layout.\n"
                    "The required virtual address range may be unavailable.",
                    "Recomp", MB_ICONERROR);
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);

    /* Bring up the emulated APU.
     *
     * Gated on the same variable that unmaps its registers, because the two
     * halves are useless apart: a trap with no model declines every access,
     * and a model nothing traps into never sees a register. The APU walks
     * Xbox physical RAM to find voice buffers, so it gets the guest RAM
     * base. */
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n",
                g_apu_state ? "up" : "FAILED to initialise");
    }

    /* The XDK's XInput drives the USB host controllers directly; the OHCI
     * model (RECOMP_USB) answers them with a pad fed from the host's
     * keyboard and XInput pads. */
    {
        extern void xbox_OhciInit(void);
        xbox_OhciInit();
    }

    /* Step 3: Initialize Xbox kernel */
    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();

    /* Step 4: Set game directory for file I/O path translation */
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        WCHAR root_w[MAX_PATH];
        char root[MAX_PATH * 4];
        const char *save = NULL;

        if (game_save_root_w(root_w, MAX_PATH)) {
            CreateDirectoryW(root_w, NULL);
            migrate_saves_from_appdata(root_w);
            if (WideCharToMultiByte(CP_UTF8, 0, root_w, -1, root, sizeof root, NULL, NULL))
                save = root;
        }
        xbox_path_init(YOUR_GAME_DIR, save);
    }

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    /*
     * TODO: Pre-initialize CRT globals if needed.
     *
     * Many Xbox games use the MSVC CRT. The CRT's __heap_init sets up a
     * heap descriptor at a game-specific address. You may need to
     * pre-initialize __active_heap to avoid small-block heap issues:
     *
     *   uint32_t *active_heap = (uint32_t *)((uint8_t *)g_xbox_mem_offset + ACTIVE_HEAP_VA);
     *   *active_heap = 1;  // 1 = system heap (HeapAlloc), avoids SBH init
     *
     * Find ACTIVE_HEAP_VA by searching the disassembly for __heap_init
     * or by looking for the CRT's __active_heap global in the data section.
     */

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", YOUR_GAME_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* Build the flat dispatch table before the guest runs.
     *
     * Without this recomp_lookup falls back to a binary search over the
     * whole function table -- roughly log2(n) branches on *every*
     * indirect call, which for a 45,000-function C++ title is about 16
     * every time the game goes through a vtable. Half-Life 2 spent most
     * of its static initialisation inside recomp_lookup for exactly this
     * reason, and it read as a hang.
     *
     * Optional by design: if the allocation fails the search still works,
     * so a failure is worth one line and not a fatal error. */
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    /* Arm the hang watchdog. Does nothing unless RECOMP_WATCHDOG_SECS is set,
     * and must be called from this thread -- the guest registers it samples are
     * thread-local, so it has to be handed the copies belonging to the thread
     * that runs guest code.
     *
     * Not optional boilerplate: without this call RECOMP_WATCHDOG_SECS is
     * silently inert, and the one diagnostic that tells a hang from slowness
     * does nothing while appearing to be set. */
    xbox_WatchdogStart();
    game_sampler_start();

    /* Step 7: Call the recompiled entry point */
    printf("\nStarting game...\n");
    fflush(stdout);

    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");

    /* Cleanup */
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* ── XBE Loading ───────────────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

/* Saves and the emulated disk images live in xboxdata next to the game, not in Users\<name>\AppData. */
int game_save_root_w(WCHAR *out, DWORD n)
{
    WCHAR exe[MAX_PATH], *slash;
    DWORD len = GetModuleFileNameW(NULL, exe, MAX_PATH);

    if (!len || len >= MAX_PATH - 16 || !(slash = wcsrchr(exe, L'\\')))
        return 0;
    *slash = L'\0';
    return swprintf_s(out, n, L"%s\\xboxdata", exe) > 0;
}

static void copy_save_tree(const WCHAR *src, const WCHAR *dst)
{
    WCHAR pat[MAX_PATH], from[MAX_PATH], to[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    CreateDirectoryW(dst, NULL);
    swprintf_s(pat, MAX_PATH, L"%s\\*", src);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        swprintf_s(from, MAX_PATH, L"%s\\%s", src, fd.cFileName);
        swprintf_s(to, MAX_PATH, L"%s\\%s", dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            copy_save_tree(from, to);
        else
            CopyFileW(from, to, TRUE);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* Older builds kept the saves in %LOCALAPPDATA%\xboxrecomp. Copy them once; the old folder is never touched. */
static void migrate_saves_from_appdata(const WCHAR *root)
{
    static const WCHAR *names[] = { L"UDATA", L"TDATA", L"patched" };
    WCHAR old[MAX_PATH], from[MAX_PATH], to[MAX_PATH];
    DWORD n;
    int i;

    swprintf_s(from, MAX_PATH, L"%s\\UDATA", root);
    if (GetFileAttributesW(from) != INVALID_FILE_ATTRIBUTES)
        return;
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", old, MAX_PATH);
    if (!n || n >= MAX_PATH - 32)
        return;
    wcscat_s(old, MAX_PATH, L"\\xboxrecomp");
    swprintf_s(from, MAX_PATH, L"%s\\UDATA", old);
    if (GetFileAttributesW(from) == INVALID_FILE_ATTRIBUTES)
        return;
    for (i = 0; i < 3; i++) {
        swprintf_s(from, MAX_PATH, L"%s\\%s", old, names[i]);
        swprintf_s(to, MAX_PATH, L"%s\\%s", root, names[i]);
        if (GetFileAttributesW(from) != INVALID_FILE_ATTRIBUTES)
            copy_save_tree(from, to);
    }
    fprintf(stderr, "[SAVES] copied saves from %%LOCALAPPDATA%%\\xboxrecomp (old files kept)\n");
}

/* Console entry point (for debugging -- lets you see printf output) */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
}
