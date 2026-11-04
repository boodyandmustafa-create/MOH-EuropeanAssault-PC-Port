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
    /* spawner total pool: sub_000A0F40 sets obj+0x48 = random(0x8AB12380, 0xB6BC1CD9) (both ints, callers A0F67/A0F7C) */
    if ((s_ec_set || s_menu_k != 1) && (hash == h_maxalive || ((hash == 0x8AB12380u && ret == 0x000A0F67u) || (hash == 0xB6BC1CD9u && ret == 0x000A0F7Cu))) && (int32_t)v > 0) {
        double k = ec_factor();
        if (k > 1.0001) {
            uint32_t nvv = (uint32_t)(int)(k * (double)(int32_t)v + 0.999);
            {
                static int said;
                if (said++ < 30)
                    fprintf(stderr, "[ENEMYCOUNT] hash %08X %d -> %u (x%g, caller %08X)\n", hash, (int)v, nvv, k, ret);
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
