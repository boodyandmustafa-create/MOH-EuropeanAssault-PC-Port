/*
 * game_patches.c -- hand-edited copies of generated functions.
 *
 * Each function here started as the lifter's output (src/recomp/gen) and was
 * changed in as few lines as possible, marked PATCH. recomp_manual.c defines
 * sub_XXXXXXXX as a call to game_patch_XXXXXXXX, which is what keeps the
 * recompiler from generating the original (--exclude-manual).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_funcs.h"
#include <math.h>
void game_pc_note(uint32_t pc);
extern int game_dev_keys(void);                     /* recomp_manual.c: RECOMP_DEV_KEYS=1 */

/* sub_00196467: before loading a level, flush the menu music stream
 * (IDirectSoundStream_FlushEx), spin on GetStatus until it stops, release it
 * and go on with the load. The spin never ended under the emulated APU, so
 * the campaign never loaded. */
#define PATCH_STREAM_STOP_MS 2000u

void game_patch_00196467(void)
{
    DWORD wait_start = 0;
    int still_playing = 0;
    uint32_t ebp = 0;
    ebp = g_ebp;  /* prologue saves caller's frame */
    int _flags = 0; /* fallback flag var */
    uint32_t _fa = 0, _fb = 0;
    int32_t _fas = 0, _fbs = 0;
    (void)_fa; (void)_fb; (void)_fas; (void)_fbs;

loc_00196467: ;
    PUSH32(esp, ebp);
    ebp = esp;
    g_ebp = ebp; /* publish frame for frameless callees */
    g_seh_ebp = ebp;
    PUSH32(esp, ecx);
    PUSH32(esp, 1);
    PUSH32(esp, 0);
    PUSH32(esp, 0);
    PUSH32(esp, MEM32(0x5239B8));
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x0019647Cu); RECOMP_ABI_CALL(0x002ECCBFu, sub_002ECCBF); /* call 0x002ECCBF */

    wait_start = GetTickCount();
loc_0019647C: ;
    eax = ebp + -4;
    PUSH32(esp, eax);
    PUSH32(esp, MEM32(0x5239B8));
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x0019648Bu); RECOMP_ABI_CALL(0x002ECD1Fu, sub_002ECD1F); /* call 0x002ECD1F */

loc_0019648B: ;
    _fa = (uint32_t)(MEM8(ebp + -4)) & 0xFFu; _fb = (uint32_t)(1) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* test MEM8(ebp + -4), 1 (8-bit) */
    if (TEST_NZ(_fa, _fb)) {
        /* PATCH: bounded. The APU model does not always report the stream
         * stopped; on the console this takes a few milliseconds. */
        if (GetTickCount() - wait_start < PATCH_STREAM_STOP_MS) {
            RECOMP_BACK_EDGE(0x0019648Fu);
            Sleep(1);
            goto loc_0019647C;
        }
        still_playing = 1;
        fprintf(stderr, "[PATCH] music stream 0x%08X still playing %u ms after its flush;"
                        " loading on without releasing it\n",
                MEM32(0x5239B8), (unsigned)PATCH_STREAM_STOP_MS);
    }

loc_00196491: ;
    /* PATCH: release the stream only once it has stopped. Freed while its
     * voices are still on DirectSound's service list, the next interrupt
     * walks freed memory. A stream left alive costs a few KB. */
    if (!still_playing) {
        PUSH32(esp, MEM32(0x5239B8));
        g_ebp = ebp; /* frame stays current across calls */
        g_seh_ebp = ebp;
        PUSH32(esp, 0x0019649Cu); RECOMP_ABI_CALL(0x002EBFE1u, sub_002EBFE1); /* call 0x002EBFE1 */
    }

loc_0019649C: ;
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x001964A1u); RECOMP_ABI_CALL(0x001950E3u, sub_001950E3); /* call 0x001950E3 */

loc_001964A1: ;
    PUSH32(esp, MEM32(0x5239D0));
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x001964ACu); RECOMP_ABI_CALL(0x00197A91u, sub_00197A91); /* call 0x00197A91 */

loc_001964AC: ;
    POP32(esp, ecx);
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x001964B2u); RECOMP_ABI_CALL(0x001950F1u, sub_001950F1); /* call 0x001950F1 */

loc_001964B2: ;
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x001964B7u); RECOMP_ABI_CALL(0x001950E3u, sub_001950E3); /* call 0x001950E3 */

loc_001964B7: ;
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x001964BCu); RECOMP_ABI_CALL(0x00199C1Au, sub_00199C1A); /* call 0x00199C1A */

loc_001964BC: ;
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x001964C1u); RECOMP_ABI_CALL(0x001950F1u, sub_001950F1); /* call 0x001950F1 */

loc_001964C1: ;
    esp = ebp;
    POP32(esp, ebp); /* leave */
    esp += 4; return; /* ret */

}


/* ── sub_001D68F0: a function the disassembler never found ──────────
 *
 * Reached only through a function pointer, so static analysis missed it and
 * every call to it was dropped ("[ICALL] Failed to resolve VA 0x001D68F0",
 * hundreds of times a level). It is the twin of sub_001D6930 -- flush the
 * cache if asked, sub_001C17B0, then draw the list at 0x528AC8 when the
 * scene has one -- calling sub_001D4E40 where that one calls sub_001D4E80.
 * Lifted by hand from sub_001D6930's generated body; recomp_lookup_manual
 * returns it. wbinvd is a cache flush with nothing to model here. */
void sub_001D68F0(void)
{
    uint32_t ebp = 0;
    int _flags = 0;
    uint32_t _fa = 0, _fb = 0;
    (void)_fa; (void)_fb; (void)_flags;
    ebp = g_seh_ebp;

    SET_LO8(eax, MEM8(0x529068));
    if (LO8(eax) != 0)
        MEM8(0x529068) = 0;                         /* after wbinvd */

    PUSH32(esp, 0x001D6907u); RECOMP_ABI_CALL(0x001C17B0u, sub_001C17B0);

    eax = MEM32(0x528AC4);
    ecx = MEM32(eax + 8);
    if (ecx != 0) {
        ecx = MEM32(0x528AC8);
        PUSH32(esp, ecx);
        PUSH32(esp, 0);
        PUSH32(esp, 0x001D6921u); RECOMP_ABI_CALL(0x001D4E40u, sub_001D4E40);
        esp = esp + 8;
    }
    g_seh_ebp = ebp; sub_001C17B0(); return;       /* tail jmp 0x001C17B0 */
}

/* The vblank callback (sub_00013130, registered with D3D and reached only by
 * indirect call): counts vblanks in 0x38DC8C for the title's frame limiter.
 *
 * The limiter (sub_000137C0) spins until 0x3727D4 vblanks have passed since
 * the last frame and returns how many did; the game steps its simulation by
 * that count, so its speed does not depend on the frame rate. 0x3727D4 is 2
 * (set by sub_0007FC50): a 30 fps cap. RECOMP_60FPS=1 holds it
 * at 1, written here at every vblank so the start-up write cannot win. */
void game_vblank_callback(void)
{
    static int fps60 = -1;
    if (fps60 < 0) {
        const char *e = getenv("RECOMP_60FPS");
        fps60 = e && *e && *e != '0';
    }
    MEM32(0x38DC8C) = MEM32(0x38DC8C) + 1;
    if (fps60)
        MEM32(0x3727D4) = 1;
    esp += 4; return; /* ret */
}

/* RECOMP_FPS=N (61..1000): the frame limiter below paces the game on a host
 * clock ticking N times a second instead of the 60 Hz vblank. 0 = off. */
static int patch_fps(void)
{
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("RECOMP_FPS");
        n = e ? atoi(e) : 0;
        if (n <= 60 || n > 1000)
            n = 0;
        if (n)
            fprintf(stderr, "[GAME] frame limiter on a %d Hz host clock\n", n);
    }
    return n;
}

/* The host clock in ticks of 1/n s. */
static uint64_t patch_tick(int n)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / f.QuadPart) * (uint64_t)n
         + (uint64_t)(c.QuadPart % f.QuadPart) * (uint64_t)n / (uint64_t)f.QuadPart;
}

/* sub_000137C0: the frame limiter (see game_vblank_callback). With the wait
 * flag 0x38DC70 set it calls sub_00013500, spins until [0x3727D4] vblanks
 * have passed since the last frame (0x38DC6C) and runs two methods of the
 * object at this+0x84; either way it returns the vblanks since the last
 * frame, at least 1. Every caller turns that into the frame's time as
 * step * [0x3727C0] (in 59.94 Hz frames: 1.0 on NTSC) and stores it at
 * 0x373268, so the game's clock runs at real speed whatever the frame rate.
 *
 * PATCH (RECOMP_FPS=N): the wait and the step are counted in ticks of a
 * host clock at N Hz, and the caller's copy of [0x3727C0] is scaled to one
 * tick's worth of frames: up to N frames a second with the same real-time
 * clock. The vblank bookkeeping (0x38DC6C) is kept for the counter's other
 * users. */
/* Frames the limiter has let through, for an outside profiler to count
 * (scratchpad hitch.py finds it by name in the PDB). */
volatile uint32_t g_game_frames;

void game_patch_000137C0(void)
{
    static uint64_t last_tick;
    int fps = patch_fps();
    g_game_frames++;
    {
        extern void game_sampler_note_thread(void);
        game_sampler_note_thread();              /* per frame, on the game loop's thread (main.c) */
    }
    int _flags = 0; /* fallback flag var */
    uint32_t _fa = 0, _fb = 0;
    int32_t _fas = 0, _fbs = 0;
    (void)_fa; (void)_fb; (void)_fas; (void)_fbs; (void)_flags;

    if (fps) {                                                  /* PATCH */
        /* The caller's copy of [0x3727C0], which it multiplies the step by
         * once this returns: [esp+4+off] here, off by return address.
         * [0x3727C0] is not seconds: it is 1.0 (NTSC; 1.1988 PAL), the game
         * keeping its time in 59.94 Hz frames ([0x3727C8] = 0.016683 s, a
         * vblank). A tick of 1/N s is then [0x3727C0] / N / [0x3727C8]
         * frames: 0.4995 at 120. (Given 1/N there, the clock ran N times
         * slow and the screen never faded in from black.) The global is
         * left alone; the copy is checked to hold its value first. */
        uint32_t off = 0xFFFFFFFFu, slot;
        switch (MEM32(esp)) {
        case 0x000804F3u: case 0x00080B7Cu: off = 0; break;
        case 0x0008073Bu: case 0x000809BEu: case 0x000817B2u: off = 0xC; break;
        }
        slot = esp + 4 + off;
        if (off != 0xFFFFFFFFu && MEM32(slot) == MEM32(0x3727C0)
            && MEMF(0x3727C8) > 0.001f)
            MEMF(slot) = MEMF(slot) / (float)fps / MEMF(0x3727C8);
    }

loc_000137C0: ;
    SET_LO8(eax, MEM8(0x38DC70));
    _fa = (uint32_t)(LO8(eax)) & 0xFFu; _fb = (uint32_t)(0) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* cmp LO8(eax), 0 (8-bit) */
    PUSH32(esp, esi);
    esi = ecx;
    if (CMP_EQ(_fa, _fb)) goto loc_0001380D; /* je: equal / zero */

loc_000137CC: ;
    PUSH32(esp, 0x000137D1u); RECOMP_ABI_CALL(0x00013500u, sub_00013500); /* call 0x00013500 */

loc_000137D1: ;
    if (fps) {                         /* PATCH: one tick of the host clock */
        while (patch_tick(fps) - last_tick < 1) {
            RECOMP_BACK_EDGE(0x000137EAu);
            YieldProcessor();
        }
        goto loc_000137EC;
    }
    eax = MEM32(0x38DC6C);
    ecx = MEM32(0x3727D4);

loc_000137E0: ;
    edx = MEM32(0x38DC8C);
    edx = edx - eax;
    _fa = (uint32_t)(edx) & 0xFFFFFFFFu; _fb = (uint32_t)(ecx) & 0xFFFFFFFFu;
    if (CMP_B(_fa, _fb)) { RECOMP_BACK_EDGE(0x000137EAu); goto loc_000137E0; } /* jb: below (unsigned <) */

loc_000137EC: ;
    ecx = MEM32(esi + 0x84);
    eax = MEM32(ecx);
    { uint32_t _icall_esp = g_esp;
    { uint32_t _icall_target = MEM32(eax + 4); PUSH32(esp, 0x000137F7u); RECOMP_ICALL_SAFE_AT(_icall_target, _icall_esp, 0x000137F4u); } /* indirect call */
    }

loc_000137F7: ;
    MEM32(0x38DCA8) = MEM32(0x38DCA8) - 1;
    ecx = MEM32(esi + 0x84);
    edx = MEM32(ecx);
    { uint32_t _icall_esp = g_esp;
    { uint32_t _icall_target = MEM32(edx); PUSH32(esp, 0x00013807u); RECOMP_ICALL_SAFE_AT(_icall_target, _icall_esp, 0x00013805u); } /* indirect call */
    }

loc_00013807: ;
    MEM32(0x38DCA8) = MEM32(0x38DCA8) + 1;

loc_0001380D: ;
    if (fps) {                         /* PATCH: the step in host ticks */
        uint64_t now = patch_tick(fps);
        eax = last_tick && now > last_tick ? (uint32_t)(now - last_tick) : 1u;
        last_tick = now;
        MEM32(0x38DC6C) = MEM32(0x38DC8C);
        POP32(esp, esi);
        esp += 4; return; /* ret */
    }
    eax = MEM32(0x38DC8C);
    edx = MEM32(0x38DC6C);
    ecx = MEM32(0x38DC8C);
    eax = eax - edx;
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(1) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp eax, 1 (32-bit) */
    MEM32(0x38DC6C) = ecx;
    POP32(esp, esi);
    if (CMP_GE(_fas, _fbs)) goto loc_00013831; /* jge: greater or equal (signed >=) */

loc_0001382C: ;
    eax = 1;

loc_00013831: ;
    esp += 4; return; /* ret */
}

/* sub_0017B210: once a frame, with the frame time (in 59.94 Hz frames) as
 * its float argument: turns it into milliseconds as (int)frame_time *
 * [0x51FBB8] (16, 1000 / 59.94 truncated) and counts a timer object down by
 * them, handing the milliseconds on to sub_00170730 as well. Below one frame
 * (RECOMP_FPS above 60: 0.4995 at 120) that was 0 ms every frame, the game's
 * clock stood still and the screen never faded in from black.
 * PATCH: the product is taken first and rounded. A whole frame time gives
 * exactly what it did. */
void game_patch_0017B210(void)
{
    int _flags = 0; /* fallback flag var */
    uint32_t _fa = 0, _fb = 0;
    int32_t _fas = 0, _fbs = 0;
    (void)_fa; (void)_fb; (void)_fas; (void)_fbs;
    #define fp_push(v) do { double _fp_value = (v); \
        g_fp_top = (g_fp_top + 7u) & 7u; \
        g_fp_stack[g_fp_top] = _fp_value; } while (0)
    #define fp_pop() (g_fp_top = (g_fp_top + 1u) & 7u)
    #define fp_top() g_fp_stack[g_fp_top]
    #define fp_st(i) g_fp_stack[(g_fp_top + (i)) & 7u]
    #define fp_st1() fp_st(1)

loc_0017B210: ;
    PUSH32(esp, esi);
    esi = ecx;
    SET_LO8(eax, MEM8(esi + 5));
    _fa = (uint32_t)(LO8(eax)) & 0xFFu; _fb = (uint32_t)(0) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* cmp LO8(eax), 0 (8-bit) */
    if (CMP_EQ(_fa, _fb)) goto loc_0017B273; /* je: equal / zero */

loc_0017B21A: ;
    SET_LO8(eax, MEM8(esi + 6));
    _fa = (uint32_t)(LO8(eax)) & 0xFFu; _fb = (uint32_t)(0) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* cmp LO8(eax), 0 (8-bit) */
    if (CMP_EQ(_fa, _fb)) goto loc_0017B234; /* je: equal / zero */

loc_0017B221: ;
    eax = MEM32(esi + 0x14);
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(0) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp eax, 0 (32-bit) */
    if (CMP_EQ(_fa, _fb)) goto loc_0017B234; /* je: equal / zero */

loc_0017B228: ;
    ecx = esi + 8;
    PUSH32(esp, 0x0017B230u); RECOMP_ABI_CALL(0x0010B220u, sub_0010B220); /* call 0x0010B220 */

loc_0017B230: ;
    _fa = (uint32_t)(LO8(eax)) & 0xFFu; _fb = (uint32_t)(0) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* cmp LO8(eax), 0 (8-bit) */
    if (CMP_EQ(_fa, _fb)) goto loc_0017B273; /* je: equal / zero */

loc_0017B234: ;
    ecx = esi;
    PUSH32(esp, 0x0017B23Bu); RECOMP_ABI_CALL(0x0017AF80u, sub_0017AF80); /* call 0x0017AF80 */

loc_0017B23B: ;
    eax = MEM32(0x51FB7C);
    PUSH32(esp, 0x322E08);
    MEM32(0x51FB74) = eax;
    PUSH32(esp, 0x0017B24Fu); RECOMP_ABI_CALL(0x00151EF0u, sub_00151EF0); /* call 0x00151EF0 */

loc_0017B24F: ;
    _fb = (uint32_t)(4) & 0xFFFFFFFFu; _fbs = (int32_t)(int32_t)(_fb); /* add source, before the write */
    esp = esp + 4;
    _fa = (uint32_t)(esp) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* add result */
    _fa = (uint32_t)(LO8(eax)) & 0xFFu; _fb = (uint32_t)(0) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* cmp LO8(eax), 0 (8-bit) */
    PUSH32(esp, 0x322DF4);
    if (CMP_EQ(_fa, _fb)) goto loc_0017B262; /* je: equal / zero */

loc_0017B25B: ;
    PUSH32(esp, 0x322DFC);
    goto loc_0017B267;

loc_0017B262: ;
    PUSH32(esp, 0x307A28);

loc_0017B267: ;
    PUSH32(esp, 0x0017B26Cu); RECOMP_ABI_CALL(0x00170760u, sub_00170760); /* call 0x00170760 */

loc_0017B26C: ;
    _fb = (uint32_t)(8) & 0xFFFFFFFFu; _fbs = (int32_t)(int32_t)(_fb); /* add source, before the write */
    esp = esp + 8;
    _fa = (uint32_t)(esp) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* add result */
    MEM8(esi + 5) = 0;

loc_0017B273: ;
    SET_LO8(eax, MEM8(esi + 4));
    _fa = (uint32_t)(LO8(eax)) & 0xFFu; _fb = (uint32_t)(0) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* cmp LO8(eax), 0 (8-bit) */
    if (CMP_EQ(_fa, _fb)) goto loc_0017B302; /* je: equal / zero */

loc_0017B27E: ;
    _fa = (uint32_t)(MEM8(0x51FBBC)) & 0xFFu; _fb = (uint32_t)(1) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* test MEM8(0x51FBBC), 1 (8-bit) */
    if (TEST_NZ(_fa, _fb)) goto loc_0017B2AA; /* jne: not equal / not zero */

loc_0017B287: ;
    fp_push(MEMF(0x2FEFB0)); /* fld float */
    eax = MEM32(0x51FBBC);
    fp_top() = RECOMP_FP_PC(fp_top() / MEMF(0x3727C4)); /* fdiv dword ptr [0x3727c4] */
    eax = eax | 1;
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* or result */
    MEM32(0x51FBBC) = eax;
    PUSH32(esp, 0x0017B2A5u); RECOMP_ABI_CALL(0x002BE320u, sub_002BE320); /* call 0x002BE320 */

loc_0017B2A5: ;
    MEM32(0x51FBB8) = eax;

loc_0017B2AA: ;
    fp_push(MEMF(esp + 8)); /* fld float */
    PUSH32(esp, edi);
    PUSH32(esp, 0x0017B2B4u); RECOMP_ABI_CALL(0x002BE320u, sub_002BE320); /* call 0x002BE320 */

loc_0017B2B4: ;
    ecx = MEM32(esp + 0xC);
    edi = eax;
    /* PATCH: (frame time * ms per frame), rounded -- was (int)frame time
     * * ms per frame, 0 below one 60 Hz frame (RECOMP_FPS). */
    edi = (uint32_t)(int32_t)(MEMF(esp + 0xC) * (float)(int32_t)MEM32(0x51FBB8) + 0.5f);
    PUSH32(esp, ecx);
    ecx = esi;
    PUSH32(esp, 0x0017B2C9u); RECOMP_ABI_CALL(0x001789F0u, sub_001789F0); /* call 0x001789F0 */

loc_0017B2C9: ;
    eax = MEM32(esi);
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(0) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp eax, 0 (32-bit) */
    if (CMP_LE(_fas, _fbs)) goto loc_0017B2E3; /* jle: less or equal (signed <=) */

loc_0017B2CF: ;
    _fb = (uint32_t)(edi) & 0xFFFFFFFFu; _fbs = (int32_t)(int32_t)(_fb); /* sub source, before the write */
    eax = eax - edi;
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* sub result */
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(0) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp eax, 0 (32-bit) */
    MEM32(esi) = eax;
    if (CMP_G(_fas, _fbs)) goto loc_0017B301; /* jg: greater (signed >) */

loc_0017B2D7: ;
    POP32(esp, edi);
    MEM8(0x3C250F) = 1;
    POP32(esp, esi);
    esp += 8; return; /* ret 4 */

loc_0017B2E3: ;
    _fa = (uint32_t)(MEM32(0x388876)) & 0xFFFFFFFFu; _fb = (uint32_t)(0x192) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* test MEM32(0x388876), 0x192 (32-bit) */
    if (TEST_Z(_fa, _fb)) goto loc_0017B2F8; /* je: equal / zero */

loc_0017B2EF: ;
    _fa = (uint32_t)(MEM32(0x51FB74)) & 0xFFFFFFFFu; _fb = (uint32_t)(0xC) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp MEM32(0x51FB74), 0xC (32-bit) */
    if (CMP_NE(_fa, _fb)) goto loc_0017B301; /* jne: not equal / not zero */

loc_0017B2F8: ;
    PUSH32(esp, edi);
    PUSH32(esp, 0x0017B2FEu); RECOMP_ABI_CALL(0x00170730u, sub_00170730); /* call 0x00170730 */

loc_0017B2FE: ;
    _fb = (uint32_t)(4) & 0xFFFFFFFFu; _fbs = (int32_t)(int32_t)(_fb); /* add source, before the write */
    esp = esp + 4;
    _fa = (uint32_t)(esp) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* add result */

loc_0017B301: ;
    POP32(esp, edi);

loc_0017B302: ;
    POP32(esp, esi);
    esp += 8; return; /* ret 4 */

    #undef fp_push
    #undef fp_pop
    #undef fp_top
    #undef fp_st
    #undef fp_st1
}

/* sub_001AD630: the sound system's per-frame update (thiscall, the frame
 * time in 59.94 Hz frames as its float argument): fades, then every sound
 * source through sub_001B1460, and every 20th call sub_002B0661. Busy spots
 * of a level have it at 1.5 ms a frame -- 8x its usual cost -- which at 120
 * fps is paid twice per 60 Hz tick for nothing the ear can tell.
 * PATCH: under RECOMP_FPS it runs once per 60 Hz frame with the frame time
 * gathered since, as at 60 fps. Its return value is not used by its one
 * caller (sub_00081190). */
/* EXPERIMENT (corpses): RECOMP_KEEP_CORPSES=<mask> skips game functions that run once per kill a few seconds
 * after the ragdoll is built (call-profile run of 2026-10-29, 8 kills): bit 1 = sub_00017930 (kill event), bit 2 =
 * sub_0002B0A0 (AI tactic object destructor), bit 16 = sub_000F8C80 (soldier vtable +0x1B0 "start fade":
 * sets {mode 2, 0, duration, 0.5} at this+0x2588, called by the kill event), bit 4 = sub_0004F920 + sub_0004F940 (ragdoll object destructors, run
 * ~15 s after the kill when the body vanishes). The marked edits in recomp_0001.c / recomp_0004.c call this. */
/* RECOMP_CORPSE_TRACE=1: every call of the traced functions prints the guest stack's code addresses (a poor
 * man's call chain) with a millisecond clock, so the kill / vanish moments can be lined up with the player's eyes. */
void game_corpse_trace(uint32_t fn)
{
    static int on = -1;
    static unsigned n;
    char line[600];
    int len, i;
    uint32_t sp;
    if (on < 0) {
        const char *e = getenv("RECOMP_CORPSE_TRACE");
        on = e && *e == '1';
    }
    if (!on || n++ > 400)
        return;
    sp = esp;
    len = snprintf(line, sizeof line, "[CORPSE-TRACE] t=%lu fn=%08X this=%08X stack:", (unsigned long)GetTickCount(),
                   (unsigned)fn, (unsigned)g_ecx);
    for (i = 0; i < 40 && len < (int)sizeof line - 12; i++) {
        uint32_t v = MEM32(sp + 4u * (uint32_t)i);
        if (v >= 0x11000u && v < 0x2D8000u)
            len += snprintf(line + len, sizeof line - (size_t)len, " %X", (unsigned)v);
    }
    fprintf(stderr, "%s\n", line);
}

/* The game's delayed-command list (head [0x3E8B34], nodes next/+4 command/+8 countdown, run by sub_0012F0F0 once
 * per tick) holds the command that deletes a killed soldier's body about two seconds after the kill (sub_000388F0,
 * which asks the world to remove the object whose handle is at command+0x18). With bit 8 of RECOMP_KEEP_CORPSES the
 * command, when it comes up for a soldier (vtable 0x317008), is put back on the list with a long countdown instead
 * (sub_0012F220), so the body stays; at most RECOMP_KEEP_CORPSES_N (default 6) bodies are kept: the oldest one's
 * countdown is cut to 1 when a new one needs the room, and it is then deleted by the game's own command. (Keeping
 * every body starved the game's pools -- ragdolls: 10 -- and soldiers froze.) */
#define KEEP_MAX 128
static uint32_t s_kept[KEEP_MAX], s_rel[KEEP_MAX];
static int s_nkept, s_nrel;
static int s_keep_mask = -1;                 /* RECOMP_KEEP_CORPSES (mask) / the Recomp Settings row (0 off, 24 on) */
static int s_keep_n = 6;                     /* how many bodies stay */
static volatile int s_keep_release;

static void keep_init(void)
{
    if (s_keep_mask < 0) {
        const char *e = getenv("RECOMP_KEEP_CORPSES");
        const char *n = getenv("RECOMP_KEEP_CORPSES_N");
        s_keep_mask = e ? atoi(e) : 0;
        if (n && atoi(n) > 0)
            s_keep_n = atoi(n);
        if (s_keep_n > KEEP_MAX - 1)
            s_keep_n = KEEP_MAX - 1;
        if (s_keep_mask)
            fprintf(stderr, "[CORPSE] RECOMP_KEEP_CORPSES=%d, %d bodies\n", s_keep_mask, s_keep_n);
    }
}

/* The settings row: 0 = off (vanilla), n = keep the last n bodies (mask 24: no fade, bodies stay; dead soldiers also
 * use their coarse model, see game_lod_scale). */
int game_keep_get(void)
{
    keep_init();
    return s_keep_mask ? s_keep_n : 0;
}

void game_keep_set(int n)
{
    keep_init();
    if (n <= 0) {
        if (s_keep_mask)
            s_keep_release = 1;
        s_keep_mask = 0;
    } else {
        s_keep_n = n > KEEP_MAX - 1 ? KEEP_MAX - 1 : n;
        s_keep_mask = 24;
    }
    fprintf(stderr, "[CORPSE] keep corpses: %d\n", game_keep_get());
}

static int cmd_find(const uint32_t *a, int n, uint32_t cmd)
{
    int i;
    for (i = 0; i < n; i++)
        if (a[i] == cmd)
            return i;
    return -1;
}

static void list_set_ticks(uint32_t cmd, uint32_t ticks)
{
    uint32_t head = MEM32(0x3E8B34), node;
    int guard = 0;
    for (node = head ? MEM32(head) : 0; node && guard < 4096; node = MEM32(node), guard++)
        if (MEM32(node + 4) == cmd) {
            MEM32(node + 8) = ticks;
            return;
        }
}

static int list_get_ticks(uint32_t cmd, uint32_t *ticks)
{
    uint32_t head = MEM32(0x3E8B34), node;
    int guard = 0;
    for (node = head ? MEM32(head) : 0; node && guard < 4096; node = MEM32(node), guard++)
        if (MEM32(node + 4) == cmd) {
            *ticks = MEM32(node + 8);
            return 1;
        }
    return 0;
}

static void list_schedule(uint32_t cmd, uint32_t ticks)
{
    uint32_t s_eax = g_eax, s_ecx = g_ecx, s_edx = g_edx, s_ebx = g_ebx, s_esi = g_esi, s_edi = g_edi,
             s_ebp = g_ebp, s_esp = esp;
    g_ecx = cmd;
    PUSH32(esp, ticks);
    PUSH32(esp, 0x000388F0u);
    RECOMP_ABI_CALL(0x0012F220u, sub_0012F220);
    g_eax = s_eax; g_ecx = s_ecx; g_edx = s_edx; g_ebx = s_ebx; g_esi = s_esi; g_edi = s_edi;
    g_ebp = s_ebp; esp = s_esp;
}

/* RECOMP_CORPSE_WATCH=1 (diagnostic only): every body that is being kept is snapshotted (0x2600 bytes of the soldier
 * object, which includes the fade state at +0x2588) and compared each draw frame; only pointer-like dwords and the
 * fade-state words are logged ("[CWATCH] obj+off old -> new"), plus a vtable change (object freed or reused), the
 * body leaving the kept list, and every pending delayed command that holds the body's pointer in its first 0x40
 * bytes. F11 prints "[CWATCH] MARK": press it the moment a body shows spikes or turns transparent. */
#define CW_MAX 32
#define CW_WORDS (0x2600 / 4)
static struct {
    uint32_t obj, cmd, t0, ticks, tick_t;
    uint32_t snap[CW_WORDS];
    uint8_t cnt[CW_WORDS];
} s_cw[CW_MAX];
static int s_ncw;

static int cw_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_CORPSE_WATCH");
        on = e && *e == '1';
        if (on)
            fprintf(stderr, "[CWATCH] on, F9 = MARK\n");
    }
    return on;
}

static int cw_ptrlike(uint32_t v)
{
    return (v >= 0x11000u && v < 0x400000u) || (v >= 0x80000000u && v < 0x90000000u);
}

static void corpse_watch_add(uint32_t obj, uint32_t cmd)
{
    int i;
    if (!cw_on() || s_ncw >= CW_MAX)
        return;
    fprintf(stderr, "[CWATCH] ragdoll pool 0x3A6738: size %u used %u peak %u\n", (unsigned)MEM32(0x3A6738 + 8),
            (unsigned)MEM32(0x3A6738 + 0x1C), (unsigned)MEM32(0x3A6738 + 0x20));
    s_cw[s_ncw].obj = obj;
    s_cw[s_ncw].cmd = cmd;
    s_cw[s_ncw].t0 = GetTickCount();
    for (i = 0; i < CW_WORDS; i++)
        s_cw[s_ncw].snap[i] = MEM32(obj + 4u * (uint32_t)i);
    memset(s_cw[s_ncw].cnt, 0, sizeof s_cw[s_ncw].cnt);
    fprintf(stderr, "[CWATCH] t=%lu keep obj=%08X cmd=%08X fade+2588: %08X %08X %08X %08X\n",
            (unsigned long)s_cw[s_ncw].t0, (unsigned)obj, (unsigned)cmd,
            (unsigned)MEM32(obj + 0x2588), (unsigned)MEM32(obj + 0x258C), (unsigned)MEM32(obj + 0x2590),
            (unsigned)MEM32(obj + 0x2594));
    s_ncw++;
}

static void lod_dead_keys(void);
void game_corpse_watch_poll(void)
{
    static int down;
    static unsigned lines, tick;
    static uint32_t seen[64];
    static int nseen;
    int i, k;
    if (s_keep_release) {                    /* "Keep corpses" switched off: every kept body gets its delete command now */
        s_keep_release = 0;
        for (i = 0; i < s_nkept; i++)
            list_set_ticks(s_kept[i], 1);
        s_nkept = 0;
    }
    lod_dead_keys();
    if (!cw_on())
        return;
    tick++;
    k = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (k && !down)
        fprintf(stderr, "[CWATCH] MARK t=%lu\n", (unsigned long)GetTickCount());
    down = k;
    for (i = 0; i < s_ncw; i++) {
        uint32_t obj = s_cw[i].obj, w;
        unsigned long dt = (unsigned long)(GetTickCount() - s_cw[i].t0);
        if (MEM32(obj) != 0x317008u) {
            fprintf(stderr, "[CWATCH] obj=%08X +%lums vtable now %08X: freed or reused\n", (unsigned)obj, dt,
                    (unsigned)MEM32(obj));
            s_cw[i] = s_cw[--s_ncw];
            i--;
            continue;
        }
        if (cmd_find(s_kept, s_nkept, s_cw[i].cmd) < 0) {
            fprintf(stderr, "[CWATCH] obj=%08X +%lums left the kept list (evicted)\n", (unsigned)obj, dt);
            s_cw[i] = s_cw[--s_ncw];
            i--;
            continue;
        }
        if ((tick & 3) == 0) {
            uint32_t tk;
            if (list_get_ticks(s_cw[i].cmd, &tk)) {
                if (s_cw[i].ticks && (tk > s_cw[i].ticks || s_cw[i].ticks - tk > 400))
                    fprintf(stderr, "[CWATCH] obj=%08X +%lums countdown %u -> %u (jump)\n", (unsigned)obj, dt,
                            (unsigned)s_cw[i].ticks, (unsigned)tk);
                s_cw[i].ticks = tk;
                s_cw[i].tick_t = GetTickCount();
            } else if (s_cw[i].ticks) {
                fprintf(stderr, "[CWATCH] obj=%08X +%lums no pending node any more (last countdown %u)\n", (unsigned)obj,
                        dt, (unsigned)s_cw[i].ticks);
                s_cw[i].ticks = 0;
            }
        }
        for (w = 0; w < CW_WORDS; w++) {
            uint32_t nv = MEM32(obj + 4u * w), ov = s_cw[i].snap[w];
            int fade = (w >= 0x2580u / 4 && w < 0x25A0u / 4);
            if (nv == ov)
                continue;
            s_cw[i].snap[w] = nv;
            if ((fade || cw_ptrlike(nv) || cw_ptrlike(ov)) && s_cw[i].cnt[w] < ((w == 0x253Cu / 4 || w == 0x2550u / 4) ? 250 : 6) && lines < 6000) {
                s_cw[i].cnt[w]++;
                lines++;
                fprintf(stderr, "[CWATCH] obj=%08X +%lums off %04X: %08X -> %08X%s\n", (unsigned)obj, dt,
                        (unsigned)(w * 4), (unsigned)ov, (unsigned)nv, fade ? " (fade state)" : "");
            }
        }
    }
    if (s_ncw && (tick % 15) == 0) {                          /* pending delayed commands that name a kept body */
        uint32_t head = MEM32(0x3E8B34), node;
        int guard = 0;
        for (node = head ? MEM32(head) : 0; node && guard < 4096; node = MEM32(node), guard++) {
            uint32_t cmd = MEM32(node + 4), off;
            if (cmd < 0x10000u || cmd >= 0x90000000u)
                continue;
            for (off = 0; off < 0x40; off += 4)
                for (i = 0; i < s_ncw; i++)
                    if (MEM32(cmd + off) == s_cw[i].obj) {
                        uint32_t key = cmd ^ (off << 28);
                        int j;
                        for (j = 0; j < nseen; j++)
                            if (seen[j] == key)
                                break;
                        if (j == nseen && nseen < 64) {
                            uint32_t vt = MEM32(cmd);
                            seen[nseen++] = key;
                            fprintf(stderr, "[CWATCH] pending cmd=%08X vtable=%08X fn=%08X ticks=%d holds obj=%08X at +%X\n",
                                    (unsigned)cmd, (unsigned)vt, (unsigned)MEM32(vt + 8), (int)MEM32(node + 8),
                                    (unsigned)s_cw[i].obj, (unsigned)off);
                        }
                    }
        }
    }
}

/* RECOMP_RAGDOLL_POOL=<k> (default 1): the game's "Pool (Ragdoll)" (sub_002CF870) is a 0x168-byte arena (nine 0x28-byte
 * blocks); this makes it k times bigger, so keeping many bodies cannot exhaust it. */
uint32_t game_ragdoll_pool_bytes(void)
{
    static uint32_t bytes;
    if (!bytes) {
        const char *e = getenv("RECOMP_RAGDOLL_POOL");
        int k = e ? atoi(e) : (keep_init(), s_keep_mask ? 16 : 1);   /* only while corpses are kept (vanilla otherwise: the game ran fine with its own sizes) */
        if (k < 1)
            k = 1;
        if (k > 64)
            k = 64;
        bytes = 0x168u * (uint32_t)k;
        if (k != 1)
            fprintf(stderr, "[CORPSE] ragdoll pool %u bytes (x%d)\n", (unsigned)bytes, k);
    }
    return bytes;
}

/* RECOMP_PLAYBACK_POOL=<k> (default 4): size multiplier of the game's "Pool (PlaybackTask)" (sub_002CF850, 0xABE0 bytes,
 * 224-byte blocks). It runs full even in normal play; kept bodies hold their tasks and starved the living soldiers. */
uint32_t game_playback_pool_bytes(void)
{
    static uint32_t bytes;
    if (!bytes) {
        const char *e = getenv("RECOMP_PLAYBACK_POOL");
        int k = e ? atoi(e) : 1;
        if (k < 1)
            k = 1;
        if (k > 64)
            k = 64;
        bytes = 0xABE0u * (uint32_t)k;
        if (k != 1) fprintf(stderr, "[CORPSE] playback pool %u bytes (x%d)\n", (unsigned)bytes, k);
    }
    return bytes;
}

/* Pool constructor hook (sub_00065970): "Pool (Animation)" (0x3A7878, 40320 bytes, 676-byte animation blocks) is full
 * even in normal play and kept bodies starved the living soldiers of animations (frozen, no blending, spikes).
 * RECOMP_ANIM_POOL=<k> (default 6) multiplies it. */
uint32_t game_pool_size(uint32_t pool, uint32_t size)
{
    if (pool == 0x3A7878u) {
        const char *e = getenv("RECOMP_ANIM_POOL");
        int k = e ? atoi(e) : (keep_init(), s_keep_mask ? 8 : 1);   /* the animation pool was FULL (39884 of 40320) with 25 kept bodies */
        if (k < 1)
            k = 1;
        if (k > 64)
            k = 64;
        if (k != 1) fprintf(stderr, "[CORPSE] animation pool %u -> %u bytes (x%d)\n", (unsigned)size, (unsigned)(size * (uint32_t)k), k);
        return size * (uint32_t)k;
    }
    return size;
}

/* called by the game's pool allocator (sub_00065B20) when no free block is big enough: it then prints a message and
 * falls back to the general allocator. Logged for every pool (first 200 times). */
void game_pool_full(uint32_t pool, uint32_t req)
{
    static unsigned n;
    if (n++ < 200) {
        char name[48];
        uint32_t p = MEM32(pool + 4);
        int i = 0;
        if (p >= 0x10000u && p < 0x400000u)
            for (; i < 40; i++) {
                name[i] = (char)MEM8(p + (uint32_t)i);
                if (!name[i])
                    break;
            }
        name[i < 40 ? i : 39] = 0;
        fprintf(stderr, "[POOLFULL] pool %08X \"%s\" request %u (arena %u used %u peak %u)\n", (unsigned)pool, name,
                (unsigned)req, (unsigned)MEM32(pool + 8), (unsigned)MEM32(pool + 0x1C), (unsigned)MEM32(pool + 0x20));
    }
}

int game_keep_corpse_obj(uint32_t cmd)
{
    uint32_t h, vt = 0;
    extern int game_keep_corpses(int bit);
    h = MEM32(cmd + 0x18);
    if (h >= 0x10000u && h < 0x90000000u)
        vt = MEM32(h);
    {
        static int on = -1;
        static unsigned n;
        if (on < 0) {
            const char *e = getenv("RECOMP_CORPSE_TRACE");
            on = e && *e == '1';
        }
        if (on && n++ < 400)
            fprintf(stderr, "[CORPSE-TRACE] t=%lu delete-command cmd=%08X obj=%08X vtable=%08X\n",
                    (unsigned long)GetTickCount(), (unsigned)cmd, (unsigned)h, (unsigned)vt);
    }
    if (vt != 0x317008u || !game_keep_corpses(8))
        return 0;
    {
        int keep_n = s_keep_n;
        int i;
        if ((i = cmd_find(s_rel, s_nrel, cmd)) >= 0) {          /* cut short to make room: let the game delete it */
            if (cw_on())
                fprintf(stderr, "[CWATCH] release cmd=%08X obj=%08X: cap eviction (kept %d)\n", (unsigned)cmd, (unsigned)h,
                        s_nkept);
            s_rel[i] = s_rel[--s_nrel];
            return 0;
        }
        if ((i = cmd_find(s_kept, s_nkept, cmd)) >= 0) {        /* its long countdown ran out: delete it */
            if (cw_on())
                fprintf(stderr, "[CWATCH] release cmd=%08X obj=%08X: countdown ended or game ran it early (kept %d)\n",
                        (unsigned)cmd, (unsigned)h, s_nkept);
            memmove(s_kept + i, s_kept + i + 1, (size_t)(s_nkept - i - 1) * sizeof s_kept[0]);
            s_nkept--;
            return 0;
        }
        while (s_nkept >= keep_n) {                              /* make room: the oldest body goes */
            uint32_t old = s_kept[0];
            memmove(s_kept, s_kept + 1, (size_t)(s_nkept - 1) * sizeof s_kept[0]);
            s_nkept--;
            if (s_nrel < KEEP_MAX)
                s_rel[s_nrel++] = old;
            list_set_ticks(old, 1);
        }
        s_kept[s_nkept++] = cmd;
        if (cw_on())
            fprintf(stderr, "[CWATCH] keep cmd=%08X obj=%08X (kept %d of %d) anim pool %u/%u peak %u\n", (unsigned)cmd,
                    (unsigned)h, s_nkept, keep_n, (unsigned)MEM32(0x3A7878 + 0x1C), (unsigned)MEM32(0x3A7878 + 8),
                    (unsigned)MEM32(0x3A7878 + 0x20));
        corpse_watch_add(h, cmd);
        list_schedule(cmd, 600000000);                          /* (6000 was only ~9 s in the game's tick unit: bodies vanished then);
                                                                 * now effectively never, the cap ends it */
        return 1;
    }
}

int game_keep_corpses(int bit)
{
    keep_init();
    return (s_keep_mask & bit) != 0;
}

/* ---- diagnostic call profile (cmake -DRECOMP_CALL_PROFILE=ON, separate build dir) ----
 * Every direct call of the recompiled game bumps a counter; F10 (while the game window is in front) writes the
 * non-zero counts to profile_<n>.txt ("<va hex> <count>"), the file number rising per press. Two snapshots around
 * an event (a corpse vanishing, a bullet hitting flesh instead of a wall) diff to the functions that event ran:
 * tools_dbg/profdiff.py. Without the define the poll is empty. */
#ifdef RECOMP_CALL_PROFILE
static uint32_t s_prof[0x300000], s_pfirst[0x300000], s_plast[0x300000];
static volatile uint32_t s_pframe = 1;      /* draw frames so far (game_prof_poll) */
void recomp_call_profile_hit(uint32_t va)
{
    if (va < sizeof s_prof / sizeof s_prof[0]) {
        if (!s_prof[va]++)
            s_pfirst[va] = s_pframe;
        s_plast[va] = s_pframe;
    }
}
void game_prof_poll(void)
{
    static int down;
    static unsigned n;
    int k = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    s_pframe++;
    if (k && !down) {
        char name[64];
        FILE *fp;
        uint32_t i;
        snprintf(name, sizeof name, "profile_%u.txt", n);
        fp = fopen(name, "w");
        if (fp) {
            fprintf(fp, "# frame %u\n", s_pframe);
            for (i = 0; i < sizeof s_prof / sizeof s_prof[0]; i++)
                if (s_prof[i])
                    fprintf(fp, "%X %u %u %u\n", i, s_prof[i], s_pfirst[i], s_plast[i]);
            fclose(fp);
        }
        fprintf(stderr, "[PROFILE] snapshot %u written (%s) at frame %u\n", n, name, s_pframe);
        n++;
    }
    down = k;
}
#else
void game_prof_poll(void) {}
#endif

/* TEST AID: RECOMP_EMITTER_STRESS=<n> makes the game itself create the problem. About four times a second it asks
 * the game's own emitter registration (sub_001B0B00, the same call the game uses) for n copies of a live emitter,
 * then clears the copies' sound object / playing / finished fields so they are idle and backend-less, exactly the
 * kind the pool fills with when the bug shows. The pool therefore grows by about 4*n a second until emitter_reap
 * (RECOMP_EMITTER_REAP_AT, default 500) starts freeing it. Registering a copy stops the emitter it was copied
 * from, so sounds are audibly disturbed while this is on. Every 4 s: [SOUNDFIX-STRESS] pool size and peak. */
static void emitter_stress(uint32_t obj, uint32_t n_now)
{
    static int per = -1, real, repro_kind;
    static unsigned calls, peak, target = 230, delay_s = 60;
    static uint64_t t_first;
    static uint32_t old[2048];
    uint32_t head = obj + 0x200, node, n = 0, src = 0, idle = 0, k, j;

    if (per < 0) {
        /* RECOMP_SOUND_REPRO=<n> (play_soundrepro.bat): the same, but the copies KEEP their backend object (their own sound
         * object, as the real burst when a level part streams in), stay un-finished, and it starts RECOMP_SOUND_REPRO_DELAY
         * seconds (default 60) after the first call and stops adding once the pool holds RECOMP_SOUND_REPRO_TARGET (230). */
        const char *e = getenv("RECOMP_EMITTER_STRESS"), *r = getenv("RECOMP_SOUND_REPRO"),
                   *d = getenv("RECOMP_SOUND_REPRO_DELAY"), *t = getenv("RECOMP_SOUND_REPRO_TARGET");
        per = e ? atoi(e) : 0;
        if (r && atoi(r) > 0) {
            per = atoi(r);
            real = 1;
        }
        if (getenv("RECOMP_SOUND_REPRO_KIND") && *getenv("RECOMP_SOUND_REPRO_KIND") == '3')
            repro_kind = 1;                         /* clone the stand-in (class 0x302BAC) emitters, the kind the real leak is made of */
        if (d)
            delay_s = (unsigned)atoi(d);
        if (t && atoi(t) > 20)
            target = (unsigned)atoi(t);
    }
    if (per <= 0)
        return;
    if (n_now > peak)
        peak = n_now;
    if (!(++calls & 15u))
        fprintf(stderr, "[SOUNDFIX-STRESS] %spool %u, peak %u\n", real ? "REPRO " : "", n_now, peak);
    if (real) {
        uint64_t now = GetTickCount64();
        if (!t_first && n_now >= 3)                 /* the clock starts when the first sounds of a level exist */
            t_first = now;
        if (!t_first || now - t_first < (uint64_t)delay_s * 1000u || n_now >= target)
            return;
    }
    for (node = MEM32(obj + 0x204); node && node != head && n < 2048; node = MEM32(node + 4)) {
        uint32_t em = MEM32(node + 8);
        old[n++] = em;
        if (real) {                                 /* a live emitter that has a backend object (not a position marker) */
            uint32_t bk = em ? MEM32(em + 0x48) : 0;
            if (!src && em && !MEM8(em + 0x44) && bk && ((MEM32(bk) == 0x302BACu) == (repro_kind == 1)))
                src = em;
            continue;
        }
        if (!idle && em && !MEM8(em + 0x44) && !MEM8(em + 0xC0) && !MEM32(em + 0x48))
            idle = em;
        if (!src && em && !MEM8(em + 0x44))
            src = em;
    }
    if (idle)                                       /* copy an idle one: stopping it harms no real sound */
        src = idle;
    if (!src || n + (uint32_t)per + 12 >= 1000)
        return;
    {
        uint32_t s_eax = g_eax, s_ecx = g_ecx, s_edx = g_edx, s_ebx = g_ebx, s_esi = g_esi, s_edi = g_edi,
                 s_ebp = g_ebp, s_esp = esp;
        for (k = 0; k < (uint32_t)per; k++) {
            g_ecx = obj;
            PUSH32(esp, src);
            PUSH32(esp, 0x001AD630u);
            RECOMP_ABI_CALL(0x001B0B00u, sub_001B0B00);
        }
        g_eax = s_eax; g_ecx = s_ecx; g_edx = s_edx; g_ebx = s_ebx; g_esi = s_esi; g_edi = s_edi;
        g_ebp = s_ebp; esp = s_esp;
    }
    for (node = MEM32(obj + 0x204); node && node != head; node = MEM32(node + 4)) {
        uint32_t em = MEM32(node + 8), seen = 0;
        for (j = 0; j < n && !seen; j++)
            seen = old[j] == em;
        if (!seen && em) {
            if (real) {                             /* keep the backend object: only undo the stop the registration did */
                MEM8(em + 0x44) = 0;
                continue;
            }
            MEM32(em + 0x48) = 0;
            MEM8(em + 0xC0) = 0;
            MEM8(em + 0x44) = 0;
        }
    }
}

/* The sound emitter pool (was 150 + the manager's own, now 1000; [0x524300]; list at +0x200, node +4 next, +8 emitter) fills
 * up some minutes into a level and then stays full: the game goes on creating emitters (every shot, every line
 * of dialogue) that never get a sound object (+0x48 == 0) and never finish, so nothing new can play and the
 * weapon and the dialogue go silent. The game's own cleanup (sub_001B1220, run by this update) destroys an
 * emitter whose byte +0x44 is 1. REAP: when the list is nearly full, the oldest emitters that are not playing
 * (+0xC0 == 0) and have no sound object are marked finished, so there is always room. Found and tried live
 * with tools_dbg/emit_free.py (2026-10-29). RECOMP_EMITTER_REAP=0 switches it off; RECOMP_EMITTER_REAP_AT=<n>
 * (default 500 of the 1000: the game's own 150 pool is raised to 1000 in the generated sub_001B0AD0 / sub_001B0B00 / sub_001AD4D0, see EMITTER-CAP) is the list length that starts it, and it frees down to 3/4 of that. */
static int hist_force;

/* [EMHIST]: what the emitters of the pool look like, grouped by the fields that decide their fate. The game's per-emitter
 * update (sub_001ADA50) marks an emitter finished (+0x44 = 1) only when the backend object says "not playing" (vtable +0x10) AND
 * "nothing pending" (vtable +0x14) AND the byte at +8 is non-zero. Printed on [MARK] and, up to 6 times, when 100 or more
 * emitters hold a sound object. One line per group (count, +8, +0x44, +0x78, voice present, +0xC0, class of the emitter
 * and of its backend object, backend byte +4), then the raw words of one sample emitter and its backend object for the
 * two biggest groups. */
static void emitter_hist(uint32_t obj)
{
    enum { MAXG = 24 };
    struct G { uint32_t key[9]; uint32_t cnt, sample; } g[MAXG];
    uint32_t ng = 0, node, head = obj + 0x200, i, j, tot = 0;
    int guard = 0;
    extern int game_emupd_last(uint32_t em);

    memset(g, 0, sizeof g);
    for (node = MEM32(obj + 0x204); node && node != head && guard++ < 2000; node = MEM32(node + 4)) {
        uint32_t em = MEM32(node + 8), bk, k[9];
        if (!em)
            continue;
        bk = MEM32(em + 0x48);
        k[0] = MEM8(em + 8);
        k[1] = MEM8(em + 0x44);
        k[2] = MEM8(em + 0x78);
        k[3] = MEM32(em + 0xBC) != 0xFFFFFFFFu;
        k[4] = MEM8(em + 0xC0);
        k[5] = MEM32(em);
        k[6] = bk ? MEM32(bk) : 0;
        k[7] = bk ? MEM8(bk + 4) : 0xFFu;
        k[8] = (uint32_t)game_emupd_last(em);       /* last verdict of the game's update: 1 busy, 2 pending, 3 kept, 4 finished, 5 no backend, -1 never */
        tot++;
        for (i = 0; i < ng && memcmp(g[i].key, k, sizeof k); i++)
            ;
        if (i == ng) {
            if (ng == MAXG)
                continue;
            memcpy(g[ng].key, k, sizeof k);
            g[ng].sample = em;
            ng++;
        }
        g[i].cnt++;
    }
    for (i = 0; i < ng; i++)                        /* biggest first */
        for (j = i + 1; j < ng; j++)
            if (g[j].cnt > g[i].cnt) {
                struct G t = g[i];
                g[i] = g[j];
                g[j] = t;
            }
    fprintf(stderr, "[EMHIST] t=%u %u emitters in %u groups\n", (unsigned)(GetTickCount64() / 1000), tot, ng);
    for (i = 0; i < ng && i < 12; i++)
        fprintf(stderr, "[EMHIST]  x%u: verdict=%d(1 busy,2 pending,3 kept,4 finished,5 no-backend) +8=%u fin=%u f78=%02X voice=%u c0=%u emitter-class=%08X backend-class=%08X backend+4=%02X\n",
                g[i].cnt, (int)g[i].key[8], g[i].key[0], g[i].key[1], g[i].key[2], g[i].key[3], g[i].key[4], g[i].key[5], g[i].key[6], g[i].key[7]);
    for (i = 0; i < ng && i < 2; i++) {
        uint32_t em = g[i].sample, bk = MEM32(em + 0x48), w;
        fprintf(stderr, "[EMHIST]  sample of group %u, emitter %08X:", i, em);
        for (w = 0; w < 0xD0; w += 4)
            fprintf(stderr, "%s%08X", (w & 0x1F) ? " " : "\n[EMHIST]   ", MEM32(em + w));
        if (bk) {
            fprintf(stderr, "\n[EMHIST]  backend object %08X:", bk);
            for (w = 0; w < 0x60; w += 4)
                fprintf(stderr, "%s%08X", (w & 0x1F) ? " " : "\n[EMHIST]   ", MEM32(bk + w));
        }
        fprintf(stderr, "\n");
    }
}

static void emitter_reap(void)
{
    static int on = -1, at = 500, aggr;
    static unsigned calls, ticks;
    static int mdown;
    static unsigned nmark;
    static int rescue_on = 1, rescue_at = 125, rescue_to = 95, rescue_hard = 140;
    static unsigned rescues;
    uint32_t obj, head, node, n = 0, freed = 0;

    if (on < 0) {
        const char *e = getenv("RECOMP_EMITTER_REAP"), *a = getenv("RECOMP_EMITTER_REAP_AT"),
                   *g = getenv("RECOMP_EMITTER_REAP_AGGR"), *r = getenv("RECOMP_SOUND_RESCUE"),
                   *ra = getenv("RECOMP_SOUND_RESCUE_AT"), *rt = getenv("RECOMP_SOUND_RESCUE_TO");
        on = !(e && *e == '0');
        if (a && atoi(a) > 20)
            at = atoi(a);
        aggr = g && *g == '1';
        rescue_on = !(r && *r == '0');
        if (ra && atoi(ra) > 30)
            rescue_at = atoi(ra);
        if (rt && atoi(rt) > 10)
            rescue_to = atoi(rt);
    }
    {
        /* [MARK]: Pause/Break or End pressed when the sound is heard to break. It prints a line with the time and makes the
         * next pass print the emitter log (SOUNDLOG), so the log shows the state at that very moment. */
        int k = game_dev_keys() && ((GetAsyncKeyState(VK_PAUSE) | GetAsyncKeyState(VK_END)) & 0x8000) != 0;
        if (k && !mdown) {
            fprintf(stderr, "[MARK] t=%u #%u: sound break marked by the player\n", (unsigned)(GetTickCount64() / 1000), ++nmark);
            calls = 15u;
            ticks = 19u;
            hist_force = 1;
        }
        mdown = k;
    }
    if (!on || (++calls & 15u))                     /* about four times a second */
        return;
    obj = MEM32(0x524300);
    if (!obj)
        return;
    head = obj + 0x200;
    for (node = MEM32(obj + 0x204); node && node != head && n < 2000; node = MEM32(node + 4))
        n++;
    emitter_stress(obj, n);
    {
        /* [SOUNDLOG] every ~5 s: the emitter pool next to the kept bodies */
        if ((++ticks % 20u) == 0) {
            uint32_t nb = 0, np = 0, nf = 0;
            int guard = 0;
            extern int game_lod_dead_count(void);
            for (node = MEM32(obj + 0x204); node && node != head && guard++ < 2000; node = MEM32(node + 4)) {
                uint32_t em = MEM32(node + 8);
                if (!em)
                    continue;
                if (MEM32(em + 0x48))
                    nb++;
                if (MEM8(em + 0xC0))
                    np++;
                if (MEM8(em + 0x44))
                    nf++;
            }
            fprintf(stderr, "[SOUNDLOG] t=%u emitters %u (with sound object %u, playing %u, finished-flag %u); kept bodies %d, dead set %d\n",
                    (unsigned)(GetTickCount64() / 1000), n, nb, np, nf, s_nkept, game_lod_dead_count());
            {
                static unsigned auto_hist;
                if (hist_force || (nb >= 100 && auto_hist++ < 6u)) {
                    emitter_hist(obj);
                    hist_force = 0;
                }
            }
        }
    }
    if (rescue_on && (int)n >= rescue_at) {
        /* SOUND RESCUE (2026-10-30, blind safety net): in the sessions where the sound broke, the emitter pool jumped from ~15 to
         * 96-193 within seconds (a streamed level part creating ~80 at once) and never drained, while healthy play peaks at
         * ~66. When it reaches RECOMP_SOUND_RESCUE_AT (125) the oldest emitters are stopped the game's own way (+0x44 = 1, the
         * byte its own stop call sets; its cleanup then destroys them and frees their sound objects) until RECOMP_SOUND_RESCUE_TO
         * (95) are left. The newest 40 and the class 0x302BAC position markers are left alone. RECOMP_SOUND_RESCUE=0 switches it
         * off (the repro launcher does). [RESCUE] line each time. */
        /* Pass 1 takes only emitters that are doing nothing audible: no voice under them (+0xBC == -1) and not playing
         * (+0xC0 == 0). Pass 2 (an emitter that is playing or has a voice) only when the pool is at RECOMP_SOUND_RESCUE_HARD
         * (140), i.e. right under the ~150 sound-object ceiling, so ambient loops and other live sounds are not cut needlessly. */
        int want = (int)n - rescue_to, pass;
        uint32_t rf = 0, rf_idle = 0, rf_far = 0;
        {   /* Distance pass (2026-10-30): the leaked stand-ins (class 0x302BAC, fixed position at +8..+0x10, no voice) of the level
             * parts the player already left are the farthest from the camera ([0x3E8D08] + 0x90), the ones still around him the
             * nearest, so stop the FARTHEST first (only those beyond RECOMP_SOUND_RESCUE_FAR, default 40 units) and leave the
             * ambient/dialogue stand-ins near the player alone. The oldest-first passes below take whatever is still needed. */
            static float far_min = -1.0f;
            uint32_t cam = MEM32(0x3E8D08);
            if (far_min < 0.0f) {
                const char *e = getenv("RECOMP_SOUND_RESCUE_FAR");
                far_min = (e && atof(e) > 0.0) ? (float)atof(e) : 40.0f;
            }
            if (((cam >= 0x10000u && cam < 0x00800000u) || (cam >= 0x80010000u && cam < 0x83FF0000u))) {
                static uint32_t cand[1024];
                static float cdist[1024];
                static int dlog = 0;
                uint32_t nc = 0, i, j, idx = 0;
                float cx = MEMF(cam + 0x90), cy = MEMF(cam + 0x94), cz = MEMF(cam + 0x98);
                for (node = MEM32(obj + 0x204); node && node != head && nc < 1024; node = MEM32(node + 4)) {
                    uint32_t em = MEM32(node + 8), bk;
                    if (idx++ + 40u >= n)
                        break;
                    if (!em || MEM8(em + 0x44) || MEM32(em + 0xBC) != 0xFFFFFFFFu)
                        continue;
                    bk = MEM32(em + 0x48);
                    if (bk && MEM32(bk) == 0x302BACu) {
                        float dx = MEMF(bk + 8) - cx, dy = MEMF(bk + 0xC) - cy, dz = MEMF(bk + 0x10) - cz;
                        float d = sqrtf(dx * dx + dy * dy + dz * dz);
                        if (d == d && d > far_min) {          /* d == d: not NaN */
                            cand[nc] = em;
                            cdist[nc++] = d;
                        }
                    }
                }
                for (i = 1; i < nc; i++) {                     /* insertion sort, farthest first */
                    uint32_t ce = cand[i]; float cd = cdist[i];
                    for (j = i; j > 0 && cdist[j - 1] < cd; j--) {
                        cand[j] = cand[j - 1];
                        cdist[j] = cdist[j - 1];
                    }
                    cand[j] = ce;
                    cdist[j] = cd;
                }
                for (i = 0; i < nc && rf < (uint32_t)want; i++) {
                    if (dlog < 20) {
                        dlog++;
                        fprintf(stderr, "[RESCUE-DIST] stand-in %08X is %.1f units from the camera (far limit %.0f)\n", cand[i],
                                (double)cdist[i], (double)far_min);
                    }
                    MEM8(cand[i] + 0x44) = 1;
                    rf++;
                    rf_idle++;
                    rf_far++;
                }
            }
        }
        for (pass = 0; pass < 2 && rf < (uint32_t)want; pass++) {
            uint32_t idx = 0;
            if (pass == 1 && (int)n < rescue_hard)
                break;
            for (node = MEM32(obj + 0x204); node && node != head && rf < (uint32_t)want; node = MEM32(node + 4)) {
                uint32_t em = MEM32(node + 8);
                if (idx++ + 40u >= n)
                    break;
                if (!em || MEM8(em + 0x44))
                    continue;
                /* 2026-10-30 (live [EMHIST]): the emitters that fill the pool are class 0x302BAC ones (a stand-in sound object that
                 * never makes a sound: vtable +0x10 -> 0, +0x14 -> 1, so the game's update never finishes them), 112-140 of them,
                 * no voice, not playing. They must NOT be left out here: they ARE the leak. */
                if (pass == 0) {
                    uint32_t bk = MEM32(em + 0x48);
                    int standin = bk && MEM32(bk) == 0x302BACu;   /* a started stand-in (byte +4 = 1, so +0xC0 = 1) still makes no sound */
                    if (MEM32(em + 0xBC) != 0xFFFFFFFFu || (MEM8(em + 0xC0) && !standin))
                        continue;
                }
                {   /* ROOT-CAUSE EVIDENCE (2026-10-30): state of the EA stream slot under a stuck emitter before it is stopped, so the
                     * log alone says why its handle never expires (slot in-use byte, length/pos words, DS buffer play flags). */
                    static int slotlog = 0;
                    if (slotlog < 30 && MEM32(0x523BF4) && MEM32(em + 0x8C) != 0xFFFFFFF4u) {
                        uint32_t h = MEM32(em + 0x8C), tbl = MEM32(0x523BF4), idx = h & 0xFFu, sl = tbl + idx * 0x90u;
                        uint32_t pool = MEM32(0x5239B0), dsb = pool ? MEM32(pool + idx * 0x24u + 0x18u) : 0;
                        slotlog++;
                        fprintf(stderr, "[RESCUE-SLOT] em=%08X bk=%08X h=%08X slot=%u id=%08X inuse=%02X +14=%u +18=%d +20=%u +22=%02X +25=%02X +6E=%u +8E=%u | dsb=%08X flags12=%04X c0=%u v=%d\n",
                                em, MEM32(em + 0x48), h, idx, MEM32(sl), MEM8(sl + 0x6D), MEM32(sl + 0x14), (int)MEM32(sl + 0x18),
                                MEM16(sl + 0x20), MEM8(sl + 0x22), MEM8(sl + 0x25), MEM16(sl + 0x6E), MEM16(sl + 0x8E),
                                dsb, dsb ? MEM16(dsb + 0x12) : 0, MEM8(em + 0xC0), (int)MEM32(em + 0xBC));
                    }
                }
                MEM8(em + 0x44) = 1;
                rf++;
                if (pass == 0)
                    rf_idle++;
            }
        }
        if (rf) {
            rescues++;
            fprintf(stderr, "[RESCUE] t=%u #%u: emitter pool had %u, stopped %u old emitters (%u idle, %u live, %u of them far from the player)\n",
                    (unsigned)(GetTickCount64() / 1000), rescues, n, rf, rf_idle, rf - rf_idle, rf_far);
        }
    }
    if ((int)n < at)
        return;
    {
        /* 2026-10-30: the game's sound goes bad at ~150-190 emitters (garbled, every new sound cut after a millisecond,
         * APU lists empty) although the pool cap is 1000, so the reaper starts at 110 and also takes emitters WITH a
         * sound object, as long as they are idle (+0xC0 == 0, no voice: +0xBC == -1) and old: the newest 30 are left
         * alone (a sound that is just starting has no voice yet), and the class 0x302BAC positional markers are kept. */
        int want = (int)n - at * 3 / 4;
        uint32_t idx = 0;
        for (node = MEM32(obj + 0x204); node && node != head && freed < (uint32_t)want; node = MEM32(node + 4)) {
            uint32_t em = MEM32(node + 8), bk;
            if (idx++ + 30u >= n)
                break;
            if (!em || MEM8(em + 0x44) || MEM8(em + 0xC0) || MEM32(em + 0xBC) != 0xFFFFFFFFu)
                continue;
            bk = MEM32(em + 0x48);
            if (bk && !aggr)                        /* the 10-29 safe rule: only emitters with no sound object at all */
                continue;
            if (bk && MEM32(bk) == 0x302BACu)
                continue;
            MEM8(em + 0x44) = 1;
            freed++;
        }
    }
    if (freed) {
        static unsigned logged;
        if (logged++ < 40 || (logged & 255u) == 0)
            fprintf(stderr, "[SOUNDFIX] emitter pool had %u, marked %u idle emitters finished\n", n, freed);
    }
}

void game_patch_001AD630(void)
{
    int _flags = 0; /* fallback flag var */
    uint32_t _fa = 0, _fb = 0;
    int32_t _fas = 0, _fbs = 0;
    (void)_fa; (void)_fb; (void)_fas; (void)_fbs;
    double _fca = 0.0, _fcb = 0.0;
    (void)_fca; (void)_fcb;
    #define fp_push(v) do { double _fp_value = (v); \
        g_fp_top = (g_fp_top + 7u) & 7u; \
        g_fp_stack[g_fp_top] = _fp_value; } while (0)
    #define fp_pop() (g_fp_top = (g_fp_top + 1u) & 7u)
    #define fp_top() g_fp_stack[g_fp_top]
    #define fp_st(i) g_fp_stack[(g_fp_top + (i)) & 7u]
    #define fp_st1() fp_st(1)

    /* PATCH (RECOMP_FPS): at most once per 60 Hz frame. The frame time
     * (the float argument, in 59.94 Hz frames) of the calls skipped is
     * added to the next one, which is then what a 60 fps frame hands it. */
    static int every = -1;                /* RECOMP_SOUND_EVERY_FRAME=1: off */
    if (every < 0) {
        const char *e = getenv("RECOMP_SOUND_EVERY_FRAME");
        every = e && *e == '1';
    }
    if (patch_fps() && !every) {
        static float acc;
        acc += MEMF(esp + 4);
        if (acc < 0.99f) {
            esp += 8; return; /* ret 4, skipped */
        }
        MEMF(esp + 4) = acc;
        acc = 0.0f;
    }
    emitter_reap();
loc_001AD630: ;
    PUSH32(esp, esi);
    esi = ecx;
    SET_LO8(eax, MEM8(esi + 0x10332));
    _fa = (uint32_t)(LO8(eax)) & 0xFFu; _fb = (uint32_t)(0) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* cmp LO8(eax), 0 (8-bit) */
    if (CMP_NE(_fa, _fb)) goto loc_001AD6E4; /* jne: not equal / not zero */

loc_001AD641: ;
    xmm0 = XMM_SCALAR(MEMF(esi + 0x100C8)); /* movss */
    _fca = xmm0.f[0]; _fcb = MEMF(esi + 0x100D0); /* comiss */
    if ((_fca < _fcb || (_fca != _fca || _fcb != _fcb))) goto loc_001AD667; /* jb: below (unsigned <) (xmm0.f[0] vs MEMF(esi + 0x100D0)) */

loc_001AD652: ;
    eax = MEM32(esi + 0x100C0);
    MEM32(esi + 0x100BC) = eax;
    MEM8(esi + 0x100CC) = 0;
    goto loc_001AD6A0;

loc_001AD667: ;
    xmm0 = XMM_SCALAR(MEMF(esp + 8)); /* movss */
    xmm1 = XMM_SCALAR(MEMF(esi + 0x100C4)); /* movss */
    xmm1.f[0] = xmm1.f[0] * xmm0.f[0]; /* mulss */
    xmm1.f[0] = xmm1.f[0] + MEMF(esi + 0x100BC); /* addss */
    xmm0.f[0] = xmm0.f[0] + MEMF(esi + 0x100C8); /* addss */
    MEMF(esi + 0x100BC) = xmm1.f[0]; /* movss */
    MEMF(esi + 0x100C8) = xmm0.f[0]; /* movss */
    MEM8(esi + 0x100CC) = 1;

loc_001AD6A0: ;
    PUSH32(esp, 0x001AD6A5u); RECOMP_ABI_CALL(0x001950E3u, sub_001950E3); /* call 0x001950E3 */

loc_001AD6A5: ;
    ecx = esi;
    PUSH32(esp, 0x001AD6ACu); RECOMP_ABI_CALL(0x001AD2E0u, sub_001AD2E0); /* call 0x001AD2E0 */

loc_001AD6AC: ;
    ecx = MEM32(esp + 8);
    PUSH32(esp, ecx);
    ecx = MEM32(0x524300);
    PUSH32(esp, 0x001AD6BCu); RECOMP_ABI_CALL(0x001B1460u, sub_001B1460); /* call 0x001B1460 */

loc_001AD6BC: ;
    PUSH32(esp, 0x001AD6C1u); RECOMP_ABI_CALL(0x001950F1u, sub_001950F1); /* call 0x001950F1 */

loc_001AD6C1: ;
    eax = MEM32(esi + 0x1032C);
    edx = ((int32_t)eax < 0) ? 0xFFFFFFFF : 0; /* cdq */
    ecx = 0x14;
    { int64_t _dividend = ((int64_t)(int32_t)edx << 32) | eax;
      eax = (uint32_t)((int32_t)(_dividend / (int32_t)ecx));
      edx = (uint32_t)((int32_t)(_dividend % (int32_t)ecx)); }
    _fa = (uint32_t)(edx) & 0xFFFFFFFFu; _fb = (uint32_t)(0) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp edx, 0 (32-bit) */
    if (CMP_NE(_fa, _fb)) goto loc_001AD6DE; /* jne: not equal / not zero */

loc_001AD6D3: ;
    PUSH32(esp, 0x001AD6D8u); RECOMP_ABI_CALL(0x002B0661u, sub_002B0661); /* call 0x002B0661 */

loc_001AD6D8: ;
    MEMF(esi + 0x10328) = (float)fp_top(); fp_pop(); /* fstp */

loc_001AD6DE: ;
    MEM32(esi + 0x1032C) = MEM32(esi + 0x1032C) + 1;
    _fa = (uint32_t)(MEM32(esi + 0x1032C)) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fb = (_fa == 0x80000000u); /* inc result/SF/OF; CF unchanged */

loc_001AD6E4: ;
    POP32(esp, esi);
    esp += 8; return; /* ret 4 */

    #undef fp_push
    #undef fp_pop
    #undef fp_top
    #undef fp_st
    #undef fp_st1
}

/* ---- native mouse aim ----------------------------------------------------
 *
 * The look update, sub_00114D20 (thiscall on the character; the stick's x
 * and y as floats), shapes the right stick through response curves, aim
 * slow-downs and ramps, then steps the view: sub_001149E0 adds
 *   arg * this->turn_speed (+0x1C8) * (1/59.94)
 * to the yaw at +0x110 (radians, wrapped at 2 pi, larger = further left)
 * and rebuilds the orientation from it; sub_00114B60 does the same for the
 * pitch, with the weapon's kick and recovery on top. Both are called from
 * there alone.
 *
 * PATCH: the mouse goes into those two arguments as the exact angle it
 * moved (mkb_native_aim_take, input/mkb_input.c): no curve, no ramp, no
 * cap, at any frame rate -- the yaw step takes the mouse since the last
 * frame and leaves the pitch part for the pitch step of the same character.
 * Off unless RECOMP_MKB is on and the game has the foreground; while it is
 * taking the mouse, the mouse no longer drives the emulated stick. */
int mkb_native_aim_take(float *yaw, float *pitch);
extern float game_zoom_scale(void);   /* recomp_manual.c: 1, or the scope zoom factor while zoomed (SCOPE ZOOM) */

static float    s_aim_pitch;        /* taken with the yaw, for the pitch step */
static uint32_t s_aim_this;         /* the character it was taken for */

/* Which characters run the look update, and how often (RECOMP_AIM_LOG=1). */
static void aim_log(uint32_t self)
{
    static int on = -1;
    static uint32_t who[8];
    static unsigned calls[8], n, total;
    static DWORD last;
    unsigned i;
    if (on < 0) { const char *e = getenv("RECOMP_AIM_LOG"); on = e && *e == '1'; }
    if (!on) return;
    for (i = 0; i < n && who[i] != self; i++) {}
    if (i == n && n < 8) who[n++] = self;
    if (i < 8) calls[i]++;
    total++;
    if (GetTickCount() - last > 5000) {
        last = GetTickCount();
        fprintf(stderr, "[AIM] %u look updates:", total);
        for (i = 0; i < n; i++) fprintf(stderr, " %08X x%u", who[i], calls[i]);
        fprintf(stderr, "\n");
    }
}

/* The character the local player controls: the look update runs for it
 * alone (RECOMP_AIM_LOG showed one object), so the yaw step records it. */
static uint32_t s_player;

/* Split-screen: more than one character runs the look update. The mouse belongs to player 1 alone. Characters seen in the
 * last second are tracked; player 1 is the highest address among them. With one character
 * (single player) that one is player 1. */
static uint32_t s_look_who[4];
static DWORD    s_look_t[4];
static uint32_t s_p1;

static int look_is_p1(uint32_t self)
{
    DWORD now = GetTickCount();
    unsigned i, free_slot = 4;
    uint32_t low = 0;
    for (i = 0; i < 4; i++) {
        if (s_look_who[i] == self) { s_look_t[i] = now; break; }
        if (free_slot == 4 && (!s_look_who[i] || now - s_look_t[i] > 1000)) free_slot = i;
    }
    if (i == 4 && free_slot < 4) { s_look_who[free_slot] = self; s_look_t[free_slot] = now; }
    for (i = 0; i < 4; i++)
        if (s_look_who[i] && now - s_look_t[i] <= 1000 && (!low || s_look_who[i] > low))
            low = s_look_who[i];
    if (low != s_p1) {
        s_p1 = low;
        fprintf(stderr, "[AIM] player 1 character is now %08X\n", s_p1);
    }
    return self == s_p1;
}

static void aim_inject_yaw(void)
{
    float dyaw, dpitch, k;
    aim_log(ecx);
    s_aim_pitch = 0.0f;
    if (!look_is_p1(ecx))               /* the mouse is player 1's alone */
        return;
    s_player = ecx;
    s_aim_pitch = 0.0f;
    if (!mkb_native_aim_take(&dyaw, &dpitch))
        return;
    {   /* SCOPE ZOOM: the more the scope is zoomed, the slower the mouse turns */
        float zs = game_zoom_scale();
        dyaw *= zs;
        dpitch *= zs;
    }
    s_aim_this = ecx;
    s_aim_pitch = dpitch;
    k = MEMF(ecx + 0x1C8) * MEMF(0x2FF020);
    if (dyaw != 0.0f && (k > 1e-6f || k < -1e-6f))
        MEMF(esp + 4) = MEMF(esp + 4) - dyaw / k;   /* right = smaller yaw */
}

static void aim_inject_pitch(void)
{
    float dpitch = s_aim_pitch, k;
    s_aim_pitch = 0.0f;
    if (ecx != s_aim_this || dpitch == 0.0f)
        return;
    k = MEMF(ecx + 0x1C8) * MEMF(0x2FF020);
    if (k > 1e-6f || k < -1e-6f)
        MEMF(esp + 4) = MEMF(esp + 4) + dpitch / k;
}

void game_patch_001149E0(void)
{
    uint32_t ebp = 0;
    ebp = g_ebp;  /* prologue saves caller's frame */
    int _flags = 0; /* fallback flag var */
    uint32_t _fa = 0, _fb = 0;
    int32_t _fas = 0, _fbs = 0;
    (void)_fa; (void)_fb; (void)_fas; (void)_fbs;
    double _fca = 0.0, _fcb = 0.0;
    (void)_fca; (void)_fcb;

    aim_inject_yaw();   /* PATCH: native mouse aim */
loc_001149E0: ;
    PUSH32(esp, ebp);
    ebp = esp;
    g_ebp = ebp; /* publish frame for frameless callees */
    g_seh_ebp = ebp;
    esp = esp & 0xFFFFFFF0u;
    _fa = (uint32_t)(esp) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* and result */
    _fb = (uint32_t)(0x5C) & 0xFFFFFFFFu; _fbs = (int32_t)(int32_t)(_fb); /* sub source, before the write */
    esp = esp - 0x5C;
    _fa = (uint32_t)(esp) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* sub result */
    xmm1 = XMM_SCALAR(MEMF(ebp + 8)); /* movss */
    PUSH32(esp, esi);
    esi = ecx;
    xmm0 = XMM_SCALAR(MEMF(esi + 0x1C8)); /* movss */
    SET_LO8(eax, MEM8(esi + 0x128));
    _fa = (uint32_t)(LO8(eax)) & 0xFFu; _fb = (uint32_t)(0) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* cmp LO8(eax), 0 (8-bit) */
    xmm0.f[0] = xmm0.f[0] * xmm1.f[0]; /* mulss */
    xmm0.f[0] = xmm0.f[0] * MEMF(0x2FF020); /* mulss */
    xmm0.f[0] = xmm0.f[0] + MEMF(esi + 0x110); /* addss */
    MEMF(esi + 0x10C) = xmm1.f[0]; /* movss */
    MEMF(esi + 0x110) = xmm0.f[0]; /* movss */
    if (CMP_EQ(_fa, _fb)) goto loc_00114A4B; /* je: equal / zero */

loc_00114A27: ;
    xmm1 = XMM_SCALAR(MEMF(esi + 0x12C)); /* movss */
    _fca = xmm1.f[0]; _fcb = xmm0.f[0]; /* comiss */
    xmm2 = XMM_SCALAR(MEMF(esi + 0x130)); /* movss */
    if ((_fca <= _fcb || (_fca != _fca || _fcb != _fcb))) goto loc_00114A41; /* jbe: below or equal (unsigned <=) (xmm1.f[0] vs xmm0.f[0]) */

loc_00114A3C: ;
    xmm0 = xmm1; /* movaps */
    goto loc_00114A6F;

loc_00114A41: ;
    _fca = xmm0.f[0]; _fcb = xmm2.f[0]; /* comiss */
    if ((_fca <= _fcb || (_fca != _fca || _fcb != _fcb))) goto loc_00114A6F; /* jbe: below or equal (unsigned <=) (xmm0.f[0] vs xmm2.f[0]) */

loc_00114A46: ;
    xmm0 = xmm2; /* movaps */
    goto loc_00114A6F;

loc_00114A4B: ;
    xmm1 = XMM_SCALAR(MEMF(0x301054)); /* movss */
    _fca = xmm0.f[0]; _fcb = xmm1.f[0]; /* comiss */
    if ((_fca <= _fcb || (_fca != _fca || _fcb != _fcb))) goto loc_00114A5E; /* jbe: below or equal (unsigned <=) (xmm0.f[0] vs xmm1.f[0]) */

loc_00114A58: ;
    xmm0.f[0] = xmm0.f[0] - xmm1.f[0]; /* subss */
    goto loc_00114A6F;

loc_00114A5E: ;
    xmm2 = XMM_SCALAR(MEMF(0x319298)); /* movss */
    _fca = xmm2.f[0]; _fcb = xmm0.f[0]; /* comiss */
    if ((_fca <= _fcb || (_fca != _fca || _fcb != _fcb))) goto loc_00114A77; /* jbe: below or equal (unsigned <=) (xmm2.f[0] vs xmm0.f[0]) */

loc_00114A6B: ;
    xmm0.f[0] = xmm0.f[0] + xmm1.f[0]; /* addss */

loc_00114A6F: ;
    MEMF(esi + 0x110) = xmm0.f[0]; /* movss */

loc_00114A77: ;
    eax = MEM32(esi + 0x1C0);
    eax = eax >> 0xD;
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* shift result */
    _fa = (uint32_t)(LO8(eax)) & 0xFFu; _fb = (uint32_t)(1) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* test LO8(eax), 1 (8-bit) */
    if (TEST_Z(_fa, _fb)) goto loc_00114AD4; /* je: equal / zero */

loc_00114A84: ;
    xmm1 = XMM_SCALAR(MEMF(0x375F84)); /* movss */
    xmm1.f[0] = xmm1.f[0] * MEMF(0x2FF084); /* mulss */
    xmm0 = XMM_SCALAR(MEMF(esi + 0x110)); /* movss */
    xmm0.f[0] = xmm0.f[0] / xmm1.f[0]; /* divss */
    xmm1 = XMM_SCALAR(MEMF(0x319294)); /* movss */
    _fca = xmm1.f[0]; _fcb = xmm0.f[0]; /* comiss */
    MEMF(esi + 0x464) = xmm0.f[0]; /* movss */
    if ((_fca > _fcb)) goto loc_00114AC2; /* ja: above (unsigned >) (xmm1.f[0] vs xmm0.f[0]) */

loc_00114AB5: ;
    xmm1 = XMM_SCALAR(MEMF(0x303CFC)); /* movss */
    _fca = xmm0.f[0]; _fcb = xmm1.f[0]; /* comiss */
    if ((_fca <= _fcb || (_fca != _fca || _fcb != _fcb))) goto loc_00114AC5; /* jbe: below or equal (unsigned <=) (xmm0.f[0] vs xmm1.f[0]) */

loc_00114AC2: ;
    xmm0 = xmm1; /* movaps */

loc_00114AC5: ;
    MEMF(esi + 0x464) = xmm0.f[0]; /* movss */
    POP32(esp, esi);
    esp = ebp;
    POP32(esp, ebp);
    esp += 8; return; /* ret 4 */

loc_00114AD4: ;
    xmm0 = XMM_MEM(esi + 0x60); /* movaps */
    ecx = MEM32(esi + 0x110);
    XMM_STORE(esp + 0x20, xmm0); /* movaps */
    xmm0 = XMM_MEM(esi + 0x70); /* movaps */
    XMM_STORE(esp + 0x30, xmm0); /* movaps */
    xmm0 = XMM_MEM(esi + 0x80); /* movaps */
    XMM_STORE(esp + 0x40, xmm0); /* movaps */
    xmm0 = XMM_MEM(esi + 0x90); /* movaps */
    XMM_STORE(esp + 0x50, xmm0); /* movaps */
    xmm0 = XMM_SCALAR(MEMF(esp + 0x50)); /* movss */
    MEMF(esp + 0x10) = xmm0.f[0]; /* movss */
    xmm0 = XMM_SCALAR(MEMF(esp + 0x54)); /* movss */
    MEMF(esp + 0x14) = xmm0.f[0]; /* movss */
    xmm0 = XMM_SCALAR(MEMF(esp + 0x58)); /* movss */
    PUSH32(esp, ecx);
    ecx = esp + 0x24;
    MEMF(esp + 0x1C) = xmm0.f[0]; /* movss */
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x00114B32u); RECOMP_ABI_CALL(0x00265DA0u, sub_00265DA0); /* call 0x00265DA0 */

loc_00114B32: ;
    edx = esp + 0x10;
    PUSH32(esp, edx);
    ecx = esp + 0x24;
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x00114B40u); RECOMP_ABI_CALL(0x00264FA0u, sub_00264FA0); /* call 0x00264FA0 */

loc_00114B40: ;
    eax = MEM32(esi + 0x30);
    ecx = esi + 0x30;
    edx = esp + 0x20;
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, edx);
    { uint32_t _icall_target = MEM32(eax + 0xEC); PUSH32(esp, 0x00114B51u); RECOMP_ICALL_SAFE_AT(_icall_target, _icall_esp, 0x00114B4Bu); } /* indirect call */
    }

loc_00114B51: ;
    POP32(esp, esi);
    esp = ebp;
    POP32(esp, ebp);
    esp += 8; return; /* ret 4 */

}

void game_patch_00114B60(void)
{
    int _flags = 0; /* fallback flag var */
    uint32_t _fa = 0, _fb = 0;
    int32_t _fas = 0, _fbs = 0;
    (void)_fa; (void)_fb; (void)_fas; (void)_fbs;
    double _fca = 0.0, _fcb = 0.0;
    (void)_fca; (void)_fcb;

    if (look_is_p1(ecx))
        game_pc_note(ecx); /* PROJ-PROBE: player character pointer (player 1 only) */
    aim_inject_pitch(); /* PATCH: native mouse aim */
loc_00114B60: ;
    xmm1 = XMM_SCALAR(MEMF(esp + 4)); /* movss */
    xmm0 = XMM_SCALAR(MEMF(ecx + 0x14C)); /* movss */
    xmm2 = XMM_ZERO(); /* xorps self = zero */
    _fca = xmm0.f[0]; _fcb = xmm2.f[0]; /* comiss */
    xmm1.f[0] = xmm1.f[0] * MEMF(ecx + 0x1C8); /* mulss */
    xmm1.f[0] = xmm1.f[0] * MEMF(0x2FF020); /* mulss */
    if ((_fca <= _fcb || (_fca != _fca || _fcb != _fcb))) goto loc_00114C01; /* jbe: below or equal (unsigned <=) (xmm0.f[0] vs xmm2.f[0]) */

loc_00114B86: ;
    _fca = xmm2.f[0]; _fcb = xmm1.f[0]; /* comiss */
    xmm4 = xmm1; /* movaps */
    if ((_fca > _fcb)) goto loc_00114B91; /* ja: above (unsigned >) (xmm2.f[0] vs xmm1.f[0]) */

loc_00114B8E: ;
    xmm4 = xmm2; /* movaps */

loc_00114B91: ;
    xmm3 = XMM_SCALAR(MEMF(ecx + 0x14C)); /* movss */
    xmm0 = xmm3; /* movaps */
    xmm0.f[0] = xmm0.f[0] + xmm4.f[0]; /* addss */
    _fca = xmm2.f[0]; _fcb = xmm0.f[0]; /* comiss */
    if ((_fca <= _fcb || (_fca != _fca || _fcb != _fcb))) goto loc_00114BAA; /* jbe: below or equal (unsigned <=) (xmm2.f[0] vs xmm0.f[0]) */

loc_00114BA5: ;
    xmm3 = xmm2; /* movaps */
    goto loc_00114BB2;

loc_00114BAA: ;
    _fca = xmm0.f[0]; _fcb = xmm3.f[0]; /* comiss */
    if ((_fca > _fcb)) goto loc_00114BB2; /* ja: above (unsigned >) (xmm0.f[0] vs xmm3.f[0]) */

loc_00114BAF: ;
    xmm3 = xmm0; /* movaps */

loc_00114BB2: ;
    xmm0 = XMM_SCALAR(MEMF(ecx + 0x13C)); /* movss */
    xmm0.f[0] = xmm0.f[0] * MEMF(ecx + 0x1C8); /* mulss */
    xmm0.f[0] = xmm0.f[0] * MEMF(0x31929C); /* mulss */
    xmm4 = xmm2; /* movaps */
    xmm4.f[0] = xmm4.f[0] - xmm3.f[0]; /* subss */
    _fca = xmm4.f[0]; _fcb = xmm0.f[0]; /* comiss */
    MEMF(ecx + 0x14C) = xmm3.f[0]; /* movss */
    if ((_fca <= _fcb || (_fca != _fca || _fcb != _fcb))) goto loc_00114BE3; /* jbe: below or equal (unsigned <=) (xmm4.f[0] vs xmm0.f[0]) */

loc_00114BDE: ;
    xmm0 = xmm4; /* movaps */
    goto loc_00114BEB;

loc_00114BE3: ;
    _fca = xmm0.f[0]; _fcb = xmm2.f[0]; /* comiss */
    if ((_fca <= _fcb || (_fca != _fca || _fcb != _fcb))) goto loc_00114BEB; /* jbe: below or equal (unsigned <=) (xmm0.f[0] vs xmm2.f[0]) */

loc_00114BE8: ;
    xmm0 = xmm2; /* movaps */

loc_00114BEB: ;
    xmm4 = xmm0; /* movaps */
    xmm4.f[0] = xmm4.f[0] + xmm1.f[0]; /* addss */
    xmm3.f[0] = xmm3.f[0] + xmm0.f[0]; /* addss */
    xmm1 = xmm4; /* movaps */
    MEMF(ecx + 0x14C) = xmm3.f[0]; /* movss */

loc_00114C01: ;
    xmm5 = XMM_SCALAR(MEMF(ecx + 0x144)); /* movss */
    xmm0 = xmm5; /* movaps */
    _fca = xmm0.f[0]; _fcb = xmm2.f[0]; /* comiss */
    if ((_fca <= _fcb || (_fca != _fca || _fcb != _fcb))) goto loc_00114C20; /* jbe: below or equal (unsigned <=) (xmm0.f[0] vs xmm2.f[0]) */

loc_00114C11: ;
    xmm0.f[0] = xmm0.f[0] + xmm1.f[0]; /* addss */
    xmm1 = xmm0; /* movaps */
    MEMF(ecx + 0x144) = xmm2.f[0]; /* movss */

loc_00114C20: ;
    SET_LO8(eax, MEM8(ecx + 0x11C));
    _fa = (uint32_t)(LO8(eax)) & 0xFFu; _fb = (uint32_t)(0) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* cmp LO8(eax), 0 (8-bit) */
    xmm0 = xmm1; /* movaps */
    xmm0.f[0] = xmm0.f[0] + MEMF(ecx + 0x108); /* addss */
    if (CMP_EQ(_fa, _fb)) goto loc_00114C57; /* je: equal / zero */

loc_00114C35: ;
    xmm3 = XMM_SCALAR(MEMF(ecx + 0x120)); /* movss */
    _fca = xmm3.f[0]; _fcb = xmm0.f[0]; /* comiss */
    xmm4 = XMM_SCALAR(MEMF(ecx + 0x124)); /* movss */
    if ((_fca <= _fcb || (_fca != _fca || _fcb != _fcb))) goto loc_00114C4F; /* jbe: below or equal (unsigned <=) (xmm3.f[0] vs xmm0.f[0]) */

loc_00114C4A: ;
    xmm0 = xmm3; /* movaps */
    goto loc_00114C57;

loc_00114C4F: ;
    _fca = xmm0.f[0]; _fcb = xmm4.f[0]; /* comiss */
    if ((_fca <= _fcb || (_fca != _fca || _fcb != _fcb))) goto loc_00114C57; /* jbe: below or equal (unsigned <=) (xmm0.f[0] vs xmm4.f[0]) */

loc_00114C54: ;
    xmm0 = xmm4; /* movaps */

loc_00114C57: ;
    _fca = xmm1.f[0]; _fcb = xmm2.f[0]; /* comiss */
    xmm0.f[0] = xmm0.f[0] - MEMF(ecx + 0x108); /* subss */
    if ((_fca <= _fcb || (_fca != _fca || _fcb != _fcb))) goto loc_00114C6D; /* jbe: below or equal (unsigned <=) (xmm1.f[0] vs xmm2.f[0]) */

loc_00114C64: ;
    xmm1.f[0] = xmm1.f[0] - xmm0.f[0]; /* subss */
    xmm3 = xmm1; /* movaps */
    goto loc_00114C74;

loc_00114C6D: ;
    xmm3 = xmm0; /* movaps */
    xmm3.f[0] = xmm3.f[0] - xmm1.f[0]; /* subss */

loc_00114C74: ;
    _fca = xmm0.f[0]; _fcb = xmm2.f[0]; /* comiss */
    xmm1 = xmm5; /* movaps */
    xmm1.f[0] = xmm1.f[0] - xmm3.f[0]; /* subss */
    if ((_fca <= _fcb || (_fca != _fca || _fcb != _fcb))) goto loc_00114CA6; /* jbe: below or equal (unsigned <=) (xmm0.f[0] vs xmm2.f[0]) */

loc_00114C80: ;
    _fca = xmm2.f[0]; _fcb = xmm1.f[0]; /* comiss */
    if ((_fca <= _fcb || (_fca != _fca || _fcb != _fcb))) goto loc_00114C8A; /* jbe: below or equal (unsigned <=) (xmm2.f[0] vs xmm1.f[0]) */

loc_00114C85: ;
    xmm1 = xmm2; /* movaps */
    goto loc_00114C92;

loc_00114C8A: ;
    _fca = xmm1.f[0]; _fcb = xmm5.f[0]; /* comiss */
    if ((_fca <= _fcb || (_fca != _fca || _fcb != _fcb))) goto loc_00114C92; /* jbe: below or equal (unsigned <=) (xmm1.f[0] vs xmm5.f[0]) */

loc_00114C8F: ;
    xmm1 = xmm5; /* movaps */

loc_00114C92: ;
    xmm2 = XMM_SCALAR(MEMF(ecx + 0x14C)); /* movss */
    xmm2.f[0] = xmm2.f[0] + xmm1.f[0]; /* addss */
    MEMF(ecx + 0x14C) = xmm2.f[0]; /* movss */

loc_00114CA6: ;
    eax = MEM32(ecx + 0x1C0);
    eax = eax >> 0xD;
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* shift result */
    _fa = (uint32_t)(LO8(eax)) & 0xFFu; _fb = (uint32_t)(1) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* test LO8(eax), 1 (8-bit) */
    xmm1 = xmm0; /* movaps */
    xmm1.f[0] = xmm1.f[0] + MEMF(ecx + 0x108); /* addss */
    MEMF(ecx + 0x108) = xmm1.f[0]; /* movss */
    MEMF(ecx + 0x104) = xmm0.f[0]; /* movss */
    if (TEST_Z(_fa, _fb)) goto loc_00114D0F; /* je: equal / zero */

loc_00114CCE: ;
    xmm0 = XMM_SCALAR(MEMF(0x375F88)); /* movss */
    xmm0.f[0] = xmm0.f[0] * MEMF(0x2FF084); /* mulss */
    xmm1.f[0] = xmm1.f[0] / xmm0.f[0]; /* divss */
    xmm0 = XMM_SCALAR(MEMF(0x319294)); /* movss */
    _fca = xmm0.f[0]; _fcb = xmm1.f[0]; /* comiss */
    MEMF(ecx + 0x460) = xmm1.f[0]; /* movss */
    if ((_fca > _fcb)) goto loc_00114D04; /* ja: above (unsigned >) (xmm0.f[0] vs xmm1.f[0]) */

loc_00114CF7: ;
    xmm0 = XMM_SCALAR(MEMF(0x303CFC)); /* movss */
    _fca = xmm1.f[0]; _fcb = xmm0.f[0]; /* comiss */
    if ((_fca <= _fcb || (_fca != _fca || _fcb != _fcb))) goto loc_00114D07; /* jbe: below or equal (unsigned <=) (xmm1.f[0] vs xmm0.f[0]) */

loc_00114D04: ;
    xmm1 = xmm0; /* movaps */

loc_00114D07: ;
    MEMF(ecx + 0x460) = xmm1.f[0]; /* movss */

loc_00114D0F: ;
    esp += 8; return; /* ret 4 */

}

/* ---- sprint ---------------------------------------------------------------
 *
 * The character's move update, sub_0011AC60 (thiscall; vtable +0x104 of the
 * soldier classes; the stick as floats, forward/back first, then sideways,
 * then a third the stances clear), turns the stick into a step with
 * constants per stance -- forward 4.0 x a gait factor, sideways 0.1 x the
 * same, crouched 0.03 -- linearly in the stick.
 *
 * PATCH: while the sprint key is held (mkb_sprint_query, input/mkb_input.c:
 * Alt by default, mkb.ini sprint_*), the local player's stick input is
 * scaled by the sprint factor, so the same step code moves it that much
 * faster. Stamina: a sprint lasts sprint_time seconds of running forward;
 * spent, it is gone until the key is let go and half of it has come back;
 * it refills over sprint_recover seconds. Only the character the look
 * update runs for (s_player) sprints -- the AI shares this method. */
int mkb_sprint_query(float *speed, float *secs, float *recover);
void mkb_sprint_pad_moving(int moving, int tired);   /* ends the pad's toggled sprint */
extern void recomp_screenfx_bar(float x0, float y0, float x1, float y1, float fill);

/* Stamina bar show/hide (Recomp Settings row "Stamina bar", ini stamina_bar, env RECOMP_STAMINA_BAR; default on).
 * The overlay bar expires 200 ms after the last draw call, so not calling it hides the bar. */
static int s_stambar = -1;
int game_stambar_get(void)
{
    if (s_stambar < 0) {
        const char *e = getenv("RECOMP_STAMINA_BAR");
        s_stambar = !(e && *e == '0');
    }
    return s_stambar;
}
void game_stambar_set(int on)
{
    s_stambar = on ? 1 : 0;
}

/* Grenade countdown (Recomp Settings row "Grenade timer", ini grenade_timer, env RECOMP_GRENADE_TIMER; default on). The player's
 * thrown grenade goes through the projectile spawner sub_00182050 (def CRC DD8CB982/ED497BEA, type 9, fuse 5.0 in arg 5, owner =
 * the player). The fuse starts when the fire button goes down (pin pulled; measured: blast 4.5 s after the throw for a quick
 * throw, 1.8 s after a long hold), so the end is the last fire press + 5 s. */
/* Split-screen (Recomp Settings "Split-screen pads", RECOMP_SPLIT_PADS): the added single-player features (sprint with its stamina
 * bar, the grenade countdown, the screen overlays) are off, so multiplayer plays as the game made it. */
static int game_split_mode(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_SPLIT_PADS");
        on = e && *e && *e != '0';
    }
    return on;
}
extern volatile int g_recomp_rt_last;
extern void recomp_screenfx_timer(float x, float y, float h, int tenths);
static int s_grentm = -1;
int game_grentm_get(void)
{
    if (s_grentm < 0) {
        const char *e = getenv("RECOMP_GRENADE_TIMER");
        s_grentm = !(e && *e == '0');
    }
    return s_grentm;
}
void game_grentm_set(int on)
{
    s_grentm = on ? 1 : 0;
}
uint32_t game_pc_get(void);
extern volatile int g_recomp_dup_down;
extern volatile unsigned g_recomp_dup_press;
extern volatile unsigned g_recomp_b_press;
extern volatile unsigned g_recomp_a_press;
static float s_gt;              /* game-side clock: only runs while the player's frames run (a pause does not count) */
static float s_gren_press = -1000.0f;   /* clock of the last grenade-button press */
static float s_gren_end;        /* clock the grenade goes off; 0 = no countdown */
static float s_gren_rel;        /* clock the button was let go (-1: still held) */
static int s_gren_inhand;
static float s_rt_down_t, s_gun_t = -100.0f;   /* clock: fire went down / the last gun bullet spawned */
static int g_equipped;          /* the grenade control was pressed and no weapon change / throw since */
static int s_gren_thrown;       /* the spawner has seen the throw */

static void grenade_cd_tick(float dt)
{
    if (game_split_mode())
        return;
    static unsigned dseen, bseen, aseen;
    static int rtprev;
    int rt = g_recomp_rt_last > 40;
    if (dt > 0.1f)
        dt = 0.1f;
    s_gt += dt;
    if (g_recomp_dup_press != dseen) {                  /* grenade control: the grenade is in hand (pin still in) */
        dseen = g_recomp_dup_press;
        g_equipped = 1;
        fprintf(stderr, "[GRENTIMER] t %.2f grenade control pressed\n", s_gt);
    }
    if (g_recomp_b_press != bseen) {                    /* change weapon: may or may not have landed on the grenade; logged */
        bseen = g_recomp_b_press;
        if (s_gren_end <= 0.0f || !s_gren_thrown) {      /* a pulled pin that was never thrown: it was not a grenade */
            s_gren_end = 0.0f;
            g_equipped = 0;
        }
        fprintf(stderr, "[GRENTIMER] t %.2f B pressed (equipped=%d end=%.2f)\n", s_gt, g_equipped, s_gren_end);
    }
    if (g_recomp_a_press != aseen) {                    /* Action: with the grenade in hand it pulls the pin; the fuse (5 s) starts now */
        aseen = g_recomp_a_press;
        if (g_equipped && s_gren_end <= 0.0f) {
            s_gren_press = s_gt;
            s_gren_end = s_gt + 5.0f;
            s_gren_rel = -1.0f;
            s_gren_thrown = 0;
            fprintf(stderr, "[GRENTIMER] t %.2f pin pulled (Action with the grenade in hand)\n", s_gt);
        }
    }
    if (rtprev && !rt)
        s_gren_rel = s_gt;
    rtprev = rt;
    s_gren_inhand = g_equipped;
    if (s_gren_end > 0.0f) {
        float left = s_gren_end - s_gt;
        /* released, no throw seen for 1.5 s: there was no grenade to arm */
        if (left <= 0.0f) {
            if (!s_gren_thrown)
                g_equipped = 0;          /* the fuse ran out in the hand: no grenade was there */
            s_gren_end = 0.0f;
        }
        else if (!s_gren_thrown && s_gren_rel >= 0.0f && s_gt - s_gren_rel > 1.0f) {
            s_gren_end = 0.0f;          /* it was a gun shot, not a grenade */
            g_equipped = 0;
        }
        else if (game_grentm_get())
            recomp_screenfx_timer(0.955f, 0.45f, 0.075f, (int)(left * 10.0f + 0.99f));
    }
}

static void grenade_cd_spawn(uint32_t sp)
{
    if (MEM32(sp) == 0x0009F3A5u)
        s_gun_t = s_gt;
    uint32_t owner = MEM32(sp + 4 + 32);
    if (MEM32(sp + 4) != 0xDD8CB982u || MEM32(sp + 8) != 0xED497BEAu || MEM32(sp + 4 + 24) != 9u)
        return;
    if (!(game_pc_get() >= 0x80000000u && owner >= game_pc_get() && owner < game_pc_get() + 0x100))
        return;
    s_gren_thrown = 1;
    s_gren_rel = s_gt;
    if (s_gren_end <= s_gt || s_gt - s_gren_press > 5.5f) {   /* no pin pull seen: a direct throw, the fuse runs 4.5 s from the spawn */
        s_gren_end = s_gt + 4.5f;
        s_gren_press = s_gt - 0.5f;
    }

    fprintf(stderr, "[GRENTIMER] throw: press age %.2f s, left %.2f s\n", s_gt - s_gren_press, s_gren_end - s_gt);
}

static void sprint_apply(void)
{
    static LARGE_INTEGER freq, last;
    static float stamina = -1.0f;
    static int tired;
    LARGE_INTEGER now;
    float speed, secs, recover, dt;
    int held;

    if (!s_player || ecx != s_player || game_split_mode())
        return;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    dt = last.QuadPart ? (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart : 0.0f;
    last = now;
    if (dt > 0.1f)
        dt = 0.1f;                      /* a pause or a load is not rest */
    grenade_cd_tick(dt);
    held = mkb_sprint_query(&speed, &secs, &recover);
    mkb_sprint_pad_moving(MEMF(esp + 4) > 0.2f, tired);
    if (stamina < 0.0f)
        stamina = secs;
    /* stamina bar above the adrenaline meter, window fractions measured on a 16:9 screenshot */
    if (game_stambar_get())
        recomp_screenfx_bar(0.7703f, 0.7800f, 0.9203f, 0.7960f, secs > 0.0f ? stamina / secs : 0.0f);
    if (held && !tired && stamina > 0.0f && MEMF(esp + 4) > 0.2f) {
        stamina -= dt;
        if (stamina <= 0.0f) {
            stamina = 0.0f;
            tired = 1;
            mkb_sprint_pad_moving(1, 1);
        }
        MEMF(esp + 4) = MEMF(esp + 4) * speed;
        MEMF(esp + 8) = MEMF(esp + 8) * speed;
        return;
    }
    stamina += dt * secs / recover;
    if (stamina > secs)
        stamina = secs;
    if (tired && !held && stamina >= 0.5f * secs)
        tired = 0;
}

void game_patch_0011AC60(void)
{
    uint32_t ebp = 0;
    ebp = g_ebp;  /* prologue saves caller's frame */
    int _flags = 0; /* fallback flag var */
    uint32_t _fa = 0, _fb = 0;
    int32_t _fas = 0, _fbs = 0;
    (void)_fa; (void)_fb; (void)_fas; (void)_fbs;
    double _fca = 0.0, _fcb = 0.0;
    (void)_fca; (void)_fcb;
    #define fp_push(v) do { double _fp_value = (v); \
        g_fp_top = (g_fp_top + 7u) & 7u; \
        g_fp_stack[g_fp_top] = _fp_value; } while (0)
    #define fp_pop() (g_fp_top = (g_fp_top + 1u) & 7u)
    #define fp_top() g_fp_stack[g_fp_top]
    #define fp_st(i) g_fp_stack[(g_fp_top + (i)) & 7u]
    #define fp_st1() fp_st(1)

    sprint_apply();     /* PATCH: sprint */
loc_0011AC60: ;
    PUSH32(esp, ebp);
    ebp = esp;
    g_ebp = ebp; /* publish frame for frameless callees */
    g_seh_ebp = ebp;
    esp = esp & 0xFFFFFFF0u;
    _fa = (uint32_t)(esp) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* and result */
    _fb = (uint32_t)(0xA8) & 0xFFFFFFFFu; _fbs = (int32_t)(int32_t)(_fb); /* sub source, before the write */
    esp = esp - 0xA8;
    _fa = (uint32_t)(esp) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* sub result */
    PUSH32(esp, ebx);
    PUSH32(esp, esi);
    esi = ecx;
    eax = MEM32(esi + 0x1C0);
    ecx = eax;
    ecx = ecx >> 0xD;
    _fa = (uint32_t)(ecx) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* shift result */
    _fa = (uint32_t)(LO8(ecx)) & 0xFFu; _fb = (uint32_t)(1) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* test LO8(ecx), 1 (8-bit) */
    if (TEST_NZ(_fa, _fb)) goto loc_0011B184; /* jne: not equal / not zero */

loc_0011AC84: ;
    xmm0 = XMM_ZERO(); /* xorps self = zero */
    edx = eax;
    edx = edx >> 0xB;
    _fa = (uint32_t)(edx) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* shift result */
    _fa = (uint32_t)(LO8(edx)) & 0xFFu; _fb = (uint32_t)(1) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* test LO8(edx), 1 (8-bit) */
    if (TEST_Z(_fa, _fb)) goto loc_0011ACA0; /* je: equal / zero */

loc_0011AC91: ;
    MEMF(ebp + 0x10) = xmm0.f[0]; /* movss */
    MEMF(ebp + 8) = xmm0.f[0]; /* movss */
    MEMF(ebp + 0xC) = xmm0.f[0]; /* movss */

loc_0011ACA0: ;
    eax = eax >> 0x12;
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* shift result */
    _fa = (uint32_t)(LO8(eax)) & 0xFFu; _fb = (uint32_t)(1) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* test LO8(eax), 1 (8-bit) */
    if (TEST_Z(_fa, _fb)) goto loc_0011ACE5; /* je: equal / zero */

loc_0011ACA7: ;
    xmm1 = XMM_SCALAR(MEMF(ebp + 0xC)); /* movss */
    eax = MEM32(esi);
    MEMF(esi + 0x430) = xmm1.f[0]; /* movss */
    xmm1 = XMM_SCALAR(MEMF(ebp + 8)); /* movss */
    ecx = MEM32(esi + 0x430);
    MEMF(esi + 0x434) = xmm1.f[0]; /* movss */
    edx = MEM32(esi + 0x434);
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, ecx);
    PUSH32(esp, edx);
    ecx = esi;
    MEMF(ebp + 0xC) = xmm0.f[0]; /* movss */
    MEMF(ebp + 8) = xmm0.f[0]; /* movss */
    { uint32_t _icall_target = MEM32(eax + 0x150); PUSH32(esp, 0x0011ACE3u); RECOMP_ICALL_SAFE_AT(_icall_target, _icall_esp, 0x0011ACDDu); } /* indirect call */
    }

loc_0011ACE3: ;
    goto loc_0011ACF5;

loc_0011ACE5: ;
    MEMF(esi + 0x430) = xmm0.f[0]; /* movss */
    MEMF(esi + 0x434) = xmm0.f[0]; /* movss */

loc_0011ACF5: ;
    eax = MEM32(esi);
    ecx = esi;
    { uint32_t _icall_esp = g_esp;
    { uint32_t _icall_target = MEM32(eax + 0x10C); PUSH32(esp, 0x0011ACFFu); RECOMP_ICALL_SAFE_AT(_icall_target, _icall_esp, 0x0011ACF9u); } /* indirect call */
    }

loc_0011ACFF: ;
    MEMF(esp + 0xC) = (float)fp_top(); fp_pop(); /* fstp */
    _fa = (uint32_t)(MEM8(esi + 0x1C0)) & 0xFFu; _fb = (uint32_t)(1) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* test MEM8(esi + 0x1C0), 1 (8-bit) */
    if (TEST_Z(_fa, _fb)) goto loc_0011AD4E; /* je: equal / zero */

loc_0011AD0C: ;
    xmm3 = XMM_ZERO(); /* xorps self = zero */
    xmm2 = XMM_SCALAR(MEMF(ebp + 8)); /* movss */
    _fca = xmm3.f[0]; _fcb = xmm2.f[0]; /* comiss */
    xmm1 = XMM_SCALAR(MEMF(esp + 0xC)); /* movss */
    xmm0 = XMM_SCALAR(MEMF(0x376168)); /* movss */
    if ((_fca > _fcb)) goto loc_0011AD2F; /* ja: above (unsigned >) (xmm3.f[0] vs xmm2.f[0]) */

loc_0011AD27: ;
    xmm0 = XMM_SCALAR(MEMF(0x376164)); /* movss */

loc_0011AD2F: ;
    xmm0.f[0] = xmm0.f[0] * xmm1.f[0]; /* mulss */
    xmm0.f[0] = xmm0.f[0] * xmm2.f[0]; /* mulss */
    MEMF(ebp + 8) = xmm0.f[0]; /* movss */
    xmm0 = XMM_SCALAR(MEMF(0x37616C)); /* movss */
    MEMF(ebp + 0x10) = xmm3.f[0]; /* movss */
    goto loc_0011ADE6;

loc_0011AD4E: ;
    ecx = MEM32(esi + 0x1C0);
    ecx = ecx >> 0x1F;
    _fa = (uint32_t)(ecx) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* shift result */
    _fa = (uint32_t)(LO8(ecx)) & 0xFFu; _fb = (uint32_t)(1) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* test LO8(ecx), 1 (8-bit) */
    if (TEST_Z(_fa, _fb)) goto loc_0011AD9B; /* je: equal / zero */

loc_0011AD5C: ;
    xmm3 = XMM_ZERO(); /* xorps self = zero */
    xmm2 = XMM_SCALAR(MEMF(ebp + 8)); /* movss */
    _fca = xmm3.f[0]; _fcb = xmm2.f[0]; /* comiss */
    xmm1 = XMM_SCALAR(MEMF(esp + 0xC)); /* movss */
    xmm0 = XMM_SCALAR(MEMF(0x376178)); /* movss */
    if ((_fca > _fcb)) goto loc_0011AD7F; /* ja: above (unsigned >) (xmm3.f[0] vs xmm2.f[0]) */

loc_0011AD77: ;
    xmm0 = XMM_SCALAR(MEMF(0x376174)); /* movss */

loc_0011AD7F: ;
    xmm0.f[0] = xmm0.f[0] * xmm1.f[0]; /* mulss */
    xmm0.f[0] = xmm0.f[0] * xmm2.f[0]; /* mulss */
    MEMF(ebp + 8) = xmm0.f[0]; /* movss */
    xmm0 = XMM_SCALAR(MEMF(0x37617C)); /* movss */
    MEMF(ebp + 0x10) = xmm3.f[0]; /* movss */
    goto loc_0011ADE6;

loc_0011AD9B: ;
    ecx = esi + 0x13D4;
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x0011ADA6u); RECOMP_ABI_CALL(0x00127A40u, sub_00127A40); /* call 0x00127A40 */

loc_0011ADA6: ;
    fp_top() = RECOMP_FP_PC(fp_top() * MEMF(esp + 0xC)); /* fmul dword ptr [esp + 0xc] */
    xmm0 = XMM_ZERO(); /* xorps self = zero */
    xmm2 = XMM_SCALAR(MEMF(ebp + 8)); /* movss */
    MEMF(esp + 0xC) = (float)fp_top(); fp_pop(); /* fstp */
    _fca = xmm0.f[0]; _fcb = xmm2.f[0]; /* comiss */
    xmm1 = XMM_SCALAR(MEMF(esp + 0xC)); /* movss */
    xmm0 = XMM_SCALAR(MEMF(0x37614C)); /* movss */
    if ((_fca > _fcb)) goto loc_0011ADD1; /* ja: above (unsigned >) (xmm0.f[0] vs xmm2.f[0]) */

loc_0011ADC9: ;
    xmm0 = XMM_SCALAR(MEMF(0x376148)); /* movss */

loc_0011ADD1: ;
    xmm0.f[0] = xmm0.f[0] * xmm1.f[0]; /* mulss */
    xmm0.f[0] = xmm0.f[0] * xmm2.f[0]; /* mulss */
    MEMF(ebp + 8) = xmm0.f[0]; /* movss */
    xmm0 = XMM_SCALAR(MEMF(0x376150)); /* movss */

loc_0011ADE6: ;
    edx = MEM32(ebp + 0x10);
    eax = MEM32(ebp + 8);
    xmm0.f[0] = xmm0.f[0] * xmm1.f[0]; /* mulss */
    xmm0.f[0] = xmm0.f[0] * MEMF(ebp + 0xC); /* mulss */
    PUSH32(esp, edx);
    MEMF(ebp + 0xC) = xmm0.f[0]; /* movss */
    ecx = MEM32(ebp + 0xC);
    PUSH32(esp, eax);
    PUSH32(esp, ecx);
    ecx = esi;
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x0011AE07u); RECOMP_ABI_CALL(0x00113E40u, sub_00113E40); /* call 0x00113E40 */

loc_0011AE07: ;
    eax = MEM32(esi + 0x1C0);
    edx = eax;
    edx = edx >> 0x12;
    _fa = (uint32_t)(edx) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* shift result */
    _fa = (uint32_t)(LO8(edx)) & 0xFFu; _fb = (uint32_t)(1) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* test LO8(edx), 1 (8-bit) */
    if (TEST_Z(_fa, _fb)) goto loc_0011AE3E; /* je: equal / zero */

loc_0011AE17: ;
    xmm0 = XMM_SCALAR(MEMF(0x376188)); /* movss */
    xmm1 = xmm0; /* movaps */
    xmm0.f[0] = xmm0.f[0] * MEMF(ebp + 8); /* mulss */
    xmm1.f[0] = xmm1.f[0] * MEMF(ebp + 0xC); /* mulss */
    MEMF(ebp + 8) = xmm0.f[0]; /* movss */
    xmm0 = XMM_ZERO(); /* xorps self = zero */
    MEMF(ebp + 0xC) = xmm1.f[0]; /* movss */
    MEMF(ebp + 0x10) = xmm0.f[0]; /* movss */

loc_0011AE3E: ;
    eax = eax >> 4;
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* shift result */
    ebx = 0; /* xor self */
    _fa = (uint32_t)(ebx) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* xor result */
    _fa = (uint32_t)(LO8(eax)) & 0xFFu; _fb = (uint32_t)(1) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* test LO8(eax), 1 (8-bit) */
    if (TEST_NZ(_fa, _fb)) goto loc_0011AFBF; /* jne: not equal / not zero */

loc_0011AE4B: ;
    xmm0 = XMM_SCALAR(MEMF(ebp + 0x10)); /* movss */
    _fca = xmm0.f[0]; _fcb = MEMF(0x3210B0); /* comiss */
    if ((_fca <= _fcb || (_fca != _fca || _fcb != _fcb))) goto loc_0011AFBF; /* jbe: below or equal (unsigned <=) (xmm0.f[0] vs MEMF(0x3210B0)) */

loc_0011AE5D: ;
    eax = MEM32(esi);
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 4);
    ecx = esi;
    { uint32_t _icall_target = MEM32(eax + 0x5C); PUSH32(esp, 0x0011AE66u); RECOMP_ICALL_SAFE_AT(_icall_target, _icall_esp, 0x0011AE63u); } /* indirect call */
    }

loc_0011AE66: ;
    _fa = (uint32_t)(LO8(eax)) & 0xFFu; _fb = (uint32_t)(0) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* cmp LO8(eax), 0 (8-bit) */
    if (CMP_EQ(_fa, _fb)) goto loc_0011AFB7; /* je: equal / zero */

loc_0011AE6E: ;
    edx = MEM32(esi);
    ecx = esi;
    { uint32_t _icall_esp = g_esp;
    { uint32_t _icall_target = MEM32(edx + 0xE8); PUSH32(esp, 0x0011AE78u); RECOMP_ICALL_SAFE_AT(_icall_target, _icall_esp, 0x0011AE72u); } /* indirect call */
    }

loc_0011AE78: ;
    _fa = (uint32_t)(LO8(eax)) & 0xFFu; _fb = (uint32_t)(0) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* cmp LO8(eax), 0 (8-bit) */
    if (CMP_EQ(_fa, _fb)) goto loc_0011AFB7; /* je: equal / zero */

loc_0011AE80: ;
    eax = MEM32(esi);
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 2);
    ecx = esi;
    { uint32_t _icall_target = MEM32(eax + 0x60); PUSH32(esp, 0x0011AE89u); RECOMP_ICALL_SAFE_AT(_icall_target, _icall_esp, 0x0011AE86u); } /* indirect call */
    }

loc_0011AE89: ;
    ecx = (uint32_t)(int32_t)SMEM16(esi + 0x330);
    xmm1 = XMM_ZERO(); /* xorps self = zero */
    xmm0 = XMM_SCALAR(MEMF(0x2FE99C)); /* movss */
    MEM32(esp + 0x68) = ecx;
    ecx = esp + 0x40;
    MEM32(esp + 0x44) = ebx;
    MEM32(esp + 0x48) = ebx;
    MEM32(esp + 0x5C) = ebx;
    MEM32(esp + 0x50) = ebx;
    MEM32(esp + 0x54) = ebx;
    MEM32(esp + 0x58) = ebx;
    MEMF(esp + 0x4C) = xmm1.f[0]; /* movss */
    MEMF(esp + 0x70) = xmm0.f[0]; /* movss */
    MEMF(esp + 0x78) = xmm0.f[0]; /* movss */
    MEM32(esp + 0x40) = 0x30BC64;
    MEM32(esp + 0x88) = ebx;
    MEM32(esp + 0x8C) = ebx;
    MEM32(esp + 0xA0) = ebx;
    MEM32(esp + 0x94) = ebx;
    MEM32(esp + 0x98) = ebx;
    MEM32(esp + 0x9C) = ebx;
    MEMF(esp + 0x90) = xmm1.f[0]; /* movss */
    MEM32(esp + 0x84) = 0x2FF9EC;
    MEM32(esp + 0xA4) = ebx;
    MEMF(esp + 0xA8) = xmm0.f[0]; /* movss */
    MEM8(esp + 0xAC) = LO8(ebx);
    MEM8(esp + 0x2C) = LO8(ebx);
    MEM32(esp + 0x28) = 0x2FFA54;
    MEM32(esp + 0x34) = ebx;
    MEM32(esp + 0x38) = ebx;
    MEM32(esp + 0x3C) = ebx;
    MEM32(esp + 0x30) = 0x2FF02C;
    MEM32(esp + 0x74) = 1;
    MEM32(esp + 0x60) = ebx;
    MEMF(esp + 0x64) = xmm1.f[0]; /* movss */
    MEM32(esp + 0x6C) = 0x11;
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x0011AF69u); RECOMP_ABI_CALL(0x00143580u, sub_00143580); /* call 0x00143580 */

loc_0011AF69: ;
    ecx = MEM32(esp + 0x34);
    _fa = (uint32_t)(ecx) & 0xFFFFFFFFu; _fb = (uint32_t)(ebx) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp ecx, ebx (32-bit) */
    MEM32(esp + 0x28) = 0x2FFA54;
    MEM32(esp + 0x30) = 0x2FEC40;
    if (CMP_EQ(_fa, _fb)) goto loc_0011AF8B; /* je: equal / zero */

loc_0011AF81: ;
    edx = esp + 0x30;
    PUSH32(esp, edx);
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x0011AF8Bu); RECOMP_ABI_CALL(0x0012F650u, sub_0012F650); /* call 0x0012F650 */

loc_0011AF8B: ;
    eax = MEM32(esi + 0x168);
    ecx = esi + 0x13D4;
    MEM32(esi + 0x178) = eax;
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x0011AFA2u); RECOMP_ABI_CALL(0x00127A50u, sub_00127A50); /* call 0x00127A50 */

loc_0011AFA2: ;
    fp_top() = RECOMP_FP_PC(fp_top() * MEMF(0x37615C)); /* fmul dword ptr [0x37615c] */
    edx = MEM32(esi);
    ecx = esi;
    MEMF(ebp + 0x10) = (float)fp_top(); fp_pop(); /* fstp */
    { uint32_t _icall_esp = g_esp;
    { uint32_t _icall_target = MEM32(edx + 0xEC); PUSH32(esp, 0x0011AFB5u); RECOMP_ICALL_SAFE_AT(_icall_target, _icall_esp, 0x0011AFAFu); } /* indirect call */
    }

loc_0011AFB5: ;
    goto loc_0011AFBF;

loc_0011AFB7: ;
    xmm0 = XMM_ZERO(); /* xorps self = zero */
    MEMF(ebp + 0x10) = xmm0.f[0]; /* movss */

loc_0011AFBF: ;
    ecx = MEM32(esi + 0x1CC);
    _fa = (uint32_t)(ecx) & 0xFFFFFFFFu; _fb = (uint32_t)(ebx) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp ecx, ebx (32-bit) */
    if (CMP_EQ(_fa, _fb)) goto loc_0011AFDC; /* je: equal / zero */

loc_0011AFC9: ;
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x0011AFCEu); RECOMP_ABI_CALL(0x000F3C30u, sub_000F3C30); /* call 0x000F3C30 */

loc_0011AFCE: ;
    fp_push(MEMF(ebp + 0xC)); /* fld float */
    fp_top() = RECOMP_FP_PC(fp_top() * fp_st1()); /* fmul st(1) */
    MEMF(ebp + 0xC) = (float)fp_top(); fp_pop(); /* fstp */
    fp_top() = RECOMP_FP_PC(fp_top() * MEMF(ebp + 8)); /* fmul dword ptr [ebp + 8] */
    MEMF(ebp + 8) = (float)fp_top(); fp_pop(); /* fstp */

loc_0011AFDC: ;
    xmm0 = XMM_MEM(esi + 0x60); /* movaps */
    XMM_STORE(esp + 0x40, xmm0); /* movaps */
    xmm0 = XMM_MEM(esi + 0x70); /* movaps */
    XMM_STORE(esp + 0x50, xmm0); /* movaps */
    xmm0 = XMM_MEM(esi + 0x80); /* movaps */
    xmm1 = XMM_SCALAR(MEMF(esp + 0x50)); /* movss */
    XMM_STORE(esp + 0x60, xmm0); /* movaps */
    xmm0 = XMM_SCALAR(MEMF(esi + 0x1C8)); /* movss */
    xmm0.f[0] = xmm0.f[0] * MEMF(0x2FF020); /* mulss */
    MEMF(esp + 0x10) = xmm1.f[0]; /* movss */
    xmm1 = XMM_SCALAR(MEMF(esp + 0x54)); /* movss */
    PUSH32(esp, ecx);
    MEMF(esp + 0x10) = xmm0.f[0]; /* movss */
    xmm0.f[0] = xmm0.f[0] * MEMF(ebp + 8); /* mulss */
    MEMF(esp + 0x18) = xmm1.f[0]; /* movss */
    xmm1 = XMM_ZERO(); /* xorps self = zero */
    ecx = esp + 0x14;
    MEMF(esp + 0x1C) = xmm1.f[0]; /* movss */
    MEMF(esp) = xmm0.f[0]; /* movss */
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x0011B045u); RECOMP_ABI_CALL(0x00266200u, sub_00266200); /* call 0x00266200 */

loc_0011B045: ;
    xmm0 = XMM_SCALAR(MEMF(esi + 0x170)); /* movss */
    xmm0.f[0] = xmm0.f[0] + MEMF(esp + 0x10); /* addss */
    MEMF(esi + 0x170) = xmm0.f[0]; /* movss */
    xmm0 = XMM_SCALAR(MEMF(esp + 0x14)); /* movss */
    xmm0.f[0] = xmm0.f[0] + MEMF(esi + 0x174); /* addss */
    MEMF(esi + 0x174) = xmm0.f[0]; /* movss */
    xmm0 = XMM_SCALAR(MEMF(esp + 0x18)); /* movss */
    xmm0.f[0] = xmm0.f[0] + MEMF(esi + 0x178); /* addss */
    MEMF(esi + 0x178) = xmm0.f[0]; /* movss */
    xmm0 = XMM_SCALAR(MEMF(esp + 0x40)); /* movss */
    MEMF(esp + 0x10) = xmm0.f[0]; /* movss */
    xmm0 = XMM_SCALAR(MEMF(esp + 0x44)); /* movss */
    MEMF(esp + 0x14) = xmm0.f[0]; /* movss */
    xmm0 = XMM_ZERO(); /* xorps self = zero */
    MEMF(esp + 0x18) = xmm0.f[0]; /* movss */
    xmm0 = XMM_SCALAR(MEMF(esp + 0xC)); /* movss */
    xmm0.f[0] = xmm0.f[0] * MEMF(ebp + 0xC); /* mulss */
    PUSH32(esp, ecx);
    ecx = esp + 0x14;
    MEMF(esp) = xmm0.f[0]; /* movss */
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x0011B0C2u); RECOMP_ABI_CALL(0x00266200u, sub_00266200); /* call 0x00266200 */

loc_0011B0C2: ;
    xmm0 = XMM_SCALAR(MEMF(esi + 0x170)); /* movss */
    xmm0.f[0] = xmm0.f[0] + MEMF(esp + 0x10); /* addss */
    MEMF(esi + 0x170) = xmm0.f[0]; /* movss */
    xmm0 = XMM_SCALAR(MEMF(esp + 0x14)); /* movss */
    xmm0.f[0] = xmm0.f[0] + MEMF(esi + 0x174); /* addss */
    MEMF(esi + 0x174) = xmm0.f[0]; /* movss */
    xmm0 = XMM_SCALAR(MEMF(esp + 0x18)); /* movss */
    xmm0.f[0] = xmm0.f[0] + MEMF(esi + 0x178); /* addss */
    MEMF(esi + 0x178) = xmm0.f[0]; /* movss */
    xmm0 = XMM_SCALAR(MEMF(esp + 0x60)); /* movss */
    MEMF(esp + 0x10) = xmm0.f[0]; /* movss */
    xmm0 = XMM_SCALAR(MEMF(esp + 0x64)); /* movss */
    MEMF(esp + 0x14) = xmm0.f[0]; /* movss */
    xmm0 = XMM_SCALAR(MEMF(esp + 0x68)); /* movss */
    MEMF(esp + 0x18) = xmm0.f[0]; /* movss */
    xmm0 = XMM_SCALAR(MEMF(esp + 0xC)); /* movss */
    xmm0.f[0] = xmm0.f[0] * MEMF(ebp + 0x10); /* mulss */
    PUSH32(esp, ecx);
    ecx = esp + 0x14;
    MEMF(esp) = xmm0.f[0]; /* movss */
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x0011B142u); RECOMP_ABI_CALL(0x00266200u, sub_00266200); /* call 0x00266200 */

loc_0011B142: ;
    xmm0 = XMM_SCALAR(MEMF(esi + 0x170)); /* movss */
    xmm0.f[0] = xmm0.f[0] + MEMF(esp + 0x10); /* addss */
    MEMF(esi + 0x170) = xmm0.f[0]; /* movss */
    xmm0 = XMM_SCALAR(MEMF(esp + 0x14)); /* movss */
    xmm0.f[0] = xmm0.f[0] + MEMF(esi + 0x174); /* addss */
    MEMF(esi + 0x174) = xmm0.f[0]; /* movss */
    xmm0 = XMM_SCALAR(MEMF(esp + 0x18)); /* movss */
    xmm0.f[0] = xmm0.f[0] + MEMF(esi + 0x178); /* addss */
    MEMF(esi + 0x178) = xmm0.f[0]; /* movss */

loc_0011B184: ;
    POP32(esp, esi);
    POP32(esp, ebx);
    esp = ebp;
    POP32(esp, ebp);
    esp += 16; return; /* ret 12 */

    #undef fp_push
    #undef fp_pop
    #undef fp_top
    #undef fp_st
    #undef fp_st1
}

/* LOD-TEST: sub_000F1C90 (LODManager update) turns an object's distance into one of 100 buckets, the bucket gives its LOD level
 * (the table at LODManager+0x148, bucket -> level 0..4). game_lod_scale multiplies the bucket index (hook in recomp_0028.c):
 * a smaller scale reaches each level at a greater distance. The scale is the "Model detail" row (game_lod_set). */
/* Dead soldiers only: the hook at the top of sub_000F8C80 (the kill's fade start) calls game_lod_dead(soldier), the
 * soldier destructor sub_000FC370 calls game_lod_undead. A LOD request object (class vtable 0x316328, created by
 * sub_000F9750 and kept at soldier+0x2584) has its owner soldier at +0x20, so game_lod_scale(request) returns
 * RECOMP_LOD_DEAD_SCALE (off unless set) for a dead soldier's request and the Model detail scale for everything else.
 * Reason: bodies that stay on the floor show stretched spike shapes when their high-detail model is used; the coarse
 * models are fine. */
#define LOD_DEAD_MAX 128
static uint32_t s_lod_dead[LOD_DEAD_MAX];
static int s_lod_dead_n, s_lod_dead_next;

void game_lod_dead(uint32_t obj)
{
    int i;
    for (i = 0; i < s_lod_dead_n; i++)
        if (s_lod_dead[i] == obj)
            return;
    if (s_lod_dead_n < LOD_DEAD_MAX) {
        s_lod_dead[s_lod_dead_n++] = obj;
    } else {
        s_lod_dead[s_lod_dead_next] = obj;
        s_lod_dead_next = (s_lod_dead_next + 1) % LOD_DEAD_MAX;
    }
}

int game_lod_dead_count(void)
{
    return s_lod_dead_n;
}

void game_lod_undead(uint32_t obj)
{
    int i;
    for (i = 0; i < s_lod_dead_n; i++)
        if (s_lod_dead[i] == obj) {
            s_lod_dead[i] = s_lod_dead[--s_lod_dead_n];
            return;
        }
}

/* Model detail (Recomp settings row "Model detail", launcher lod=0..2, env RECOMP_LOD): 0 = the game's own distances (the
 * default), 1 = Higher, 2 = Highest. Its scale multiplies every bucket index: 0.75 reaches each level at 1.33 times the
 * distance, 0.5 at twice. Both values are a first guess: play_log.txt shows the level table to tune them. */
static const float k_lod_scale[3] = { 1.0f, 0.75f, 0.5f };
static const char *const k_lod_name[3] = { "Game", "Higher", "Highest" };
static int s_lod_idx = -1;

static void lod_idx_init(void)
{
    if (s_lod_idx < 0) {
        const char *e = getenv("RECOMP_LOD");
        s_lod_idx = e && *e ? atoi(e) : 0;
        if (s_lod_idx < 0 || s_lod_idx > 2)
            s_lod_idx = 0;
    }
}

int game_lod_get(void)
{
    lod_idx_init();
    return s_lod_idx;
}

const char *game_lod_name(int idx)
{
    return k_lod_name[idx < 0 ? 0 : idx > 2 ? 2 : idx];
}

void game_lod_set(int idx)
{
    lod_idx_init();
    s_lod_idx = idx < 0 ? 0 : idx > 2 ? 2 : idx;
    fprintf(stderr, "[LOD] model detail now %s (bucket scale x%g)\n", k_lod_name[s_lod_idx], (double)k_lod_scale[s_lod_idx]);
}

/* The distance buckets' level table (LODManager+0x148). It is filled when a level loads, so game_lod_scale tries it every
 * 64 calls until it has an entry, and logs it once: play_log.txt then shows the distance where each level starts. */
static int lod_table_text(char *out, size_t n)
{
    uint32_t mgr = MEM32(0x3DB614), b, last = 0xFFFFFFFFu;
    int len, nz = 0;
    if (!mgr || mgr < 0x10000u || mgr >= 0xFD000000u)
        return 0;
    for (b = 0; b < 100; b++)
        if (MEM32(mgr + 0x148 + b * 4))
            nz++;
    if (!nz)
        return 0;
    len = snprintf(out, n, "level table by distance bucket (first bucket of each run):");
    for (b = 0; b < 100 && len < (int)n - 24; b++) {
        uint32_t v = MEM32(mgr + 0x148 + b * 4);
        if (v != last)
            len += snprintf(out + len, n - len, " %u:L%d", (unsigned)b, (int)v);
        last = v;
    }
    return 1;
}

float game_lod_scale(uint32_t req)
{
    static float dead_env = -1.0f;
    static unsigned calls;
    static int table_logged;
    float all, dead;
    int i;
    lod_idx_init();
    all = k_lod_scale[s_lod_idx];
    if (dead_env < 0.0f) {
        const char *d = getenv("RECOMP_LOD_DEAD_SCALE");
        dead_env = d && *d ? (float)atof(d) : 0.0f;
    }
    if (!table_logged && !(++calls & 63)) {
        char text[400];
        if (lod_table_text(text, sizeof text)) {
            table_logged = 1;
            fprintf(stderr, "[LOD] %s; model detail %s\n", text, k_lod_name[s_lod_idx]);
        }
    }
    dead = dead_env > 0.0f ? dead_env : 1.0f;      /* (only an explicit RECOMP_LOD_DEAD_SCALE: the bucket scale jumps straight to the flat billboard level) */
    if (dead != 1.0f && req > 0x10000u && req < 0xFD000000u && s_lod_dead_n) {
        uint32_t owner = MEM32(req + 0x20);
        for (i = 0; i < s_lod_dead_n; i++)
            if (s_lod_dead[i] == owner)
                return dead;
    }
    return all;
}

/* Dead soldiers (while corpses are kept) get a FIXED level of detail instead of the distance's: the highest level
 * shows spike shapes on bodies, the lowest one is a flat billboard picture of a standing soldier (what the global
 * bucket test showed). RECOMP_LOD_DEAD_LEVEL=<n> (default 1) is the level; the bucket table (LODManager+0x148, one
 * level per distance bucket) is logged once as "[LOD] table ..." so the number of levels is known. */
static int s_dead_level = -2;
static void lod_dead_level_init(void)
{
    if (s_dead_level == -2) {
        const char *e = getenv("RECOMP_LOD_DEAD_LEVEL");
        s_dead_level = e && *e ? atoi(e) : 1;
    }
}

/* [ and ] (while the game window is in front, corpses kept): the dead soldiers' level of detail down / up, shown on
 * screen, so the right one can be found by eye (0 = highest detail ... 4 = the flat billboard). */
extern void recomp_osd_show(const char *msg);
static void lod_dead_keys(void)
{
    static int down;
    DWORD pid = 0;
    int up, dn, cur;
    char msg[48];
    if (!game_keep_corpses(8) || !game_dev_keys())
        return;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId())
        return;
    up = (GetAsyncKeyState(VK_OEM_6) & 0x8000) != 0;
    dn = (GetAsyncKeyState(VK_OEM_4) & 0x8000) != 0;
    if ((up || dn) && !down) {
        lod_dead_level_init();
        cur = s_dead_level + (up ? 1 : -1);
        s_dead_level = cur < 0 ? 0 : cur > 4 ? 4 : cur;
        snprintf(msg, sizeof msg, "Dead bodies detail: %d  ( [ ] )", s_dead_level);
        recomp_osd_show(msg);
        fprintf(stderr, "[LOD] dead soldiers level now %d\n", s_dead_level);
    }
    down = up || dn;
}

int game_impact_cap(void)
{
    static int cap = -1;
    if (cap < 0) {
        const char *e = getenv("RECOMP_IMPACT_CAP");
        cap = e && *e ? atoi(e) : 3;       /* the game's own (was 1000: unproven, and more impacts = more sounds) */
        if (cap != 3)
            fprintf(stderr, "[IMPACT] per-object impact effect cap %d (the game's own: 3)\n", cap);
    }
    return cap;
}

/* IMPACT-DBG: why is blood missing on some hits? Counts every call of the impact spawner sub_001497B0 by caller and
 * outcome (1 entered, 2 global flag skip, 3 short-vector skip, 4 per-object cap skip, 5 effect event posted).
 * RECOMP_IMPACT_DBG=1 -> [IMPACTDBG] lines in play_log.txt. */
/* HIT-DBG: soldier hit handler sub_000FB7C0 (spawns Pls_impactBodyBlood01 near its end). 20 entered, 25 damage value
 * (blood needs > 0), 26 fade alpha of the victim (blood needs >= 0.75), 27 blood path reached, 21/22/23/24 = exits
 * (21 early out after the first checks, 22 skipped the effects, 23 no damage / fading, 24 no target). */
void game_hit_dbg(int id, uint32_t bits)
{
    static int on = -1;
    static uint32_t cnt[48], last, nd, nf;
    float v;
    uint32_t now;
    if (on < 0) {
        const char *e = getenv("RECOMP_IMPACT_DBG");
        on = e && *e && *e != '0';
    }
    if (!on || id < 20 || id > 40)
        return;
    cnt[id]++;
    memcpy(&v, &bits, 4);
    if (id == 25 && nd++ < 120)
        fprintf(stderr, "[HITDBG] damage %g\n", (double)v);
    if (id == 26 && nf++ < 120)
        fprintf(stderr, "[HITDBG] victim fade value %g\n", (double)v);
    now = GetTickCount();
    if (now - last >= 5000) {
        last = now;
        fprintf(stderr, "[HITDBG] entered %u damage-checked %u fade-checked %u BLOOD-PATH %u | exits: early %u noeffects %u nodamage-or-fading %u notarget %u\n",
                cnt[20], cnt[25], cnt[26], cnt[27], cnt[21], cnt[22], cnt[23], cnt[24]);
        fprintf(stderr, "[HITDBG] noeffects reasons: no-owner-flag %u owner-flag2000 %u soldier-no-D8 %u D8-state!=2 %u victim-flag13 %u | blood-all bypasses %u\n",
                cnt[30], cnt[31], cnt[32], cnt[33], cnt[34], cnt[35]);
    }
}

/* SHOT-PROBE: RECOMP_SHOT_PROBE=1 logs each shot (the spawn matrix and the owner, sub_000F5180), each soldier hit
 * test (the shot's segment, +0x1C0 to +0x60 of its object, and the victim, sub_000FA8C0) and the test's result
 * (sub_000FB7C0), with the game's step. Off by default; a flag test per call when off. */
static int shot_probe_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_SHOT_PROBE");
        on = e && *e && *e != '0';
    }
    return on;
}

static float shot_f(uint32_t va)
{
    uint32_t w = MEM32(va);
    float f;
    memcpy(&f, &w, 4);
    return f;
}

static unsigned shot_lines;
static FILE *shot_fp;

/* Own file (shot_probe_<date>_<time>.txt), so a .bat log that is overwritten or rotated cannot lose the run. */
static FILE *shot_out(void)
{
    if (!shot_fp) {
        char name[64];
        SYSTEMTIME t;
        GetLocalTime(&t);
        snprintf(name, sizeof name, "shot_probe_%04d%02d%02d_%02d%02d%02d.txt",
                 t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        shot_fp = fopen(name, "w");
        if (!shot_fp)
            shot_fp = stderr;
    }
    return shot_fp;
}

void game_shot_fire(uint32_t m, uint32_t owner)
{
    int k;
    FILE *f;
    if (!shot_probe_on() || shot_lines++ >= 30000)
        return;
    f = shot_out();
    fprintf(f, "[SHOT] fire frame %u step %g owner %08X matrix", (unsigned)g_game_frames, (double)shot_f(0x373268), owner);
    for (k = 0; k < 16; k++)
        fprintf(f, " %g", (double)shot_f(m + 4 * k));
    fprintf(f, "\n");
    fflush(f);
}

/* One line per soldier hit test: the object (its vtable tells the bullet class), the step, the segment. A bullet
 * shows as a chain of lines with the same obj where the next "from" equals the previous "to". */
void game_shot_hit(uint32_t obj, uint32_t victim)
{
    FILE *f;
    if (!shot_probe_on() || shot_lines++ >= 30000)
        return;
    f = shot_out();
    fprintf(f, "[SHOT] hit test t %u frame %u step %g obj %08X vt %08X victim %08X from %g %g %g to %g %g %g\n",
            (unsigned)GetTickCount(), (unsigned)g_game_frames, (double)shot_f(0x373268), obj, (unsigned)MEM32(obj), victim,
            (double)shot_f(obj + 0x1C0), (double)shot_f(obj + 0x1C4), (double)shot_f(obj + 0x1C8),
            (double)shot_f(obj + 0x60), (double)shot_f(obj + 0x64), (double)shot_f(obj + 0x68));
    fflush(f);
}

void game_shot_result(uint32_t r)
{
    FILE *f;
    if (!shot_probe_on() || shot_lines++ >= 30000)
        return;
    f = shot_out();
    fprintf(f, "[SHOT] hit test result %u\n", r);
    fflush(f);
}

/* RELOAD-PROBE (RECOMP_RELOAD_PROBE=1): each look update of the player, compare 0x200 bytes of the player object
 * (+0x1100) and 0x300 bytes of the held weapon object (pointer at player+0x1128) with the last snapshot and log the
 * words that changed (at most 60 lines per word), to reload_probe_<time>.txt. Shows what a reload changes. */
static FILE *rl_fp;
static uint32_t rl_prev_p[0x200 / 4], rl_prev_w[0x300 / 4], rl_wobj;
static unsigned short rl_cnt_p[0x200 / 4], rl_cnt_w[0x300 / 4];
static unsigned rl_cap = 60, rl_line_cap = 40000;   /* RECOMP_RELOAD_PROBE=2: per word 3000 changes, 300000 lines (fire-rate hunt) */
static double rl_ms(void) { LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f); return 1000.0 * (double)q.QuadPart / (double)f.QuadPart; }
static unsigned rl_lines;
static uint32_t rl_base;
/* FIRE-RATE (new in 0.9.1): the game's own cooldown is frame-rate bound (the MP40 shoots every 41 ms from 60 fps up, 67 ms at
 * 30 fps). The shot cadence is therefore held at the trigger: after each bullet of an AUTOMATIC weapon the right trigger
 * reads released for the chosen number of ms (mkb_input.c), then the held button counts as a fresh press. A weapon is
 * automatic when two of its bullets come < 100 ms apart without a new press. */
static const int k_fire_gate[4] = { 0, 67, 90, 120 };    /* ms between an automatic weapon's shots: Fast (the game's own) / Normal (30 fps) / Slow / Slowest */      /* Fast (the game's own) / Normal (30 fps feel) / Slow / Slowest */
static const char *const k_fire_name[4] = { "Fast", "Normal", "Slow", "Slowest" };
static int s_fire_idx = -1;
static void fire_idx_init(void)
{
    if (s_fire_idx < 0) {
        const char *e = getenv("RECOMP_FIRE_CAP");
        s_fire_idx = e && *e ? atoi(e) : 1;
        if (s_fire_idx < 0 || s_fire_idx > 3)
            s_fire_idx = 1;
    }
}
int game_fire_get(void) { fire_idx_init(); return s_fire_idx; }
void game_fire_set(int i) { fire_idx_init(); s_fire_idx = i < 0 ? 0 : i > 3 ? 3 : i; }
const char *game_fire_name(int i) { return k_fire_name[i < 0 ? 0 : i > 3 ? 3 : i]; }

static int fire_cap_skip(uint32_t self, uint32_t ev, uint32_t pc)
{
    (void)self; (void)ev; (void)pc;
    return 0;
}

int game_wevent(uint32_t self, uint32_t ev, uint32_t sp)
{
    extern uint32_t game_pc_get(void);
    uint32_t pc = game_pc_get();
    int near_pc, skip = fire_cap_skip(self, ev, pc);
    if (ev == 49 && pc >= 0x80000000u && self == MEM32(pc + 0x1CC)) {   /* FIRE-CALLERS: who delivers the player's shot event */
        static unsigned nfc;
        if (nfc < 25) {
            uint32_t k, v;
            nfc++;
            fprintf(stderr, "[FIRECALL] t %.3f ret %08X chain:", rl_ms(), MEM32(sp));
            for (k = 4; k < 0x100; k += 4) {
                v = MEM32(sp + k);
                if (v > 0x10000u && v < 0x340000u)
                    fprintf(stderr, " %08X", v);
            }
            fprintf(stderr, "\n");
        }
    }
    if (pc >= 0x80000000u && self == MEM32(pc + 0x1CC) && ev != 65 && ev != 4)   /* the player soldier's events, to tell a reload from a pin pull */
        fprintf(stderr, "[GRENTIMER] t %.2f player event %u (end=%.2f)\n", s_gt, (unsigned)ev, s_gren_end);

    if (!rl_fp)
        return skip;
    near_pc = pc && self + 0x3000u > pc && self < pc + 0x3000u;
    if (near_pc && MEM32(self + 0x1128) >= 0x80000000u)
        rl_base = self;
    if (rl_lines < rl_line_cap) {
        fprintf(rl_fp, "[RLP] t %.3f EVENT %u this %08X (pc%+d) near %d\n", rl_ms(), (unsigned)ev, self, (int)(self - pc), near_pc);
        rl_lines++;
    }
    return skip;
}


void game_reload_poll(uint32_t pc)
{
    static int on = -1;
    uint32_t wobj, i;
    if (on < 0) {
        const char *e = getenv("RECOMP_RELOAD_PROBE");
        on = e && *e && *e != '0';
        if (on && *e == '2') { rl_cap = 3000; rl_line_cap = 300000; }
    }
    if (!on || pc < 0x80000000u || rl_lines > rl_line_cap)
        return;
    if (!rl_fp) {
        char name[64];
        SYSTEMTIME t;
        GetLocalTime(&t);
        snprintf(name, sizeof name, "reload_probe_%04d%02d%02d_%02d%02d%02d.txt", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        rl_fp = fopen(name, "w");
        if (!rl_fp)
            return;
        fprintf(rl_fp, "[RLP] start player %08X\n", pc);
    }
    for (i = 0; i < 0x200 / 4; i++) {
        uint32_t v = MEM32(pc + 0x1100 + 4 * i);
        if (v != rl_prev_p[i]) {
            rl_prev_p[i] = v;
            if (rl_cnt_p[i] < rl_cap) {
                float f; memcpy(&f, &v, 4);
                rl_cnt_p[i]++;
                fprintf(rl_fp, "[RLP] t %.3f step %g player+%X = %08X (%g)\n", rl_ms(), (double)shot_f(0x373268), 0x1100 + 4 * i, v, (double)f);
                rl_lines++;
            }
        }
    }
    if (!rl_base) {
        uint32_t cand[3], k;
        cand[0] = MEM32(pc + 0x1CC); cand[1] = MEM32(pc + 0x1FC); cand[2] = pc;
        for (k = 0; k < 3; k++)
            if (cand[k] >= 0x80000000u && MEM32(cand[k] + 0x1128) >= 0x80000000u) {
                rl_base = cand[k];
                fprintf(rl_fp, "[RLP] soldier base candidate %u = %08X (weapon %08X)\n", k, rl_base, MEM32(rl_base + 0x1128));
                break;
            }
    }
    if (!rl_base) {
        static unsigned nextlog;
        if ((int)(GetTickCount() - nextlog) > 0) {
            nextlog = GetTickCount() + 3000;
            fprintf(rl_fp, "[RLP] no weapon holder yet: pc+1CC=%08X pc+1FC=%08X pc+1128=%08X\n", MEM32(pc + 0x1CC), MEM32(pc + 0x1FC), MEM32(pc + 0x1128));
        }
    }
    if (rl_base)
        pc = rl_base;
    wobj = MEM32(pc + 0x1128);
    if (wobj != rl_wobj) {
        rl_wobj = wobj;
        fprintf(rl_fp, "[RLP] t %u held weapon object now %08X\n", (unsigned)GetTickCount(), wobj);
        memset(rl_cnt_w, 0, sizeof rl_cnt_w);
        if (wobj >= 0x80000000u)
            for (i = 0; i < 0x300 / 4; i++)
                rl_prev_w[i] = MEM32(wobj + 4 * i);
        rl_lines++;
    }
    if (wobj >= 0x80000000u) {
        for (i = 0; i < 0x300 / 4; i++) {
            uint32_t v = MEM32(wobj + 4 * i);
            if (v != rl_prev_w[i]) {
                rl_prev_w[i] = v;
                if (rl_cnt_w[i] < rl_cap) {
                    float f; memcpy(&f, &v, 4);
                    rl_cnt_w[i]++;
                    fprintf(rl_fp, "[RLP] t %.3f step %g weapon+%X = %08X (%g)\n", rl_ms(), (double)shot_f(0x373268), 4 * i, v, (double)f);
                    rl_lines++;
                }
            }
        }
    }
    fflush(rl_fp);
}

/* PROJ-PROBE: the projectile class (vtable 0x323930). sub_00183C90 builds one (def pointer, speed, position, direction,
 * owner), sub_00183610 is its per-update call (arg = step). Logs creation and up to 120 updates of each of the last
 * 24 objects created, into the same shot_probe file. */
static uint32_t game_pc_val;
uint32_t game_pc_get(void) { return game_pc_val; }
void game_pc_note(uint32_t pc) { extern void game_reload_poll(uint32_t); game_pc_val = pc; game_reload_poll(pc); }
static uint32_t proj_obj[24];
static unsigned proj_cnt[24];
static unsigned proj_made;

/* AIM-FIX (Karabiner and its class): from 60 fps up the player's fast bullet leaves along the camera pitch, but at
 * 30 fps it leaves along (camera pitch - the pending kick at player+0x14C), which is what the crosshair really
 * shows; at 144 the bullet rides about 2 degrees high and passes over targets at range. Measured in
 * shot_probe logs (2026-11-04). The direction is set to the 30 fps value at every frame rate (idempotent).
 * Only the player's bullets with muzzle speed above 6.5 (Karabiner 8). RECOMP_AIM_FIX=0 turns it off. */
extern float game_view_fov_deg(void);               /* recomp_manual.c: the game's own world fov, degrees (70 = normal view, ~20 = scope) */
int game_fire_get(void);
static int aim_mode(void)
{
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("RECOMP_AIM_MODE");
        m = e && *e ? atoi(e) : 3;
    }
    return m;
}

static float s_dyaw;
static void aimfix_log(int why, float spd, float ang, float np, float pitch, uint32_t pc)
{
    static unsigned lines;
    if (lines >= 400)
        return;
    lines++;
    fprintf(stderr, "[AIMFIX2] yaw110 %g diryaw %g ", (double)shot_f(pc + 0x110), (double)s_dyaw);
    fprintf(stderr, "[AIMFIX2] why %d speed %g fov %.1f dirpitch %g -> %g pitch108 %g f13C %g f140 %g f144 %g f148 %g f14C %g step %g\n",
            why, (double)spd, (double)game_view_fov_deg(), (double)ang, (double)np, (double)pitch, (double)shot_f(pc + 0x13C),
            (double)shot_f(pc + 0x140), (double)shot_f(pc + 0x144), (double)shot_f(pc + 0x148), (double)shot_f(pc + 0x14C),
            (double)shot_f(0x373268));
}

extern volatile unsigned g_mkb_rt_presses;
extern volatile double g_mkb_gate_until_ms;
static void fire_gate_note(uint32_t weapon, float spd)
{
    static uint32_t autow[8];
    static unsigned nauto, last_press;
    static double last_ms;
    double now = rl_ms();
    int i, is_auto = 0, ms;
    if (spd >= 6.5f)
        return;
    if (!weapon)
        weapon = 1;
    for (i = 0; i < (int)nauto && i < 8; i++)
        if (autow[i] == weapon)
            is_auto = 1;
    if (!is_auto && now - last_ms < 100.0 && g_mkb_rt_presses == last_press && nauto < 8) {
        autow[nauto++] = weapon;
        is_auto = 1;
        fprintf(stderr, "[FIREGATE] weapon %08X is automatic\n", weapon);
    }
    last_ms = now;
    last_press = g_mkb_rt_presses;
    ms = k_fire_gate[game_fire_get()];
    if (is_auto && ms > 0)
        g_mkb_gate_until_ms = now + (double)ms;
}

void game_proj_fix(uint32_t sp)
{
    static int on = -1;
    static unsigned n;
    uint32_t owner, dir;
    float spd, nx, ny, nz, h, pitch, kick, np, ang;
    uint32_t w;
    if (on < 0) {
        const char *e = getenv("RECOMP_AIM_FIX");
        on = !(e && *e == '0');
    }
    if (!on || game_pc_val < 0x80000000u)
        return;
    owner = MEM32(sp + 0x24);
    if (owner < game_pc_val || owner >= game_pc_val + 0x100)
        return;
    w = MEM32(sp + 0x18);
    memcpy(&spd, &w, 4);
    {   uint32_t sol = MEM32(game_pc_val + 0x1CC);
        fire_gate_note(sol >= 0x80000000u ? MEM32(sol + 0x1128) : 0, spd);
    }
    {   /* BULLET: when each of the player's projectiles is made (fire cadence), ms on a high-resolution clock */
        static unsigned nb;
        if (nb < 800) {
            uint32_t pp = MEM32(sp + 0x10);
            nb++;
            fprintf(stderr, "[BULLET] t %.3f speed %g pos %g %g %g step %g fire %d\n", rl_ms(), (double)spd,
                    pp >= 0x1000u ? (double)shot_f(pp) : 0.0, pp >= 0x1000u ? (double)shot_f(pp + 4) : 0.0,
                    pp >= 0x1000u ? (double)shot_f(pp + 8) : 0.0, (double)shot_f(0x373268), game_fire_get());
        }
    }
    if (!(spd > 6.5f && spd < 50.0f))
        return;
    dir = MEM32(sp + 0x14);
    if (dir < 0x1000u) { aimfix_log(1, spd, 0, 0, 0, game_pc_val); return; }
    nx = shot_f(dir); ny = shot_f(dir + 4); nz = shot_f(dir + 8);
    h = sqrtf(nx * nx + ny * ny);
    s_dyaw = atan2f(ny, nx);
    ang = asinf(nz);
    pitch = shot_f(game_pc_val + 0x108);
    kick = shot_f(game_pc_val + 0x14C);
    if (h < 0.3f) { aimfix_log(2, spd, ang, ang, pitch, game_pc_val); return; }
    if (!(kick >= 0.0f && kick < 0.5f) || !(pitch > -1.4f && pitch < 1.4f)) { aimfix_log(3, spd, ang, ang, pitch, game_pc_val); return; }
    {   /* mode 3 (default): in a scope the game's own direction carries a random sideways / up-down error four times the hip
         * one (yaw 0.02 rad against 0.005, measured), so there the bullet leaves exactly along the camera, lowered by the kick */
        extern int game_scope_up(void);
        int m = aim_mode();
        if (m == 2 || (m == 3 && game_scope_up())) {
            float yaw = shot_f(game_pc_val + 0x110) + 1.5707963f, c0 = cosf(pitch - kick);
            np = pitch - kick;
            nx = c0 * cosf(yaw);
            ny = c0 * sinf(yaw);
            h = c0;
        } else
            np = ang - kick;
    }
    if (0)
        np = ang - kick;            /* only the pending kick is added to the game's own direction; its own offset (larger in a scope) stays */
    /* sanity: only fix when the stored direction is near the camera pitch (within 0.25 rad) */
    if (fabsf(ang - pitch) > 0.25f) { aimfix_log(4, spd, ang, np, pitch, game_pc_val); return; }
    {
        float c = cosf(np), s = sinf(np), fx = nx / h, fy = ny / h;
        float ox = c * fx, oy = c * fy, oz = s;
        memcpy(&w, &ox, 4); MEM32(dir) = w;
        memcpy(&w, &oy, 4); MEM32(dir + 4) = w;
        memcpy(&w, &oz, 4); MEM32(dir + 8) = w;
    }
    aimfix_log(0, spd, ang, np, pitch, game_pc_val);
    if (n++ < 20)
        fprintf(stderr, "[AIMFIX] bullet pitch %g -> %g (camera %g, kick %g)\n", (double)ang, (double)np, (double)pitch, (double)kick);
}

/* RECOMP_GRENADE_PROBE=1: log only the player's projectiles (owner inside the player object), with up to 1500 updates each */
static int grenade_probe_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GRENADE_PROBE");
        on = e && *e && *e != '0';
    }
    return on;
}

/* GRENADE-PROBE: every call of the projectile spawner sub_00182050 (the player's gun bullets, casings, and whatever else
 * goes through it): return address and the ten stack words; for words that look like a definition pointer the fields at
 * +08..+14 as floats. */
void game_spawn_log(uint32_t sp)
{
    FILE *f;
    unsigned k;
    static unsigned n;
    grenade_cd_spawn(sp);
    if (!grenade_probe_on() || n >= 3000)
        return;
    n++;
    f = shot_out();
    fprintf(f, "[SPAWN] t %u ret %08X args", (unsigned)GetTickCount(), MEM32(sp));
    for (k = 0; k < 10; k++) {
        uint32_t v = MEM32(sp + 4 + 4 * k);
        fprintf(f, " %08X", v);
    }
    for (k = 0; k < 10; k++) {
        uint32_t v = MEM32(sp + 4 + 4 * k);
        if (v >= 0x82000000u && v < 0x83000000u)
            fprintf(f, " | arg%u def +08 %g +0C %g +10 %g +14 %g", k, (double)shot_f(v + 8), (double)shot_f(v + 12), (double)shot_f(v + 16), (double)shot_f(v + 20));
    }
    fprintf(f, "\n");
    fflush(f);
}

void game_proj_new(uint32_t self, uint32_t sp)
{
    FILE *f;
    uint32_t def, pos, dir, owner;
    unsigned slot;
    int gp = grenade_probe_on();
    if (!(shot_probe_on() || gp) || proj_made >= 4000)
        return;
    owner = MEM32(sp + 0x24);
    if (gp) {
        float life = shot_f(MEM32(sp + 0xC) + 20);
        int mine = game_pc_val >= 0x80000000u && owner >= game_pc_val && owner < game_pc_val + 0x100;
        if (!mine && !(life > 280.0f && life < 400.0f))   /* a 5 s fuse is about 299.7 frames */
            return;
    }
    f = shot_out();
    def = MEM32(sp + 0xC);
    pos = MEM32(sp + 0x10);
    dir = MEM32(sp + 0x14);
    slot = proj_made++ % 24;
    proj_obj[slot] = self;
    proj_cnt[slot] = 0;
    fprintf(f, "[PROJ] new #%u t %u frame %u step %g obj %08X owner %08X type %u def %08X def: +04 %08X +08 %g +0C %g +10 %g +14 %g speedarg %g pos %g %g %g dir %g %g %g\n",
            slot, (unsigned)GetTickCount(), (unsigned)g_game_frames, (double)shot_f(0x373268), self, owner,
            (unsigned)MEM32(sp + 8), def, (unsigned)MEM32(def + 4), (double)shot_f(def + 8), (double)shot_f(def + 12),
            (double)shot_f(def + 16), (double)shot_f(def + 20), (double)shot_f(sp + 0x18),
            (double)shot_f(pos), (double)shot_f(pos + 4), (double)shot_f(pos + 8),
            (double)shot_f(dir), (double)shot_f(dir + 4), (double)shot_f(dir + 8));
    if (game_pc_val >= 0x80000000u)
        fprintf(f, "[PROJ] player #%u pc %08X pitch108 %g yaw110 %g f13C %g f140 %g f144 %g f14C %g f1C8 %g dirpitch %g\n", slot, game_pc_val,
                (double)shot_f(game_pc_val + 0x108), (double)shot_f(game_pc_val + 0x110), (double)shot_f(game_pc_val + 0x13C),
                (double)shot_f(game_pc_val + 0x140), (double)shot_f(game_pc_val + 0x144), (double)shot_f(game_pc_val + 0x14C),
                (double)shot_f(game_pc_val + 0x1C8), asin((double)shot_f(dir + 8)));
    fflush(f);
}

void game_proj_tick(uint32_t self, uint32_t dtbits)
{
    unsigned i;
    float dt;
    FILE *f;
    if (!(shot_probe_on() || grenade_probe_on()))
        return;
    for (i = 0; i < 24 && i < proj_made; i++)
        if (proj_obj[i] == self)
            break;
    if (i == 24 || i >= proj_made || proj_cnt[i] >= (grenade_probe_on() ? 1500u : 120u))
        return;
    proj_cnt[i]++;
    memcpy(&dt, &dtbits, 4);
    f = shot_out();
    fprintf(f, "[PROJ] tick #%u n %u t %u dt %g life %g pos %g %g %g vel %g %g %g\n", i, proj_cnt[i],
            (unsigned)GetTickCount(), (double)dt, (double)shot_f(self + 0xE8),
            (double)shot_f(self + 0x60), (double)shot_f(self + 0x64), (double)shot_f(self + 0x68),
            (double)shot_f(self + 0x40), (double)shot_f(self + 0x44), (double)shot_f(self + 0x48));
    fflush(f);
}

/* BLOOD-ALL: sub_000FB7C0 (hit effects of a soldier) rejects many bullet hits before the blood block (gates 30 / 31 / 34,
 * ~90% of the ray hits in play logs). With Blood amount above Normal those hits jump to the blood-only part (damage > 0
 * test done here, fade check and spawn follow in the game's code); the hit-reaction sounds stay skipped. Gate 33 (the
 * player's own soldier) is never bypassed. RECOMP_BLOOD_ALL=0 turns it off. */
extern int game_blood_get(void);
int game_blood_all(float dmg)
{
    static int on = -1;
    static unsigned n;
    if (on < 0) {
        const char *e = getenv("RECOMP_BLOOD_ALL");
        on = !(e && *e == '0');
    }
    if (!on || game_blood_get() <= 0 || !(dmg > 0.0f))
        return 0;
    game_hit_dbg(35, 0);
    if (n++ < 5)
        fprintf(stderr, "[BLOOD] bypassed an effects gate (damage %g)\n", (double)dmg);
    return 1;
}

/* SCREEN-FX: called from the soldier hit handler (sub_000FB7C0) when its blood block runs, with the world position of
 * the hit. A hit within RECOMP_SCREENFX_DIST (default 3.0 world units, metres) of the camera (player view, object at
 * [0x3E8D08], position at +0x90) splatters blood on the screen, the closer the stronger. Melee kills are always that
 * close. The first 40 hits are logged with their distance so the range can be tuned. */
extern void recomp_screenfx_trigger(int kind, float strength);
void game_screen_blood(float hx, float hy, float hz)
{
    static int inited;
    static float range = 3.0f;
    static int logged;
    uint32_t cam = MEM32(0x3E8D08);
    float cx, cy, cz, dx, dy, dz, d;
    if (!inited) {
        const char *e = getenv("RECOMP_SCREENFX_DIST");
        inited = 1;
        if (e && atof(e) > 0.0)
            range = (float)atof(e);
    }
    if (!((cam >= 0x10000u && cam < 0x00800000u) || (cam >= 0x80010000u && cam < 0x83FF0000u)))
        return;
    cx = MEMF(cam + 0x90);
    cy = MEMF(cam + 0x94);
    cz = MEMF(cam + 0x98);
    dx = hx - cx;
    dy = hy - cy;
    dz = hz - cz;
    d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (logged < 40) {
        logged++;
        fprintf(stderr, "[SCREENFX] hit distance %.2f (camera %.1f %.1f %.1f, hit %.1f %.1f %.1f, range %.1f)\n",
                (double)d, (double)cx, (double)cy, (double)cz, (double)hx, (double)hy, (double)hz, (double)range);
    }
    if (!(d < range))
        return;
    recomp_screenfx_trigger(0, 1.0f - d / range);
}

/* BLOOD-EVERY: a soldier hit picks its effect at random in sub_000FB7C0 (sub_000E70F0 roll 0..99 against 0x28: 40% blood
 * [soldier+0x14AC], else one of three dust puffs [+0x14A0/A4/A8]; confirmed by play logs: 25 blood vs 45 dust). With Blood
 * amount above Normal every hit that reaches the effect picks blood. RECOMP_BLOOD_EVERY=0 keeps the game's roll. */
int game_blood_every(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_BLOOD_EVERY");
        on = !(e && *e == '0');
    }
    return on && game_blood_get() > 0;
}

uint32_t g_dbg_last_mat = 0xFFFFFFFFu;

/* sub_00072E90 (surface hit handler): material id of the hit, effect object found for it (sub_00070810, 0 = no effect
 * will be posted), result of the sub_000707F0 predicate. Histogram per material every 5 s: [MATDBG]. */
void game_impact_mat(uint32_t mat, uint32_t fx, uint32_t pass)
{
    static int on = -1;
    static uint32_t m[48], n[48], nofx[48], nopass[48];
    static int nm;
    static uint32_t last;
    int i;
    uint32_t now;
    if (on < 0) {
        const char *e = getenv("RECOMP_IMPACT_DBG");
        on = e && *e && *e != '0';
    }
    if (!on)
        return;
    g_dbg_last_mat = mat;
    for (i = 0; i < nm; i++)
        if (m[i] == mat)
            break;
    if (i == nm) {
        if (nm >= 48)
            return;
        m[nm++] = mat;
    }
    n[i]++;
    if (fx == 0)
        nofx[i]++;
    if (pass == 0)
        nopass[i]++;
    now = GetTickCount();
    if (now - last >= 5000) {
        last = now;
        for (i = 0; i < nm; i++)
            fprintf(stderr, "[MATDBG] material %u: hits %u, no effect object %u, predicate-0 %u\n", m[i], n[i], nofx[i], nopass[i]);
    }
}

void game_impact_dbg(int stage, uint32_t a, uint32_t b, uint32_t c)
{
    static int on = -1;
    static uint32_t cnt[6][8], callers[8], ncallers;
    static uint32_t last, detail;
    uint32_t i, now;
    if (on < 0) {
        const char *e = getenv("RECOMP_IMPACT_DBG");
        on = e && *e && *e != '0';
    }
    if (!on)
        return;
    for (i = 0; i < ncallers; i++)
        if (callers[i] == a)
            break;
    if (i == ncallers) {
        if (ncallers >= 8)
            i = 7;
        else
            callers[ncallers++] = a;
    }
    cnt[stage][i]++;
    if (detail < 300 && stage >= 3) {
        detail++;
        fprintf(stderr, "[IMPACTDBG] stage %d caller %08X a=%08X b=%08X\n", stage, a, b, c);
    }
    now = GetTickCount();
    if (now - last >= 5000) {
        last = now;
        for (i = 0; i < ncallers; i++)
            fprintf(stderr, "[IMPACTDBG] caller %08X: entered %u flagskip %u shortskip %u capskip %u posted %u\n",
                    callers[i], cnt[1][i], cnt[2][i], cnt[3][i], cnt[4][i], cnt[5][i]);
    }
}

uint32_t game_lod_level(uint32_t req, uint32_t level)
{
    static int logged;
    int i;
    int fixed;
    if (!s_lod_dead_n || !game_keep_corpses(8) || req < 0x10000u || req >= 0xFD000000u)
        return level;
    lod_dead_level_init();
    fixed = s_dead_level;
    {
        uint32_t owner = MEM32(req + 0x20);
        for (i = 0; i < s_lod_dead_n; i++)
            if (s_lod_dead[i] == owner)
                break;
        if (i == s_lod_dead_n)
            return level;
    }
    if (!logged) {
        uint32_t mgr = MEM32(0x3DB614), b, last = 0xFFFFFFFFu;
        char line[400];
        int len = 0;
        logged = 1;
        len += snprintf(line + len, sizeof line - len, "[LOD] level table by distance bucket (first bucket of each run):");
        for (b = 0; mgr && b < 100 && len < (int)sizeof line - 24; b++) {
            uint32_t v = MEM32(mgr + 0x148 + b * 4);
            if (v != last)
                len += snprintf(line + len, sizeof line - len, " %u:L%d", (unsigned)b, (int)v);
            last = v;
        }
        fprintf(stderr, "%s; dead soldiers use level %d\n", line, fixed);
    }
    return (uint32_t)fixed;
}

/* The AI tuning and aim code is in its own files but compiled as part of this one: the profile-guided build only
 * links the object files that existed when the profile was recorded. */
#include "ai_tuning.c"
#include "ai_aim.c"
