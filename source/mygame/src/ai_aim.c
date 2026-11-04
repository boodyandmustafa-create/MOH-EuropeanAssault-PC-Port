/*
 * ai_aim.c -- enemy aim measurement and live knob test (AI-AIM). Everything is off unless RECOMP_AI_LOG=1.
 *
 *  game_aim_measure(sp)  is called from the projectile init hook (recomp_0044.c, marked AI-AIM) with the stack pointer
 *      of sub_00183C90: owner = [sp+0x24], definition = [sp+0xC], position pointer = [sp+0x10], direction pointer =
 *      [sp+0x14], speed = [sp+0x18].
 *      The first time the player fires, the player object (game_pc_get()) is scanned for a float triple close to the
 *      bullet's spawn point: that is the player position offset. After that, every bullet that is not the player's
 *      gets one line in ai_aim.txt: time, owner, owner vtable, definition, speed, distance to the player, the angle
 *      between the bullet direction and the line to the player, the time since that owner's previous bullet, and the
 *      state of the two test switches.
 *  game_aim_poll()       is called from game_ai_probe (every frame while the player brain runs). With RECOMP_DEV_KEYS=1:
 *      F8 multiplies the tuning floats 0x376234..0x37623C and 0x376248..0x376250 by 10, F9 multiplies 0x376170 and
 *      0x376180 (and 0x376198 is left alone). The values are re-applied once a second and restored on the next press.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>

extern ptrdiff_t g_xbox_mem_offset;
extern uint32_t game_pc_get(void);
extern int game_dev_keys(void);

static int s_on = -1;
static int s_pos_off = -1;                 /* offset of the player position inside the player object, -1 = not found yet */
static int s_sw[2];                        /* F8 / F9 states */
static FILE *s_f;

typedef struct { uint32_t owner, last_t; } AimOwner;
static AimOwner s_own[128];

static float gf(uint32_t va)
{
    float v;
    uint32_t w = *(volatile uint32_t *)((uintptr_t)va + g_xbox_mem_offset);
    memcpy(&v, &w, 4);
    return v;
}

static uint32_t gw(uint32_t va)
{
    return *(volatile uint32_t *)((uintptr_t)va + g_xbox_mem_offset);
}

static int guest_ok(uint32_t va)
{
    return (va >= 0x10000u && va < 0x04000000u) || (va >= 0x80000000u && va < 0x84000000u);   /* low range includes the stack */
}

static int on(void)
{
    if (s_on < 0) {
        const char *e = getenv("RECOMP_AI_LOG");
        s_on = e && e[0] == '1';
    }
    return s_on;
}

static uint32_t owner_dt(uint32_t owner, uint32_t now)
{
    int i, free_i = -1;
    uint32_t dt = 0;
    for (i = 0; i < 128; i++) {
        if (s_own[i].owner == owner) {
            dt = now - s_own[i].last_t;
            s_own[i].last_t = now;
            return dt;
        }
        if (!s_own[i].owner && free_i < 0)
            free_i = i;
    }
    if (free_i >= 0) {
        s_own[free_i].owner = owner;
        s_own[free_i].last_t = now;
    }
    return 0;
}

void game_aim_measure(uint32_t sp)
{
    uint32_t owner, def, pos, dir, pc, now;
    float p[3], d[3], spd;
    uint32_t w;

    if (!on())
        return;
    owner = gw(sp + 0x24);
    def = gw(sp + 0xC);
    pos = gw(sp + 0x10);
    dir = gw(sp + 0x14);
    w = gw(sp + 0x18);
    memcpy(&spd, &w, 4);
    pc = game_pc_get();
    if (!guest_ok(pos) || !guest_ok(dir) || pc < 0x80000000u)
        return;
    p[0] = gf(pos); p[1] = gf(pos + 4); p[2] = gf(pos + 8);
    d[0] = gf(dir); d[1] = gf(dir + 4); d[2] = gf(dir + 8);
    now = GetTickCount();
    if (!s_f)
        s_f = fopen("ai_aim.txt", "w");
    if (!s_f)
        return;
    if (owner >= pc && owner < pc + 0x100u) {
        if (s_pos_off < 0) {
            uint32_t o;
            for (o = 0; o < 0x400; o += 4) {
                float x = gf(pc + o), y = gf(pc + o + 4), z = gf(pc + o + 8);
                if (fabsf(x - p[0]) < 2.0f && fabsf(y - p[1]) < 2.0f && fabsf(z - p[2]) < 2.5f && fabsf(x) > 0.5f) {
                    s_pos_off = (int)o;
                    fprintf(s_f, "# player position found at pc+0x%X (%g %g %g), bullet spawn %g %g %g\n", o, (double)x, (double)y, (double)z,
                            (double)p[0], (double)p[1], (double)p[2]);
                    fflush(s_f);
                    break;
                }
            }
            if (s_pos_off < 0) {
                fprintf(s_f, "# player shot, position offset not found (spawn %g %g %g)\n", (double)p[0], (double)p[1], (double)p[2]);
                fflush(s_f);
            }
        }
        return;
    }
    if (s_pos_off < 0 || spd < 1.0f || spd > 20.0f)
        return;
    {
        float t[3], dist, dn, dot, ang;
        t[0] = gf(pc + (uint32_t)s_pos_off) - p[0];
        t[1] = gf(pc + (uint32_t)s_pos_off + 4) - p[1];
        t[2] = gf(pc + (uint32_t)s_pos_off + 8) - p[2];
        dist = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
        dn = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dist < 3.0f || dn < 0.01f)
            return;
        dot = (t[0] * d[0] + t[1] * d[1] + t[2] * d[2]) / (dist * dn);
        if (dot > 1.0f) dot = 1.0f;
        if (dot < -1.0f) dot = -1.0f;
        ang = acosf(dot) * 57.29578f;
        fprintf(s_f, "t %u owner %08X vt %08X def %08X speed %g dist %.1f angle %.2f dt %u sw %02X\n", now, owner, guest_ok(owner) ? gw(owner) : 0u, def,
                (double)spd, (double)dist, (double)ang, owner_dt(owner, now), s_sw[0]);
        fflush(s_f);
    }
}

static const uint32_t k_set0[] = { 0x376234, 0x376238, 0x37623C, 0x376248, 0x37624C, 0x376250 };
static float s_orig0[6];
static int s_have0, s_ind[6];              /* Numpad1..6 switch the six floats of k_set0 one by one (x10) */
static DWORD s_last_apply;

/* sub_00115B90 aim smoothing (recomp_0032.c, marked AI-SMOOTH). game_smooth_gate(obj, factor) is asked once per call at
 * the start of the smoothing block: returns 1 = run it, 0 = skip it (state untouched, the last output in [esp+0xC] is
 * replayed through game_smooth_cache/game_smooth_get). With the toggle ON the block runs on a fixed 1/30 s step. */
static int s_smooth_fixed;
static unsigned s_frames;
typedef struct { uint32_t obj; double acc; LARGE_INTEGER last; float out; int have; unsigned n; } SmoothObj;
static SmoothObj s_so[8];
static unsigned s_so_calls;
static DWORD s_so_t0;
static unsigned s_so_f0;

static SmoothObj *smooth_obj(uint32_t obj)
{
    int i;
    for (i = 0; i < 8; i++)
        if (s_so[i].obj == obj)
            return &s_so[i];
    for (i = 0; i < 8; i++)
        if (!s_so[i].obj) {
            s_so[i].obj = obj;
            return &s_so[i];
        }
    return &s_so[7];
}

int game_smooth_gate(uint32_t obj, float factor)
{
    SmoothObj *o;
    LARGE_INTEGER now, fq;
    DWORD t;
    int run = 1;
    if (!on())
        return 1;
    o = smooth_obj(obj);
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&fq);
    s_so_calls++;
    o->n++;
    if (o->last.QuadPart) {
        double dt = (double)(now.QuadPart - o->last.QuadPart) / (double)fq.QuadPart;
        if (dt > 0.25) dt = 0.25;
        o->acc += dt;
    }
    o->last = now;
    if (s_smooth_fixed && o->have) {
        if (o->acc >= 1.0 / 30.0)
            o->acc -= 1.0 / 30.0;
        else
            run = 0;
    } else if (o->acc >= 1.0 / 30.0)
        o->acc = 0;
    t = GetTickCount();
    if (!s_so_t0)
        s_so_t0 = t;
    if (t - s_so_t0 >= 1000u) {
        int i;
        uint32_t pc = game_pc_get();
        fprintf(stderr, "[SMOOTH] 1s: frames %u calls %u fixed %d factor %g |", s_frames - s_so_f0, s_so_calls, s_smooth_fixed, (double)factor);
        for (i = 0; i < 8; i++)
            if (s_so[i].obj) {
                fprintf(stderr, " obj %08X vt %08X%s n=%u;", s_so[i].obj, guest_ok(s_so[i].obj) ? gw(s_so[i].obj) : 0u, s_so[i].obj == pc ? "(PLAYER)" : "", s_so[i].n);
                s_so[i].n = 0;
            }
        fprintf(stderr, "\n");
        s_so_t0 = t;
        s_so_f0 = s_frames;
        s_so_calls = 0;
    }
    return run;
}

void game_smooth_cache(uint32_t obj, float out)
{
    SmoothObj *o = smooth_obj(obj);
    o->out = out;
    o->have = 1;
}

float game_smooth_get(uint32_t obj)
{
    return smooth_obj(obj)->out;
}

static void apply_ind(void)
{
    int i;
    if (!s_have0) {
        for (i = 0; i < 6; i++)
            s_orig0[i] = gf(k_set0[i]);
        s_have0 = 1;
    }
    for (i = 0; i < 6; i++) {
        float v = s_ind[i] ? s_orig0[i] * 10.0f : s_orig0[i];
        memcpy((void *)((uintptr_t)k_set0[i] + g_xbox_mem_offset), &v, 4);
    }
}

void game_aim_poll(void)
{
    static int kk[6];
    DWORD now;
    int i, any = 0;
    static int kins;
    if (!on() || !game_dev_keys())
        return;
    s_frames++;
    now = GetTickCount();
    if (GetAsyncKeyState(VK_INSERT) & 0x8000) {
        if (!kins) {
            s_smooth_fixed = !s_smooth_fixed;
            fprintf(stderr, "[SMOOTH] fixed 1/30 s step now %s\n", s_smooth_fixed ? "ON" : "off");
        }
        kins = 1;
    } else
        kins = 0;
    for (i = 0; i < 6; i++) {
        static const int k_vk[6] = { 0, VK_DELETE, VK_HOME, VK_END, VK_PRIOR, VK_NEXT };   /* Insert belongs to the smoothing toggle */
        if (GetAsyncKeyState(k_vk[i]) & 0x8000) {
            if (!kk[i]) {
                s_ind[i] = !s_ind[i];
                fprintf(stderr, "[AIM] Numpad%d (0x%X x10) now %s\n", i + 1, k_set0[i], s_ind[i] ? "ON" : "off");
                s_last_apply = 0;
            }
            kk[i] = 1;
        } else
            kk[i] = 0;
        any |= s_ind[i];
        s_sw[0] = s_sw[0] & ~(1 << i);
        s_sw[0] |= s_ind[i] << i;
    }
    if (now - s_last_apply < 1000u)
        return;
    s_last_apply = now;
    if (any || s_have0)
        apply_ind();
}
