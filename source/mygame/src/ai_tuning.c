/*
 * ai_tuning.c -- the game's tuning-constants block (AI-TUNING).
 *
 * sub_001265F0 reads ~117 named floats (CRC32 names, no readable strings in the data) from one definition
 * object and stores them in the static block 0x376148..0x3766BF. Soldier and AI code reads them later
 * (difficulty tables such as 0x3764B0.. are 4 floats, entry 0 = Green ... entry 3 = Hero).
 * The loader calls game_ai_tuning_loaded() once, right before it returns (hook line in recomp_0033.c, marked
 * AI-TUNING; re-add it if the gen files are regenerated).
 *
 *   RECOMP_AI_LOG=1  -> ai_tuning_dump.txt: difficulty index + every dword of the block (float and hex), one section per load.
 *   mods\ai\tuning.txt (RECOMP_MODS / RECOMP_MODS_DIR, same pack rule as the other mods):
 *       3764B0 = 1.5        set the float at that address (hex, with or without 0x)
 *       3764B0 *= 2         multiply the float
 *       376520 = i:3        set a 32-bit integer
 *   Lines apply in order, later packs override earlier ones. Applied after the game's own load, so it wins.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>

extern ptrdiff_t g_xbox_mem_offset;

#define AIT_FIRST 0x376148u
#define AIT_LAST  0x3766C0u            /* exclusive */

static int s_ait_loads;

static uint32_t *ait_word(uint32_t va)
{
    return (uint32_t *)((uintptr_t)va + g_xbox_mem_offset);
}

static void ait_dump(void)
{
    FILE *f;
    uint32_t va, diff = 0xFFFFFFFFu;
    uint32_t base = *(volatile uint32_t *)((uintptr_t)0x3888C0u + g_xbox_mem_offset);

    if (base >= 0x100000u && base < 0x8000000u)
        diff = *(volatile uint32_t *)((uintptr_t)(base + 0xE70u) + g_xbox_mem_offset);
    static int opened;
    f = fopen("ai_tuning_dump.txt", opened ? "a" : "w");
    opened = 1;
    if (!f)
        return;
    fprintf(f, "# load %d  difficulty index [[0x3888C0]+0xE70] = %d (0 Green, 1 Veteran, 2 Elite, 3 Hero, -1 unset)\n",
            s_ait_loads, (int)diff);
    for (va = AIT_FIRST; va < AIT_LAST; va += 4) {
        uint32_t w = *ait_word(va);
        float fl;
        memcpy(&fl, &w, 4);
        fprintf(f, "%06X  %08X  %g  int %d\n", va, w, (double)fl, (int)w);
    }
    fclose(f);
}

/* friendly names: MaxHealth.<level>, Revives.<level> (level = Recruit/Green, Veteran, Elite, Hero or All), EnemyDamage, HitZoneMult0..3 */
typedef struct { const char *name; uint32_t va, count, isint; } AitAlias;
static const AitAlias s_alias[] = {
    { "MaxHealth.Green",   0x3764B0, 1, 0 }, { "MaxHealth.Veteran", 0x3764B4, 1, 0 },
    { "MaxHealth.Elite",   0x3764B8, 1, 0 }, { "MaxHealth.Hero",    0x3764BC, 1, 0 },
    { "MaxHealth.All",     0x3764B0, 4, 0 },
    { "MaxHealth.Recruit", 0x3764B0, 1, 0 },
    { "EnemyDamage",       0x3764B0, 4, 0 },   /* = every max health divided by k (same hits-to-die as k x damage taken) */
    { "Revives.Green",     0x376520, 1, 1 }, { "Revives.Veteran",   0x376524, 1, 1 },
    { "Revives.Elite",     0x376528, 1, 1 }, { "Revives.Hero",      0x37652C, 1, 1 },
    { "Revives.All",       0x376520, 4, 1 },
    { "Revives.Recruit",   0x376520, 1, 1 },
    { "HitZoneMult0",      0x3764D8, 1, 0 }, { "HitZoneMult1",      0x3764DC, 1, 0 },
    { "HitZoneMult2",      0x3764E0, 1, 0 }, { "HitZoneMult3",      0x3764E4, 1, 0 },
};

static const AitAlias *ait_alias(const char *k)
{
    size_t i;
    for (i = 0; i < sizeof s_alias / sizeof s_alias[0]; i++)
        if (!_stricmp(s_alias[i].name, k))
            return &s_alias[i];
    return NULL;
}

/* EnemyCount = k  (all levels) / EnemyCount.<level> = k : multiplies the spawners' MaxAlive (def field "MaxAlive", rounded up). */
static double s_ec_all = 1.0, s_ec_lvl[4];
static int s_ec_set;

static int ait_enemycount_key(const char *k, const char *v)
{
    static const char *lv[] = { "Recruit", "Veteran", "Elite", "Hero" };
    int i;
    if (!_stricmp(k, "EnemyCount")) { s_ec_all = atof(v); s_ec_set = 1; return 1; }
    if (!_strnicmp(k, "EnemyCount.", 11)) {
        if (!_stricmp(k + 11, "Green")) { s_ec_lvl[0] = atof(v); s_ec_set = 1; return 1; }
        for (i = 0; i < 4; i++)
            if (!_stricmp(k + 11, lv[i])) { s_ec_lvl[i] = atof(v); s_ec_set = 1; return 1; }
    }
    return 0;
}

static void ait_apply_file(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    int n = 0;

    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *k = line, *v, *eq, *t;
        int mul = 0;
        uint32_t va;
        line[strcspn(line, "\r\n")] = 0;
        t = strchr(line, '#');
        if (t)
            *t = 0;
        while (*k == ' ' || *k == '\t')
            k++;
        eq = strchr(k, '=');
        if (!*k || !eq)
            continue;
        if (eq > k && eq[-1] == '*') {
            mul = 1;
            eq[-1] = 0;
        }
        *eq = 0;
        v = eq + 1;
        while (*v == ' ' || *v == '\t')
            v++;
        t = k + strlen(k);
        while (t > k && (t[-1] == ' ' || t[-1] == '\t'))
            *--t = 0;
        if (!*v)
            continue;
        if (ait_enemycount_key(k, v)) {
            n++;
            continue;
        }
        {
            uint32_t a0, cnt = 1, step = 4, isint = 0, j;
            const AitAlias *al = ait_alias(k);
            int divide = !_stricmp(k, "EnemyDamage");
            if (al) {
                a0 = al->va; cnt = al->count; isint = al->isint;
            } else {
                char *end;
                a0 = (uint32_t)strtoul(k, &end, 16);
                if (*end || a0 < AIT_FIRST || a0 >= AIT_LAST || (a0 & 3)) {
                    fprintf(stderr, "[AITUNE] %s: unknown key or address \"%s\" (skipped)\n", path, k);
                    continue;
                }
            }
            for (j = 0; j < cnt; j++) {
                va = a0 + j * step;
                if (!_strnicmp(v, "i:", 2) || (isint && !mul)) {
                    *ait_word(va) = (uint32_t)strtol(!_strnicmp(v, "i:", 2) ? v + 2 : v, NULL, 0);
                } else if (isint) {
                    *ait_word(va) = (uint32_t)(int)((double)(int32_t)*ait_word(va) * atof(v) + 0.5);
                } else {
                    float cur, val = (float)atof(v);
                    memcpy(&cur, ait_word(va), 4);
                    if (divide)
                        val = val > 0.0f ? cur / val : cur;
                    else if (mul)
                        val *= cur;
                    memcpy(ait_word(va), &val, 4);
                }
            }
        }
        n++;
    }
    fclose(f);
    fprintf(stderr, "[AITUNE] %s: %d value(s) applied\n", path, n);
}

/* Entry probe for the difficulty consumers (RECOMP_AI_LOG=1): first time each (function, caller) pair is seen it
 * logs self (ecx) next to the player character pointer; every 4000 calls it appends the call counts. */
extern uint32_t game_pc_get(void);

typedef struct { uint32_t id, ret, self, n, pl, vt; } AitProbe;
static AitProbe s_pr[96];
static int s_pr_n, s_pr_on = -1;
static unsigned s_pr_calls;

void game_ai_probe(uint32_t id, uint32_t ret, uint32_t self)
{
    int i;
    FILE *f;
    {
        extern void game_aim_poll(void);
        game_aim_poll();                   /* F8/F9 live test (ai_aim.c), cheap when RECOMP_AI_LOG is off */
    }
    if (s_pr_on < 0) {
        const char *lg = getenv("RECOMP_AI_LOG");
        s_pr_on = lg && lg[0] == '1';
    }
    if (!s_pr_on)
        return;
    for (i = 0; i < s_pr_n; i++)
        if (s_pr[i].id == id && s_pr[i].ret == ret)
            break;
    if (i == s_pr_n) {
        uint32_t pc = game_pc_get();
        if (s_pr_n >= 96)
            return;
        s_pr[i].id = id; s_pr[i].ret = ret; s_pr[i].self = self; s_pr[i].n = 0;
        s_pr[i].pl = pc && self >= pc && self < pc + 0x2000u;
        s_pr[i].vt = (self >= 0x10000u && self < 0x84000000u) ? *(volatile uint32_t *)((uintptr_t)self + g_xbox_mem_offset) : 0;
        s_pr_n++;
        f = fopen("ai_probe.txt", "a");
        if (f) {
            fprintf(f, "NEW fn %06X  caller %08X  self %08X  vtable %08X  player %08X  isplayer %d\n", id, ret, self, s_pr[i].vt, pc, (int)s_pr[i].pl);
            fclose(f);
        }
    }
    s_pr[i].n++;
    if (++s_pr_calls % 4000u == 0) {
        f = fopen("ai_probe.txt", "a");
        if (f) {
            fprintf(f, "COUNTS after %u calls\n", s_pr_calls);
            for (i = 0; i < s_pr_n; i++)
                fprintf(f, "  fn %06X caller %08X n=%u\n", s_pr[i].id, s_pr[i].ret, s_pr[i].n);
            fclose(f);
        }
    }
}

/* ============================================================
 * Damage mode (Recomp Settings "Damage mode": Normal / Low damage) and the hit marker.
 *
 * What the game does (read from the generated code, not guessed): a soldier's health lives in a "meter" object at
 * soldier + 0x2C60 (soldier class vtable 0x317008): +0x98 = maximum, +0x9C = current, +0x88 = 3 once it reached zero.
 * Every change of that value goes through sub_00016BE0(this = meter, float delta); a hit is a negative delta. The hook
 * line in recomp_0000.c (marker DMG-MODE) calls game_meter_add before the add, and this code may rewrite the delta:
 *   Normal      nothing changes (a Damage line in mods\weapons\<Name>.txt works).
 *               Low damage ignores such a Damage line: the hit count is fixed, whatever the file says.
 *   Low damage  the bullet that hit is known from game_hit_confirm (ballistics row = bullet + 0x174; row +0 = the weapon's
 *               id, row +8 = base damage). Light guns (pistols, SMGs, MGs): 7 hits to kill, 3 for a head shot. Heavy
 *               single-shot guns (game_weapon_is_heavy: Karabiner, M1, scoped rifles): 2 hits to the body, 1 to the head.
 *               A head shot = the hit is 3x or more of the row's base damage. A hit with no known bullet (explosions, melee)
 *               keeps the old size buckets (dm_hits).
 * The player's health is a plain float at player + 0x3B0 and does not go through the meter, so Low damage does not touch
 * him. Friendly soldiers share the soldier class and follow the same rule.
 * Hit marker: sub_000FB7C0 (a soldier is hit by a bullet) calls game_hit_confirm once the ray test says the bullet is
 * inside the hit volume (hook marker HIT-MARK); the shooter is bullet + 0x180. game_hitmark_poll (once per frame, from
 * the sprint hook) then looks at the victim's meter: dead -> red X, alive -> white X, both drawn by nv2a_gpu_hud_hitmark.
 * ============================================================ */
extern void nv2a_gpu_hud_hitmark(int kind);
uint32_t game_pc_get(void);
int recomp_menu_up(void);

int game_weapon_is_heavy(uint32_t row_id, char *name_out, size_t name_sz);     /* recomp_manual.c */
void game_pain_event(uint32_t victim, int death);                               /* game_sfx.c */
void game_pain_poll(void);

static int s_dm = -1;                   /* 0 Normal, 1 Low damage; -1 = not read yet */
static int s_hm = -1;                   /* hit marker on/off */

static void dm_init(void)
{
    const char *e;
    if (s_dm >= 0)
        return;
    s_dm = 0;
    e = getenv("RECOMP_DAMAGE_MODE");
    if (e && e[0] && e[0] != '0' && e[0] != 'n' && e[0] != 'N')       /* old ini values 1 (Realism, removed) and 2 mean Low damage */
        s_dm = 1;
    e = getenv("RECOMP_HIT_MARKER");
    s_hm = !(e && e[0] == '0');
}
int  game_dmgmode_get(void)      { dm_init(); return s_dm; }
void game_dmgmode_set(int m)     { dm_init(); s_dm = m > 0 ? 1 : 0; }
int  game_hitmark_get(void)      { dm_init(); return s_hm; }
void game_hitmark_set(int on)    { dm_init(); s_hm = on ? 1 : 0; }

/* Unlock all missions: sub_001764B0 (LOADVARLEVELSCOMPLETEDINFO) hands the mission-select UI one "campaign_mission_completed"
 * record per profile entry (profile + 0x610, stride 0x50, completed = bit 0 of the byte at +0xC); the hook line in recomp_0043.c
 * (marker UNLOCK-ALL) makes it 1 for every valid entry while this is on. The save file itself is not touched. */
static int s_unlock = -1;
int  game_unlock_get(void)
{
    if (s_unlock < 0) {
        const char *e = getenv("RECOMP_UNLOCK_ALL");
        s_unlock = e && e[0] == '1';
    }
    return s_unlock;
}
void game_unlock_set(int on)     { s_unlock = on ? 1 : 0; }

/* guest memory reads that never fault (a bad pointer gives 0) */
static int dm_rd(uint32_t va, uint32_t *out)
{
    __try {
        *out = *(volatile uint32_t *)((uintptr_t)va + g_xbox_mem_offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 1;
}
static int dm_rdf(uint32_t va, float *out)
{
    uint32_t w;
    if (!dm_rd(va, &w))
        return 0;
    memcpy(out, &w, 4);
    return 1;
}

/* hits a soldier of this maximum health may take when the bullet is not known, by the size of the hit
 * (24-26 light guns, 50 heavy, x5 head, bigger = explosions) */
static float dm_hits(float a)
{
    if (a <= 30.0f) return 7.0f;
    if (a <= 60.0f) return 5.0f;
    if (a <= 130.0f) return 3.0f;
    return 2.0f;
}

/* the bullets that reached a soldier (game_hit_confirm runs before the damage is applied), newest last */
static struct { uint32_t meter; DWORD t; float base; int heavy; } s_blt[8];
static int s_blt_n;

static int dm_bullet_for(uint32_t meter, float *base, int *heavy)
{
    int i;
    DWORD now = GetTickCount();
    for (i = s_blt_n - 1; i >= 0 && i >= s_blt_n - 8; i--) {
        int k = i & 7;
        if (s_blt[k].meter == meter && (int)(now - s_blt[k].t) < 500) {
            *base = s_blt[k].base;
            *heavy = s_blt[k].heavy;
            return 1;
        }
    }
    return 0;
}

static void dm_note_bullet(uint32_t victim, uint32_t bullet)
{
    uint32_t row = 0, id = 0;
    float base = 0.0f, range = 0.0f;
    char nm[32] = "";
    int heavy;
    static int logged;

    if (!dm_rd(bullet + 0x174u, &row) || !dm_rd(row, &id) || !dm_rdf(row + 8u, &base) || !dm_rdf(row + 0x14u, &range))
        return;
    heavy = game_weapon_is_heavy(id, nm, sizeof nm) || range >= 200.0f;     /* range 239.76 = the Karabiner's row */
    s_blt[s_blt_n & 7].meter = victim + 0x2C60u;
    s_blt[s_blt_n & 7].t = GetTickCount();
    s_blt[s_blt_n & 7].base = base;
    s_blt[s_blt_n & 7].heavy = heavy;
    s_blt_n++;
    if (logged < 30) {
        logged++;
        fprintf(stderr, "[DMGMODE] bullet row %08X id %08X weapon '%s' damage %g range %g -> %s\n", row, id, nm,
                (double)base, (double)range, heavy ? "heavy" : "light");
    }
}

void game_meter_add(uint32_t meter, uint32_t sp)
{
    float d, mx, cur, a, share, nd, base = 0.0f;
    uint32_t vt;
    int heavy = 0, known, head = 0;
    static int logged;

    if (s_dm < 0)
        dm_init();
    if (!dm_rdf(sp + 4u, &d) || !(d < -0.0001f))
        return;
    if (meter < 0x80002C60u || meter >= 0x84000000u)
        return;
    if (!dm_rd(meter - 0x2C60u, &vt) || vt != 0x317008u)        /* not a soldier's meter */
        return;
    if (!dm_rdf(meter + 0x98u, &mx) || !dm_rdf(meter + 0x9Cu, &cur) || !(mx > 1.0f) || !(mx < 100000.0f))
        return;
    a = -d;
    nd = d;
    known = dm_bullet_for(meter, &base, &heavy);
    if (s_dm) {
        if (known && base > 0.01f) {
            head = a >= base * 3.0f;                    /* a head shot is 5x the row's damage */
            if (heavy)
                nd = head ? -(mx * 1.001f) : -(mx * 0.5005f);     /* 1 hit in the head, 2 in the body */
            else {
                share = mx / (head ? 3.0f : 7.0f) * 1.001f;       /* 3 hits in the head, 7 in the body */
                nd = -share;        /* always this size: a Damage line in mods\weapons is ignored while Low damage is on */
            }
        } else {
            share = mx / dm_hits(a) * 1.001f;
            if (a > share)
                nd = -share;
        }
        if (nd != d) {
            uint32_t w;
            memcpy(&w, &nd, 4);
            __try {
                *(volatile uint32_t *)((uintptr_t)(sp + 4u) + g_xbox_mem_offset) = w;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return;
            }
        }
    }
    {   /* PAIN SOUNDS (game_sfx.c): the final hit size decides pain or scream; a soldier who is already down makes no sound */
        uint32_t st = 0;
        if (cur > 0.0f && !(dm_rd(meter + 0x88u, &st) && st == 3u))
            game_pain_event(meter - 0x2C60u, cur + nd <= 0.0f);
    }
    if (logged < 40) {
        logged++;
        fprintf(stderr, "[DMGMODE] soldier meter %08X health %g of %g, hit %g -> %g (mode %d, %s%s)\n",
                meter, (double)cur, (double)mx, (double)a, (double)-nd, s_dm,
                known ? (heavy ? "heavy" : "light") : "no bullet", head ? " head" : "");
    }
}

static struct { uint32_t meter; DWORD t; float before; } s_pend[4];
static int s_npend;

void game_hit_confirm(uint32_t victim, uint32_t bullet)
{
    uint32_t shooter = 0, alt = 0, st = 0, pc = game_pc_get();
    float cur = 1.0f;
    static int logged_mine, logged_other;
    int mine;

    if (s_dm < 0)
        dm_init();
    if (s_dm)
        dm_note_bullet(victim, bullet);
    if (!s_hm)
        return;
    if (!dm_rd(bullet + 0x180u, &shooter))
        return;
    mine = pc && (uint32_t)(shooter - pc) < 0x400u;
    /* every line carries both owner candidates; other shooters are only logged when they sit near the player object, so a
     * wrong window shows up in one run instead of being hidden behind the first 12 enemy shots */
    if (mine ? logged_mine < 12 : (logged_other < 12 && pc && (uint32_t)(shooter - pc + 0x10000u) < 0x20000u)) {
        dm_rd(bullet + 0xE0u, &alt);
        fprintf(stderr, "[HITMARK] victim %08X shooter %08X (+0xE0 %08X) player %08X diff %d -> %s\n", victim, shooter, alt,
                pc, (int)(shooter - pc), mine ? "player's shot" : "not the player's");
        if (mine) logged_mine++; else logged_other++;
    }
    if (!mine || victim == pc)
        return;
    if (dm_rd(victim + 0x2C60u + 0x88u, &st) && st == 3u)         /* already dead */
        return;
    if (dm_rdf(victim + 0x2C60u + 0x9Cu, &cur) && cur <= 0.0f)
        return;
    if (s_npend < 4) {
        s_pend[s_npend].meter = victim + 0x2C60u;
        s_pend[s_npend].t = GetTickCount();
        s_pend[s_npend].before = cur;
        s_npend++;
    }
}

/* once per frame: shows the marker for the hits recorded at least 10 ms ago (the damage has been applied by then) */
void game_hitmark_poll(void)
{
    int i, j, kind = 0;
    DWORD now;
    static int logged;

    game_pain_poll();                                   /* pain / scream sounds and the player's health (game_sfx.c) */
    if (!s_npend)
        return;
    now = GetTickCount();
    for (i = j = 0; i < s_npend; i++) {
        uint32_t st = 0;
        float cur = 1.0f, mx = 0.0f;
        if ((int)(now - s_pend[i].t) < 10) {
            s_pend[j++] = s_pend[i];
            continue;
        }
        dm_rd(s_pend[i].meter + 0x88u, &st);
        dm_rdf(s_pend[i].meter + 0x9Cu, &cur);
        dm_rdf(s_pend[i].meter + 0x98u, &mx);
        /* a hit that did no damage (the game's own gates drop some) shows nothing: red = dead, white = health went down */
        if (logged < 20) {
            logged++;
            fprintf(stderr, "[HITMARK] resolve meter %08X health %g -> %g (max %g, state %u) -> %s\n", s_pend[i].meter,
                    (double)s_pend[i].before, (double)cur, (double)mx, st,
                    (st == 3u || cur <= 0.0f) ? "red X" : cur < s_pend[i].before - 0.001f ? "white X" : "no marker");
        }
        if (st == 3u || cur <= 0.0f)
            kind = 2;
        else if (cur < s_pend[i].before - 0.001f && !kind)
            kind = 1;
    }
    s_npend = j;
    if (kind && !recomp_menu_up())
        nv2a_gpu_hud_hitmark(kind);
}

void game_ai_tuning_loaded(void)
{
    const char *e = getenv("RECOMP_MODS"), *d = getenv("RECOMP_MODS_DIR"), *lg = getenv("RECOMP_AI_LOG");
    char root[MAX_PATH], path[MAX_PATH + 32], pat[MAX_PATH + 8];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    s_ait_loads++;
    if (lg && lg[0] == '1' && s_ait_loads <= 12)
        ait_dump();
    if (e && e[0] == '0')
        return;
    snprintf(root, sizeof root, "%s", d && d[0] ? d : "mods");
    snprintf(path, sizeof path, "%s\\ai\\tuning.txt", root);
    ait_apply_file(path);
    snprintf(pat, sizeof pat, "%s\\*", root);
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') {
                snprintf(path, sizeof path, "%s\\%s\\ai\\tuning.txt", root, fd.cFileName);
                ait_apply_file(path);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (lg && lg[0] == '1' && s_ait_loads <= 12)
        ait_dump();                         /* second section: the values after the mods were applied */
    fflush(stderr);
}

/* ============================================================
 * Def getter log + override (AI-TUNING). The game reads named fields of loaded defs with sub_000367F0 (float) and
 * sub_00036790 (int); the name is a CRC32. Both getters call game_def_f/game_def_i with the value they found.
 *   RECOMP_AI_LOG=1 -> ai_defs_log.txt: one line per (caller, hash) with the first value, and a counts file
 *                      ai_defs_counts.txt rewritten every 20000 calls.
 *   mods\ai\defs.txt (and mods\<pack>\ai\defs.txt):  <Name or hex hash> [@<hex caller>] = value  |  *= factor
 *      Name = CRC32 of the text exactly as written (the game's names are PascalCase words, e.g. EngagementDistance).
 *      @caller limits it to one call site (the return address in the log). Ints are rounded.
 * ============================================================ */
typedef struct { uint32_t hash, caller; int has_caller, mul; double v; } AitDef;
static AitDef s_df[256];
static int s_df_n, s_df_init, s_df_log = -1;
typedef struct { uint32_t ret, hash, n; int isint; uint32_t first; } AitSeen;
static AitSeen s_seen[4096];
static int s_seen_n;
static unsigned s_df_calls;

static uint32_t ait_crc32_impl(const char *s)
{
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; s++) {
        int k;
        c ^= (uint8_t)*s;
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1));
    }
    return ~c;
}

static void def_parse_file(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    if (!f)
        return;
    while (fgets(line, sizeof line, f) && s_df_n < 256) {
        char *k = line, *eq, *v, *t, *at;
        AitDef d;
        line[strcspn(line, "\r\n")] = 0;
        t = strchr(line, '#');
        if (t)
            *t = 0;
        while (*k == ' ' || *k == '\t')
            k++;
        eq = strchr(k, '=');
        if (!*k || !eq)
            continue;
        memset(&d, 0, sizeof d);
        if (eq > k && eq[-1] == '*') {
            d.mul = 1;
            eq[-1] = 0;
        }
        *eq = 0;
        v = eq + 1;
        d.v = atof(v);
        at = strchr(k, '@');
        if (at) {
            *at = 0;
            d.caller = (uint32_t)strtoul(at + 1, NULL, 16);
            d.has_caller = 1;
        }
        t = k + strlen(k);
        while (t > k && (t[-1] == ' ' || t[-1] == '\t'))
            *--t = 0;
        {
            char *end;
            unsigned long hv = strtoul(k, &end, 16);
            if (!*end && strlen(k) >= 6)
                d.hash = (uint32_t)hv;
            else
                d.hash = ait_crc32_impl(k);
        }
        s_df[s_df_n++] = d;
    }
    fclose(f);
}

static void def_init(void)
{
    const char *e = getenv("RECOMP_MODS"), *d = getenv("RECOMP_MODS_DIR"), *lg = getenv("RECOMP_AI_LOG");
    char root[MAX_PATH], path[MAX_PATH + 32], pat[MAX_PATH + 8];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    s_df_init = 1;
    s_df_log = lg && lg[0] == '1';
    if (e && e[0] == '0')
        return;
    snprintf(root, sizeof root, "%s", d && d[0] ? d : "mods");
    snprintf(path, sizeof path, "%s/ai/defs.txt", root);
    def_parse_file(path);
    snprintf(pat, sizeof pat, "%s/*", root);
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') {
                snprintf(path, sizeof path, "%s/%s/ai/defs.txt", root, fd.cFileName);
                def_parse_file(path);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (s_df_n)
        fprintf(stderr, "[AIDEF] %d override line(s) loaded\n", s_df_n);
}

static void def_log(uint32_t ret, uint32_t hash, uint32_t bits, int isint)
{
    int i;
    FILE *f;
    for (i = 0; i < s_seen_n; i++)
        if (s_seen[i].ret == ret && s_seen[i].hash == hash)
            break;
    if (i == s_seen_n) {
        if (s_seen_n >= 4096)
            return;
        s_seen[i].ret = ret; s_seen[i].hash = hash; s_seen[i].n = 0; s_seen[i].isint = isint; s_seen[i].first = bits;
        s_seen_n++;
        f = fopen("ai_defs_log.txt", "a");
        if (f) {
            float fl;
            memcpy(&fl, &bits, 4);
            if (isint)
                fprintf(f, "caller %08X hash %08X int %d\n", ret, hash, (int)bits);
            else
                fprintf(f, "caller %08X hash %08X float %g\n", ret, hash, (double)fl);
            fclose(f);
        }
    }
    s_seen[i].n++;
    if (++s_df_calls % 20000u == 0) {
        f = fopen("ai_defs_counts.txt", "w");
        if (f) {
            for (i = 0; i < s_seen_n; i++)
                fprintf(f, "caller %08X hash %08X n=%u\n", s_seen[i].ret, s_seen[i].hash, s_seen[i].n);
            fclose(f);
        }
    }
}

static int def_find(uint32_t ret, uint32_t hash, double *out_v, int *out_mul)
{
    int i, hit = -1;
    for (i = 0; i < s_df_n; i++)
        if (s_df[i].hash == hash && (!s_df[i].has_caller || s_df[i].caller == ret))
            hit = i;
    if (hit < 0)
        return 0;
    *out_v = s_df[hit].v;
    *out_mul = s_df[hit].mul;
    return 1;
}

float game_def_f(float v, uint32_t hash, uint32_t ret)
{
    uint32_t bits;
    double nv;
    int mul;
    if (!s_df_init)
        def_init();
    if (s_df_log) {
        memcpy(&bits, &v, 4);
        def_log(ret, hash, bits, 0);
    }
    if (s_df_n && def_find(ret, hash, &nv, &mul))
        return mul ? (float)(v * nv) : (float)nv;
    return v;
}

/* Recomp Settings rows: "Enemy count" (Off, x2..x5) and one On/Off row per difficulty. ini enemy_count=1..5 and
 * enemy_count_recruit|veteran|elite|hero=0/1; env RECOMP_ENEMY_COUNT, RECOMP_EC_RECRUIT etc. An EnemyCount line in
 * mods/ai/tuning.txt (when present) overrides the menu. The factor is never above 5. */
static int s_menu_k = -1, s_menu_lv[4] = { 1, 1, 1, 1 };

static void ec_menu_init(void)
{
    const char *e;
    if (s_menu_k >= 0)
        return;
    e = getenv("RECOMP_ENEMY_COUNT");
    s_menu_k = e ? atoi(e) : 1;
    if (s_menu_k < 1) s_menu_k = 1;
    if (s_menu_k > 5) s_menu_k = 5;
    {
        static const char *nm[4] = { "RECOMP_EC_RECRUIT", "RECOMP_EC_VETERAN", "RECOMP_EC_ELITE", "RECOMP_EC_HERO" };
        int i;
        for (i = 0; i < 4; i++) {
            e = getenv(nm[i]);
            s_menu_lv[i] = e && e[0] ? atoi(e) > 0 : 1;
        }
    }
}
int game_ec_get(void)            { ec_menu_init(); return s_menu_k; }
void game_ec_set(int k)          { ec_menu_init(); s_menu_k = k < 1 ? 1 : k > 5 ? 5 : k; }
int game_eclvl_get(int lvl)      { ec_menu_init(); return lvl >= 0 && lvl < 4 ? s_menu_lv[lvl] : 0; }
void game_eclvl_set(int lvl, int v) { ec_menu_init(); if (lvl >= 0 && lvl < 4) s_menu_lv[lvl] = v ? 1 : 0; }

static double ec_factor(void)
{
    uint32_t base = *(volatile uint32_t *)((uintptr_t)0x3888C0u + g_xbox_mem_offset);
    int lvl = 1;
    double k;
    ec_menu_init();
    if (base >= 0x100000u && base < 0x8000000u) {
        int d = (int)*(volatile uint32_t *)((uintptr_t)(base + 0xE70u) + g_xbox_mem_offset);
        if (d >= 0 && d < 4)
            lvl = d;
    }
    k = (double)s_menu_k;
    if (!s_menu_lv[lvl])
        k = 1.0;
    if (s_ec_set) {
        k = s_ec_all;
        if (s_ec_lvl[lvl] > 0.0)
            k = s_ec_lvl[lvl];
    }
    return k > 5.0 ? 5.0 : k;
}

/* Spawners: which team each spawner def (def+8, the getters' ecx) belongs to. sub_00038B70 reads the spawner def's Team at
 * 0x38C2A (1 = friends, 2 = enemies); that hook calls game_spawner_team. Enemy count only touches Team 2. */
static uint32_t s_defctx;
static struct { uint32_t def8; int team; int skip; } s_spt[256];
static unsigned s_spn;
void game_def_ctx(uint32_t c) { s_defctx = c; }
static int spawner_team(uint32_t def8)
{
    unsigned i;
    for (i = 0; i < s_spn; i++)
        if (s_spt[i].def8 == def8)
            return s_spt[i].skip ? -1 : s_spt[i].team;
    return 0;
}
static int spt_slot(uint32_t def8)
{
    unsigned i;
    for (i = 0; i < s_spn; i++)
        if (s_spt[i].def8 == def8)
            return (int)i;
    if (s_spn >= 256)
        return -1;
    s_spt[s_spn].def8 = def8;
    s_spt[s_spn].team = 0;
    s_spt[s_spn].skip = 0;
    return (int)s_spn++;
}

/* IN-GAME EDITOR (game_patches.c): the spawner's team (2 = enemies, -1 = leader, 0 unknown) and a one-spawner "one more may be alive"
 * window: while the boost runs, MaxAlive reads of that spawner's def return one more, so a pool of +1 really makes a soldier. */
static uint32_t s_ed_boost_def8;
static int s_ed_boost_left;
static uint32_t ed_def8(uint32_t spawner)
{
    uint32_t h = *(volatile uint32_t *)((uintptr_t)spawner + 0xCu + g_xbox_mem_offset);
    if (h < 0x80000000u || h >= 0x84000000u)
        return 0;
    return *(volatile uint32_t *)((uintptr_t)h + 4u + g_xbox_mem_offset) + 8u;
}
int game_editor_spawner_team(uint32_t spawner)
{
    uint32_t d = ed_def8(spawner);
    return d ? spawner_team(d) : 0;
}
void game_editor_boost(uint32_t spawner, int reads)
{
    s_ed_boost_def8 = ed_def8(spawner);
    s_ed_boost_left = reads;
}

/* Leaders (officers, commanders, captains ...) must not be multiplied. The soldier type a spawner makes is a name string read in
 * sub_000A1470 at 0x000A157F (hook game_spawner_name); a name containing one of these words, or a line of
 * mods\ai\enemycount_skip.txt (one word per line, any case), marks the spawner as skipped. Every new type name is logged. */
static const char *const k_leader_words[] = { "offic", "leader", "command", "captain", "capt", "lieut", "sergeant", "sgt", "boss", "chief", "colonel", "major", "hauptmann", "kommandant", "ldr", "cmdr", "marshal", "nemesis", "general", "engle", "schrader", "partisanleader", NULL };
static char s_skip_words[16][32];
static int s_skip_n = -1;
static void skip_words_load(void)
{
    FILE *f;
    char line[128];
    s_skip_n = 0;
    f = fopen("mods\\ai\\enemycount_skip.txt", "r");
    if (!f)
        return;
    while (s_skip_n < 16 && fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '))
            line[--n] = 0;
        if (n && line[0] != '#') {
            strncpy(s_skip_words[s_skip_n], line, 31);
            s_skip_words[s_skip_n][31] = 0;
            s_skip_n++;
        }
    }
    fclose(f);
}
static int name_has(const char *low, const char *w)
{
    char lw[32];
    size_t i;
    for (i = 0; w[i] && i < 31; i++)
        lw[i] = (char)((w[i] >= 'A' && w[i] <= 'Z') ? w[i] + 32 : w[i]);
    lw[i] = 0;
    return lw[0] && strstr(low, lw) != NULL;
}
void game_spawner_name(uint32_t spawner, uint32_t str)
{
    char nm[256], low[256];
    int i, skip = 0, slot;
    uint32_t def8, h;
    if (spawner < 0x80000000u || spawner >= 0x84000000u || str < 0x10000u)
        return;
    h = *(volatile uint32_t *)((uintptr_t)spawner + 0xCu + g_xbox_mem_offset);
    if (h < 0x80000000u || h >= 0x84000000u)
        return;
    def8 = *(volatile uint32_t *)((uintptr_t)h + 4u + g_xbox_mem_offset) + 8u;
    for (i = 0; i < 255; i++) {
        nm[i] = *(volatile char *)((uintptr_t)str + (uintptr_t)i + g_xbox_mem_offset);
        if (!nm[i])
            break;
        low[i] = (char)((nm[i] >= 'A' && nm[i] <= 'Z') ? nm[i] + 32 : nm[i]);
    }
    nm[i] = 0;
    low[i] = 0;
    if (!nm[0])
        return;
    game_editor_spawner_named(spawner, nm);                 /* map editor: which type file this spawner makes */
    if (s_skip_n < 0)
        skip_words_load();
    {   /* match the file name only (after the last backslash), not the folders */
        const char *bs = strrchr(low, 92);
        if (bs)
            memmove(low, bs + 1, strlen(bs + 1) + 1);
    }
    for (i = 0; k_leader_words[i]; i++)
        if (name_has(low, k_leader_words[i]))
            skip = 1;
    for (i = 0; i < s_skip_n; i++)
        if (name_has(low, s_skip_words[i]))
            skip = 1;
    slot = spt_slot(def8);
    if (slot < 0)
        return;
    if (s_spt[slot].skip != skip || !s_spt[slot].team) {
        static int said;
        if (said++ < 80)
            fprintf(stderr, "[ENEMYCOUNT] spawner type '%s' -> %s\n", nm, skip ? "leader, NOT multiplied" : "normal");
    }
    s_spt[slot].skip = skip;
}
void game_spawner_team(uint32_t spawner, uint32_t def8, uint32_t team)
{
    int i = spt_slot(def8);
    if (i < 0)
        return;
    s_spt[i].def8 = def8;
    s_spt[i].team = (int)team;
    ec_menu_init();
    if (team == 2 && !s_spt[i].skip && (s_ec_set || s_menu_k != 1) && spawner >= 0x80000000u && spawner < 0x84000000u &&
        *(volatile uint32_t *)((uintptr_t)spawner + 0x40u + g_xbox_mem_offset) == 0u) {
        double k = ec_factor();
        uint32_t pool = *(volatile uint32_t *)((uintptr_t)spawner + 0x48u + g_xbox_mem_offset);
        if (k > 1.0001 && (int32_t)pool > 0 && pool < 1000u) {
            uint32_t np = (uint32_t)(int)(k * (double)pool + 0.999);
            *(volatile uint32_t *)((uintptr_t)spawner + 0x48u + g_xbox_mem_offset) = np;
            {
                static int said;
                if (said++ < 30)
                    fprintf(stderr, "[ENEMYCOUNT] enemy spawner %08X pool %u -> %u (x%g)\n", spawner, pool, np, k);
            }
        }
    }
}

uint32_t game_def_i(uint32_t v, uint32_t hash, uint32_t ret)
{
    double nv;
    int mul;
    static uint32_t h_maxalive;
    ec_menu_init();
    if (!s_df_init)
        def_init();
    if (!h_maxalive)
        h_maxalive = ait_crc32_impl("MaxAlive");
    if (s_ed_boost_left > 0 && hash == h_maxalive && s_defctx == s_ed_boost_def8) {   /* editor: the one spawner asked to make one more */
        s_ed_boost_left--;
        v += 1u;
    }
    /* MaxAlive only for spawners whose Team is 2 (enemies); the total pool is scaled in game_spawner_team */
    if ((s_ec_set || s_menu_k != 1) && hash == h_maxalive && (int32_t)v > 0 && spawner_team(s_defctx) == 2) {
        double k = ec_factor();
        if (k > 1.0001) {
            uint32_t nvv = (uint32_t)(int)(k * (double)(int32_t)v + 0.999);
            {
                static int said;
                if (said++ < 30)
                    fprintf(stderr, "[ENEMYCOUNT] MaxAlive %d -> %u (x%g, caller %08X, enemy spawner)\n", (int)v, nvv, k, ret);
            }
            v = nvv;
        }
    }
    if (s_df_log)
        def_log(ret, hash, v, 1);
    if (s_df_n && def_find(ret, hash, &nv, &mul))
        return (uint32_t)(int)(mul ? (double)(int32_t)v * nv + 0.5 : nv + 0.5);
    return v;
}

/* FRIENDS mode (Recomp Settings row "Squad mates"): 0 Normal, 1 Tough, 2 Weak. A friend = a soldier-class object (vtable 0x317008)
 * whose team code at obj+0xD8 is 6 (measured with RECOMP_TEAM_PROBE, 2026-11-18: 6 = squad mates, 3 = enemies).
 *  - game_friend_dmg(proj): called from the projectile damage getter sub_00185120 (recomp_0044.c, marked FRIENDS); owner of a
 *    projectile = proj+0x180. Returns the damage multiplier (Tough x3, Weak x0.4, else 1).
 *  - game_friend_aim(owner, dir): called from game_proj_fix at bullet creation; Weak adds a random sideways / vertical error
 *    (about 5 degrees) to the bullet direction. ini friends_mode, env RECOMP_FRIENDS. */
static int s_friends = -1;
static int friends_mode(void)
{
    if (s_friends < 0) {
        const char *e = getenv("RECOMP_FRIENDS");
        s_friends = e && e[0] ? atoi(e) : 0;
        if (s_friends < 0 || s_friends > 2) s_friends = 0;
    }
    return s_friends;
}
int game_friends_get(void)       { return friends_mode(); }
void game_friends_set(int m)     { s_friends = m < 0 || m > 2 ? 0 : m; }

static int is_friend(uint32_t o)
{
    if (!((o >= 0x80000000u && o < 0x84000000u)))
        return 0;
    return *(volatile uint32_t *)((uintptr_t)o + g_xbox_mem_offset) == 0x317008u &&
           *(volatile uint32_t *)((uintptr_t)o + 0xD8u + g_xbox_mem_offset) == 6u;
}

float game_friend_dmg(uint32_t proj)
{
    int m = friends_mode();
    uint32_t owner;
    float k;
    if (!m || proj < 0x80000000u || proj >= 0x84000000u)
        return 1.0f;
    owner = *(volatile uint32_t *)((uintptr_t)proj + 0x180u + g_xbox_mem_offset);
    if (!is_friend(owner))
        return 1.0f;
    k = m == 1 ? 3.0f : 0.4f;
    {
        static int said;
        if (said++ < 12)
            fprintf(stderr, "[FRIENDS] mode %d: friend %08X damage x%g\n", m, owner, (double)k);
    }
    return k;
}

static uint32_t s_fr_rng = 0x1234567u;
static float fr_rand(void)                 /* -1..1 */
{
    s_fr_rng ^= s_fr_rng << 13; s_fr_rng ^= s_fr_rng >> 17; s_fr_rng ^= s_fr_rng << 5;
    return (float)(s_fr_rng & 0xFFFFu) / 32767.5f - 1.0f;
}

void game_friend_aim(uint32_t owner, uint32_t dir)
{
    float d[3];
    float len;
    int i;
    if (friends_mode() != 2 || dir < 0x1000u || !is_friend(owner))
        return;
    for (i = 0; i < 3; i++)
        memcpy(&d[i], (const void *)((uintptr_t)dir + 4u * (uintptr_t)i + g_xbox_mem_offset), 4);
    len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 0.01f)
        return;
    for (i = 0; i < 3; i++) {
        d[i] += fr_rand() * 0.09f * len;           /* about 5 degrees each axis */
        memcpy((void *)((uintptr_t)dir + 4u * (uintptr_t)i + g_xbox_mem_offset), &d[i], 4);
    }
}
