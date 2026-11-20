/* IN-GAME EDITOR (2026-11-20). Included by game_patches.c just before game_ctor_trace; no other file includes it.
 *
 * With the free camera on (Insert), NUMPAD2 puts a soldier of the enemy team, NUMPAD3 one of the friendly team, 6 units in front of the
 * camera. NUMPAD7 takes the last one back. NUMPAD4 kills the soldier the camera looks at. NUMPAD6 empties the saved list of this level.
 * Every placement is written to mods\maps\<level>.txt (level = the folder of level_a.str, e.g. 1_11) and made again by itself a few
 * seconds after that level starts. Format, one line each:   soldier <team 2 enemy | 1 friend> <x> <y> <z> <type file name or ->
 *
 * How a soldier is made (spike, builds 1..3): a spawner (vtable 0x30ED54) has no position, only counters (+0x40 spawned, +0x44 killed,
 * +0x48 pool) and a timer; its handler sub_000A1470 makes a soldier when pool > 0 and fewer are alive than MaxAlive. The game calls it
 * from the timer or at area load, so we call it ourselves: pool + 1, MaxAlive + 1 for that one spawner (game_editor_boost in
 * ai_tuning.c), with the stack arguments the game last passed that spawner (captured at its entry hook, gen/recomp_0019.c
 * loc_000A1486). The soldier constructor (hook in game_ctor_trace) runs inside that call; the new soldier is then written to the wanted
 * position (+0x60 / +0xA0 / +0x100 and the first holders found by searching its object for its own position) for ~90 frames, after
 * which the game's own physics lets it fall to the floor and its AI takes over. Moving an EXISTING soldier does not work: its AI walks
 * it back to its post. Which type comes out is the spawner's choice (random inside its list), so the type name is kept only to prefer
 * the same spawner next time.
 *
 * Off unless ini map_editor=1 / RECOMP_EDITOR=1 (F1 row "Map editor"); then the cost is one call a frame. RECOMP_EDITOR_LOG=1 adds [EDITOR]
 * detail lines. Keys can be moved with RECOMP_EDITOR_KEYS="enemy,friend,undo,kill,clear" (five virtual-key numbers). */
#define ED_NSP 32
#define ED_NSOL 256
#define ED_NSLOT 24
#define ED_NPL 128
#define ED_NHOLD 8

#define ED_PATH 160
#define ED_NCAT 96

static uint32_t s_ed_sp[ED_NSP], s_ed_sol[ED_NSOL];
static uint32_t s_ed_arg[ED_NSP][8];            /* the stack arguments the GAME passed to the spawner handler last (entry esp + 4 ..), per spawner */
static char s_ed_name[ED_NSP][ED_PATH];         /* the type file the spawner asked for last (full path), empty = not seen yet */
static int s_ed_nsp, s_ed_nsol;
/* the catalog: every soldier type file (full path) a spawner of this level has asked for, and the first spawner that asked for it */
static char s_ed_cat[ED_NCAT][ED_PATH];
static uint32_t s_ed_cat_sp[ED_NCAT];
static int s_ed_ncat, s_ed_sel = -1;            /* s_ed_sel = the type NUMPAD2 places (-1 = the spawner's own pick) */
static int s_ed_deleting;                       /* our own call of the delete command is running (game_keep_corpse_obj must let it through) */
static uint32_t s_ed_cur_sp;                    /* the spawner our current call is made through */
static uint32_t s_ed_name_ptr;                  /* guest address of the type path to ask for instead of the spawner's own, 0 = leave it */
static int s_ed_cfg = -1, s_ed_enabled, s_ed_dbg;
static int s_ed_self_call;                      /* our own call of the handler is running */
static int s_ed_apply = 1;                      /* the player's edits of THIS level are used when the level starts or restarts (line "enabled 0" in mods\maps\<level>.txt turns them off) */

typedef struct { uint32_t s; float tgt[3]; int left, manual, found; uint32_t hold[ED_NHOLD]; int nhold; float p0[3]; uint32_t sp; int swapped; int h25; } EdSlot;   /* h25: the +0x25C4 object's triple at +0x80 was written (HOME test, 0 = not yet, 1 = written, 2 = looked and not usable) */   /* p0 = the position the game gave it (before our first write) */
static EdSlot s_ed_slot[ED_NSLOT];
typedef struct { int team; float p[3]; char name[ED_PATH]; } EdPlace;
static EdPlace s_ed_pl[ED_NPL];
static uint32_t s_ed_pl_sol[ED_NPL];            /* the soldier that belongs to each saved line (0 = not made this session) */
static int s_ed_npl;
static int s_ed_cur_swapped;                    /* the spawn point was swapped for the target during this spawn (see ed_spawn) */
/* saved props (pickups made with sub_00037CD0 from a template the level itself made): placed again at every level start / restart */
#define ED_NPP 32
typedef struct { float p[3]; char path[ED_PATH]; } EdProp;
static EdProp s_ed_pp[ED_NPP];
static int s_ed_npp;
static int s_ed_pr, s_ed_pr_i, s_ed_pr_tries;   /* prop replay: 0 idle, 1 waiting for the level to run, 2 placing */
static uint32_t s_ed_pr_t0;
/* SPAWN ZONES (2026-11-21): our own spawn areas, not game spawner objects. A zone = centre, radius, soldier type, soldiers per wave, waves, trigger distance.
 * While the player is within the trigger distance and none of the zone's soldiers is alive, the next wave is born at random points inside the circle
 * (one soldier every few frames, through ed_spawn like every editor soldier). Killed soldiers count against count * waves, so a zone ends; soldiers the game
 * unloaded (player went away) are not counted as killed and come back when the player returns. Saved as "zone" lines in mods\maps\<level>.txt. */
#define ED_NZN 6
#define ED_ZMAX 8
typedef struct { float p[3], r, trig; int count, waves, wander, cover; char name[ED_PATH]; } EdZone;
static EdZone s_ed_zn[ED_NZN];
static int s_ed_nzn;
static uint32_t s_zn_sol[ED_NZN][ED_ZMAX];
static int s_zn_killed[ED_NZN], s_zn_pend[ED_NZN], s_zn_force[ED_NZN], s_zn_fail[ED_NZN];
static int s_zn_need[ED_NZN], s_zn_live[ED_NZN], s_zn_k0[ED_NZN];   /* need: a wave was shot down, the next one waits until the player has been out of the wake distance; live: a wave is out; k0: kills when it started */
static uint32_t s_zn_t0;
static int s_zn_def_count = 4, s_zn_def_waves = 2, s_zn_def_wander = 1, s_zn_def_cover = 0;
static float s_zn_def_r = 6.0f, s_zn_def_trig = 50.0f;
static void ed_zone_clear(void);
static int ed_zone_note_deleted(uint32_t soldier);
static int s_ed_zn_wander_on(uint32_t soldier);
static int ed_zone_wander_of(uint32_t soldier, float *c, float *r, int *cover);
static float ed_cast(const float *a, const float *b, uint32_t mask, uint32_t *obj);
static float ed_zrand(void);
static void ed_zone_remove(int zi);
static int ed_world_point(const float *eye, const float *dir, float *out);
static void ed_zone_reset_runtime(void)
{
    memset(s_zn_sol, 0, sizeof s_zn_sol);
    memset(s_zn_killed, 0, sizeof s_zn_killed);
    memset(s_zn_pend, 0, sizeof s_zn_pend);
    memset(s_zn_force, 0, sizeof s_zn_force);
    memset(s_zn_fail, 0, sizeof s_zn_fail);
    memset(s_zn_need, 0, sizeof s_zn_need);
    memset(s_zn_live, 0, sizeof s_zn_live);
    memset(s_zn_k0, 0, sizeof s_zn_k0);
    s_zn_t0 = 0;
}
static char s_ed_selprop[ED_PATH];              /* the prop type chosen in the window (empty = a soldier type is chosen) */
static int s_ptpl_n;                            /* templates seen this level (PROP MAKER below) */
static struct { uint32_t hash; int ok; uint32_t frame; } s_pt_res[160];   /* residency answers of this level (ed_kind_resident) */
static int s_pt_resn, s_pt_probed;
static uint32_t s_pt_pt0;
static uint32_t s_ed_pp_obj[ED_NPP];            /* the pickup object (vtable 0x317248) that belongs to each saved prop line, 0 = not made in this world */
static uint32_t s_pt_inner, s_pt_last_inner;    /* the object the pickup constructor built while OUR make call ran (the one the creator gives the matrix to) */
static int s_pt_making;
static char s_ed_level[24];                     /* "1_11" */
static int s_ed_replay;                         /* 0 idle, 1 waiting for the level to run, 2 placing */
static uint32_t s_ed_t0;                        /* game frame the level was first seen running */
static int s_ed_replay_i, s_ed_gap, s_ed_tries;
static uint32_t s_ed_last_sol;                  /* the last soldier placed by hand (for undo) */
static int s_ed_rot;                            /* round-robin over the spawners of a team, so the types vary */
/* the spawn in progress (set around our call of the handler) */
static int s_ed_in_spawn, s_ed_cur_team, s_ed_cur_record, s_ed_cur_manual, s_ed_answered, s_ed_cur_slot;
static float s_ed_cur_tgt[3];

extern int game_editor_spawner_team(uint32_t spawner);
extern void game_editor_boost(uint32_t spawner, int reads);
extern int game_freecam_active(void);
uint32_t game_pc_get(void);
extern void recomp_osd_show(const char *msg);

static int s_ed_anyslot;                        /* a placed soldier is still being written */
#define ED_NPLACED 192
static uint32_t s_ed_placed[ED_NPLACED];                /* every soldier the editor made this level (the unload quota refund must not respawn the spawner's own) */
static int s_ed_nplaced;
static int ed_on(void)
{
    if (s_ed_cfg < 0) {
        const char *e = getenv("RECOMP_EDITOR"), *d = getenv("RECOMP_EDITOR_LOG");
        s_ed_dbg = d && *d == '1';
        s_ed_enabled = (e && *e == '1') || s_ed_dbg;
        s_ed_cfg = 1;
    }
    return s_ed_enabled;
}
/* The toggle only gates the keys. The level id, the spawner list and the saved layout are kept whatever it says (rare events, no per-frame
 * cost), so a layout replays for a player who never turns the editor on, and turning it on from F1 inside a mission works at once. */
int game_editor_is_placed(uint32_t soldier)
{
    int i;
    for (i = 0; i < s_ed_nplaced && i < ED_NPLACED; i++)
        if (s_ed_placed[i] == soldier)
            return 1;
    return 0;
}
int  game_editor_get(void)      { return ed_on(); }
void game_editor_set(int v)     { ed_on(); s_ed_enabled = v ? 1 : 0; }

static void ed_gcat_add(const char *path);      /* map_editor_ui.c (included at the end) */
static int ed_anytype(void);
static int s_ed_capped;                         /* ed_spawn refused because of the limit */
static int ed_cap(void)                         /* most editor soldiers alive / saved per level */
{
    static int cap = -1;
    if (cap < 0) {
        const char *e = getenv("RECOMP_EDITOR_MAX");
        cap = e && atoi(e) > 0 ? atoi(e) : 16;
    }
    return cap;
}
static int ed_ptr(uint32_t p) { return p >= 0x80000000u && p < 0x84000000u; }
static float ed_f(uint32_t va) { float f; uint32_t w = MEM32(va); memcpy(&f, &w, 4); return f; }
static void ed_setf(uint32_t va, float f) { uint32_t w; memcpy(&w, &f, 4); MEM32(va) = w; }

static void ed_osd(const char *fmt, int n)
{
    char b[96];
    snprintf(b, sizeof b, fmt, n);
    recomp_osd_show(b);
}

/* ---------- the spawners and soldiers the game makes ---------- */
static int ed_sp_slot(uint32_t sp, int add)
{
    int i;
    for (i = 0; i < s_ed_nsp; i++)
        if (s_ed_sp[i] == sp)
            return i;
    if (!add || s_ed_nsp >= ED_NSP)
        return -1;
    s_ed_sp[s_ed_nsp] = sp;
    s_ed_name[s_ed_nsp][0] = 0;
    return s_ed_nsp++;
}
/* sub_000A1470 entry (hook after its prologue: esp = entry esp - 0x48, so the first argument is at +0x4C) */
void game_editor_spawner(uint32_t sp, uint32_t hook_esp)
{
    int i, k;
    if (!ed_ptr(sp) || s_ed_self_call)
        return;
    i = ed_sp_slot(sp, 1);
    if (i < 0)
        return;
    for (k = 0; k < 8; k++)
        s_ed_arg[i][k] = MEM32(hook_esp + 0x4Cu + 4u * (uint32_t)k);
}
/* ai_tuning.c game_spawner_name: the spawner is about to request a soldier of this type file */
void game_editor_spawner_named(uint32_t sp, const char *path)
{
    int i, c;
    if (!path || !path[0] || s_ed_self_call)        /* our own call asks for the type we chose: not a spawner's own pick */
        return;
    i = ed_sp_slot(sp, 1);
    if (i < 0)
        return;
    strncpy(s_ed_name[i], path, ED_PATH - 1);
    s_ed_name[i][ED_PATH - 1] = 0;
    if (_strnicmp(s_ed_name[i], "Z:\\moh4\\data\\shared\\definitions\\characters\\", 43) == 0)
        ed_gcat_add(s_ed_name[i]);              /* the global list of soldier types (mods\maps\catalog.txt) */
    for (c = 0; c < s_ed_ncat; c++)
        if (!_stricmp(s_ed_cat[c], s_ed_name[i]))
            return;
    if (s_ed_ncat < ED_NCAT) {
        strcpy(s_ed_cat[s_ed_ncat], s_ed_name[i]);
        s_ed_cat_sp[s_ed_ncat] = sp;
        s_ed_ncat++;
        if (s_ed_dbg)
            fprintf(stderr, "[EDITOR] catalog %d: %s (spawner %08X)\n", s_ed_ncat, s_ed_name[i], (unsigned)sp);
    }
}
/* hook after the spawner's type-name lookup (gen/recomp_0019.c, 0xA157F): returns the string pointer the spawner should use */
uint32_t game_editor_name_override(uint32_t sp, uint32_t str)
{
    if (s_ed_self_call && s_ed_name_ptr && sp == s_ed_cur_sp)
        return s_ed_name_ptr;
    return str;
}
int game_editor_deleting(void) { return s_ed_deleting; }
/* the catalog line whose path is, or ends with, want (a saved line may hold only the file name) */
static int ed_cat_find(const char *want)
{
    size_t wl;
    int c;
    if (!want || !want[0])
        return -1;
    wl = strlen(want);
    for (c = 0; c < s_ed_ncat; c++) {
        size_t cl = strlen(s_ed_cat[c]);
        if (cl >= wl && !_stricmp(s_ed_cat[c] + cl - wl, want) && (cl == wl || s_ed_cat[c][cl - wl - 1] == '\\'))
            return c;
    }
    return -1;
}
static const char *ed_base(const char *p)
{
    const char *b = strrchr(p, '\\');
    return b ? b + 1 : p;
}
static void ed_note_soldier(uint32_t self)
{
    int i;
    if (!ed_ptr(self))
        return;
    for (i = 0; i < s_ed_nsol; i++)
        if (s_ed_sol[i] == self)
            return;
    if (s_ed_nsol < ED_NSOL)
        s_ed_sol[s_ed_nsol++] = self;
    else
        s_ed_sol[g_game_frames % ED_NSOL] = self;
}

/* soldier state: vtable 0x317008, team +0xD8 (3 enemy, 6 friend), health meter at +0x2C60 (+0x88 state, 3 = dead; +0x9C health) */
static int ed_is_soldier(uint32_t s) { return ed_ptr(s) && MEM32(s) == 0x317008u; }
static int ed_alive(uint32_t s)
{
    uint32_t m = s + 0x2C60u;
    return ed_is_soldier(s) && MEM32(m + 0x88) != 3u && ed_f(m + 0x9C) > 0.0f;
}

/* ---------- writing a position into a soldier ---------- */
/* an object other soldiers (or the spawner) point to as well is shared: writing a position into it would move something that is not ours */
static int ed_shared(uint32_t base, uint32_t self)
{
    int i, j;
    for (i = 0; i < s_ed_nsol; i++) {
        uint32_t o = s_ed_sol[i];
        if (!o || o == self || !ed_is_soldier(o))
            continue;
        for (j = 0; j < 0x130 / 4; j++)
            if (MEM32(o + 0x10u + 4u * (uint32_t)j) == base)
                return 1;
    }
    if (ed_ptr(s_ed_cur_sp))
        for (j = 0; j < 0x60 / 4; j++)
            if (MEM32(s_ed_cur_sp + 4u * (uint32_t)j) == base)
                return 1;
    return 0;
}
static void ed_find_holders(EdSlot *e)
{
    uint32_t s = e->s, o;
    float old[3];
    int j;
    old[0] = ed_f(s + 0x60); old[1] = ed_f(s + 0x64); old[2] = ed_f(s + 0x68);
    e->nhold = 0;
    for (j = 0; j < 0x130 / 4; j++) {           /* the first 0x400 bytes of every heap object the soldier points to (fields 0x10..0x140) */
        uint32_t base = MEM32(s + 0x10u + 4u * (uint32_t)j), len = 0x400u;
        if (!ed_ptr(base) || base == s || ed_shared(base, s))
            continue;
        if (base + len > 0x84000000u)
            len = 0x84000000u - base;
        for (o = 0; o + 12u <= len && e->nhold < ED_NHOLD; o += 4)
            if (fabsf(ed_f(base + o) - old[0]) < 0.02f && fabsf(ed_f(base + o + 4) - old[1]) < 0.02f && fabsf(ed_f(base + o + 8) - old[2]) < 0.02f)
                e->hold[e->nhold++] = base + o;
    }
    if (s_ed_dbg)
        fprintf(stderr, "[EDITOR] %08X: %d position holders outside the soldier\n", (unsigned)s, e->nhold);
}
/* HOME TEST (advisor 2026-11-21): soldier+0x25C4 points to an object whose +0x80 triple held the exact birth point of every soldier, different per soldier (so
 * not shared data); the same address is also reached as holder(+0xB8)+0x100+0x110. Is it the body's position or the home? Write the target into it with the
 * rest of the pin (only when it is the soldier's own: heap, not pointed to by another soldier, triple within 0.6 m of the birth or target point). */
static uint32_t ed_h25(uint32_t s)
{
    uint32_t h = MEM32(s + 0x25C4u), i;
    if (!ed_ptr(h) || h + 0x90u > 0x84000000u)
        return 0;
    for (i = 0; i < s_ed_nsol; i++)
        if (s_ed_sol[i] && s_ed_sol[i] != s && ed_is_soldier(s_ed_sol[i]) && MEM32(s_ed_sol[i] + 0x25C4u) == h)
            return 0;
    return h + 0x80u;
}
static void ed_put(EdSlot *e)
{
    static const uint32_t k_off[3] = { 0x60u, 0xA0u, 0x100u };
    int a, c;
    if (e->h25 != 1 && s_ed_dbg) {
        uint32_t t = ed_h25(e->s);
        if (t) {
            float x = ed_f(t), y = ed_f(t + 4), z = ed_f(t + 8);
            if ((fabsf(x - e->p0[0]) < 0.6f && fabsf(y - e->p0[1]) < 0.6f && fabsf(z - e->p0[2]) < 0.6f) ||
                (fabsf(x - e->tgt[0]) < 0.6f && fabsf(y - e->tgt[1]) < 0.6f)) {
                for (c = 0; c < 3; c++)
                    ed_setf(t + 4u * (uint32_t)c, e->tgt[c]);
                e->h25 = 1;
                fprintf(stderr, "[HOME] soldier %08X: +25C4 field %08X written (was %.2f %.2f %.2f, target %.2f %.2f %.2f)\n", (unsigned)e->s, (unsigned)t, (double)x, (double)y,
                        (double)z, (double)e->tgt[0], (double)e->tgt[1], (double)e->tgt[2]);
            }
        } else if (!e->h25) {
            e->h25 = 2;
            fprintf(stderr, "[HOME] soldier %08X: +25C4 = %08X not usable (not set yet, or shared)\n", (unsigned)e->s, (unsigned)MEM32(e->s + 0x25C4u));
        }
    }
    if (e->h25 == 1) {
        uint32_t t = ed_h25(e->s);
        if (t)
            for (c = 0; c < 3; c++)
                ed_setf(t + 4u * (uint32_t)c, e->tgt[c]);
    }
    for (a = 0; a < 3; a++)
        for (c = 0; c < 3; c++)
            ed_setf(e->s + k_off[a] + 4u * (uint32_t)c, e->tgt[c]);
    for (a = 0; a < e->nhold; a++)
        for (c = 0; c < 3; c++)
            ed_setf(e->hold[a] + 4u * (uint32_t)c, e->tgt[c]);
}
/* HOME DIAGNOSTIC (RECOMP_EDITOR_LOG=1 only): where does a placed soldier go once our position pin lets go, and which fields of it (or of the objects it
 * points to) hold that place? The user sees: still while the free camera is on, runs to "a spawn point" when it closes. Log: P0 = the position the game
 * gave it at birth, every 60 frames its position, the final stop D, and at the release and at the stop every float triple near P0 / D with its path. */
typedef struct { uint32_t s; float p0[3], last[3]; uint32_t next; int stable, logged, lines, goalscans; uint32_t h25; } EdTrk;
static EdTrk s_ed_trk[6];
static int s_ed_ntrk;
static void ed_scan_near(const char *tag, uint32_t s, const float *c, float r, int *lines)
{
    uint32_t o, j;
    for (o = 0; o + 12u <= 0x2D00u && *lines < 40; o += 4)
        if (o != 0x60u && o != 0xA0u && o != 0x100u && fabsf(ed_f(s + o) - c[0]) < r && fabsf(ed_f(s + o + 4) - c[1]) < r && fabsf(ed_f(s + o + 8) - c[2]) < r) {
            fprintf(stderr, "[HOME] %s: sol+%03X holds %.2f %.2f %.2f\n", tag, (unsigned)o, (double)ed_f(s + o), (double)ed_f(s + o + 4), (double)ed_f(s + o + 8));
            (*lines)++;
        }
    for (j = 0; j < 0x130 / 4 && *lines < 40; j++) {
        uint32_t base = MEM32(s + 0x10u + 4u * j), len = 0x400u;
        if (!ed_ptr(base) || base == s)
            continue;
        if (base + len > 0x84000000u)
            len = 0x84000000u - base;
        for (o = 0; o + 12u <= len && *lines < 40; o += 4)
            if (fabsf(ed_f(base + o) - c[0]) < r && fabsf(ed_f(base + o + 4) - c[1]) < r && fabsf(ed_f(base + o + 8) - c[2]) < r) {
                fprintf(stderr, "[HOME] %s: sol+%03X -> %08X +%03X holds %.2f %.2f %.2f%s\n", tag, (unsigned)(0x10u + 4u * j), (unsigned)base, (unsigned)o,
                        (double)ed_f(base + o), (double)ed_f(base + o + 4), (double)ed_f(base + o + 8), ed_shared(base, s) ? " (SHARED)" : "");
                (*lines)++;
            }
    }
}
/* two levels deep from a root object: root fields (first `rootlen` bytes) -> objects (0x400 bytes) -> objects (0x200 bytes). The soldier turned back to its birth
 * point although no float triple near it was found one level down: the home may sit in the spawner / a spawn-point node, or deeper. */
static void ed_scan_deep(const char *tag, uint32_t root, uint32_t rootlen, const float *c, float r, int *lines)
{
    uint32_t seen[512], nseen = 0, f, o, g, q;
    for (f = 0; f + 4u <= rootlen && *lines < 60; f += 4) {
        uint32_t b1 = MEM32(root + f);
        if (!ed_ptr(b1) || b1 == root)
            continue;
        for (q = 0; q < nseen && seen[q] != b1; q++)
            ;
        if (q < nseen)
            continue;
        if (nseen < 512)
            seen[nseen++] = b1;
        for (o = 0; o < 0x400u && b1 + o + 12u <= 0x84000000u && *lines < 60; o += 4) {
            uint32_t b2 = MEM32(b1 + o);
            if (fabsf(ed_f(b1 + o) - c[0]) < r && fabsf(ed_f(b1 + o + 4) - c[1]) < r && fabsf(ed_f(b1 + o + 8) - c[2]) < r) {
                fprintf(stderr, "[HOME] %s: root+%03X -> %08X +%03X holds %.2f %.2f %.2f%s\n", tag, (unsigned)f, (unsigned)b1, (unsigned)o, (double)ed_f(b1 + o),
                        (double)ed_f(b1 + o + 4), (double)ed_f(b1 + o + 8), ed_shared(b1, root) ? " (SHARED)" : "");
                (*lines)++;
            }
            if (!ed_ptr(b2) || b2 == root || b2 == b1)
                continue;
            for (q = 0; q < nseen && seen[q] != b2; q++)
                ;
            if (q < nseen)
                continue;
            if (nseen < 512)
                seen[nseen++] = b2;
            for (g = 0; g < 0x200u && b2 + g + 12u <= 0x84000000u && *lines < 60; g += 4)
                if (fabsf(ed_f(b2 + g) - c[0]) < r && fabsf(ed_f(b2 + g + 4) - c[1]) < r && fabsf(ed_f(b2 + g + 8) - c[2]) < r) {
                    fprintf(stderr, "[HOME] %s: root+%03X -> %08X +%03X -> %08X +%03X holds %.2f %.2f %.2f\n", tag, (unsigned)f, (unsigned)b1, (unsigned)o, (unsigned)b2,
                            (unsigned)g, (double)ed_f(b2 + g), (double)ed_f(b2 + g + 4), (double)ed_f(b2 + g + 8));
                    (*lines)++;
                }
        }
    }
}
/* WHO READS THE HOME (advisor 2026-11-21, debug only): the goal lives in shared per-spawner data, so no per-soldier field can be written. Instead watch every
 * READ of that triple with the kernel's read watch (page made no-access, each access single-stepped, RECOMP_WATCH_DISTINCT = one line per reading function with the
 * regs and the guest call chain). The reader that fires right when the soldier starts walking is the code to hook. RECOMP_HOME_WATCH="<soldier off>:<off>:<len>"
 * (hex) = the pointer held at soldier+off, then +off inside it, len bytes (default C8:290:A0 = the per-soldier object whose +290/+2A0/+320 held the exact point
 * the soldier walked back to; the first run watched 257C:18C, the shared spawn point, and NOBODY read it). One address per run; released when the tracking ends. */
static int s_ed_hwatch;
static void ed_home_watch(uint32_t s)
{
    extern int xbox_WatchArmAt(uint32_t va, unsigned max_reports, int reads);
    const char *w = getenv("RECOMP_HOME_WATCH");
    uint32_t so = 0xC8u, off = 0x290u, len = 0xA0u, base, va;
    char *end, env[40];
    if (s_ed_hwatch || !w || !*w)               /* only when asked for: the watched page is slow */
        return;
    {
        so = (uint32_t)strtoul(w, &end, 16);
        if (*end == ':') {
            off = (uint32_t)strtoul(end + 1, &end, 16);
            if (*end == ':')
                len = (uint32_t)strtoul(end + 1, NULL, 16);
        }
    }
    if (len < 12u || len > 256u)
        len = 12u;
    base = MEM32(s + so);
    if (!ed_ptr(base) || base + off + len > 0x84000000u) {
        fprintf(stderr, "[HOME] read watch: soldier+%X = %08X is not a usable pointer\n", (unsigned)so, (unsigned)base);
        return;
    }
    va = base + off;
    snprintf(env, sizeof env, "RECOMP_WATCH_RLEN=%u", (unsigned)len);
    _putenv("RECOMP_WATCH_DISTINCT=1");
    _putenv(env);
    s_ed_hwatch = xbox_WatchArmAt(va, 0u, 1);
    fprintf(stderr, "[HOME] read+write watch on soldier+%X -> %08X +%X = %08X (%u bytes): %s (frame %u)\n", (unsigned)so, (unsigned)base, (unsigned)off, (unsigned)va,
            (unsigned)len, s_ed_hwatch ? "armed" : "NOT armed", (unsigned)g_game_frames);
}
/* THE SOLDIER'S HOME (build 36, from the build 35 log): each soldier has a goal object at soldier+0xC8, whose node at +0x280 holds the CURRENT goal triple at +0x290 and the
 * HOME triple at +0x2A0 (the exact point the soldier walked back to, ~1 m from where it was born). sub_000262B0 (recomp_0003.c) copies home -> goal for every node
 * of the group, so the soldier always returns to +0x2A0. Placed soldiers get our drop point written into both triples; nothing else is touched, so they keep
 * fighting and taking cover, and walk back to where they were dropped. Skipped when another living soldier points to the same object (it would move theirs) or
 * when +0x2A0 does not hold the birth point (some other kind of goal object). */
#define ED_NHOME 64
typedef struct { uint32_t s; float tgt[3], p0[3]; int state, tries, npair; uint32_t pair[8]; float orig[8][2]; uint32_t obj0; int c8warn, more; int wander, cover; uint32_t wnext, cnext, cuntil; float wc[3], wr, hplast; } EdHome;   /* pair[j] = (home triple address) - obj0, wrapping: nodes before obj0 have a "negative" offset */   /* pair[] = offsets in the +0xC8 object of "goal triple, home triple 0x10 later" */
static EdHome s_ed_home[ED_NHOME];
static int s_ed_nhome;
static void ed_home_add(const EdSlot *e)
{
    int i;
    EdHome *h;
    for (i = 0; i < s_ed_nhome; i++)
        if (s_ed_home[i].s == e->s)
            return;
    if (s_ed_nhome < ED_NHOME)
        h = &s_ed_home[s_ed_nhome++];
    else {                                      /* full: take the place of a soldier that is gone or dead, else the first one */
        int q, pick = 0;
        for (q = 0; q < ED_NHOME; q++)
            if (!ed_is_soldier(s_ed_home[q].s) || !ed_alive(s_ed_home[q].s)) {
                pick = q;
                break;
            }
        h = &s_ed_home[pick];
    }
    memset(h, 0, sizeof *h);
    h->s = e->s;
    memcpy(h->tgt, e->tgt, sizeof h->tgt);
    memcpy(h->p0, e->p0, sizeof h->p0);
}
/* debug only (advisor 2026-11-21): which soldier field leads to the home node? found = the node(s) the finder chose; otherwise the finder gave up and this lists
 * what the +0xC8 object (0x1000 bytes) holds near the birth point and which soldier fields point into it. */
static void ed_home_dump(const EdHome *h, uint32_t obj, int found)
{
    static const uint32_t k_via[] = { 0x2760u, 0x2C40u, 0xC8u, 0xB8u, 0xF0u, 0x245Cu, 0x2630u, 0x257Cu };
    uint32_t s = h->s, o, q, v, k;
    int j, n = 0;
    if (!s_ed_dbg)
        return;
    if (found) {
        for (j = 0; j < h->npair; j++) {
            uint32_t node = obj + h->pair[j] - 0x20u;
            fprintf(stderr, "[HOME] soldier %08X: node %08X (object %08X +%X) dword0 %08X dword4 %08X dword8 %08X dwordC %08X\n", (unsigned)s, (unsigned)node, (unsigned)obj,
                    (unsigned)(node - obj), (unsigned)MEM32(node), (unsigned)MEM32(node + 4u), (unsigned)MEM32(node + 8u), (unsigned)MEM32(node + 0xCu));
            for (o = 0; o < 0x2D00u; o += 4)
                if (MEM32(s + o) >= node && MEM32(s + o) < node + 0x100u)
                    fprintf(stderr, "[HOME]   soldier+%X -> node+%X\n", (unsigned)o, (unsigned)(MEM32(s + o) - node));
            for (k = 0; k < sizeof k_via / sizeof k_via[0]; k++) {
                uint32_t p = MEM32(s + k_via[k]);
                if (!ed_ptr(p) || p + 0x400u > 0x84000000u)
                    continue;
                for (q = 0; q < 0x400u; q += 4) {
                    v = MEM32(p + q);
                    if (v >= node && v < node + 0x100u)
                        fprintf(stderr, "[HOME]   soldier+%X -> %08X +%X -> node+%X\n", (unsigned)k_via[k], (unsigned)p, (unsigned)q, (unsigned)(v - node));
                }
            }
        }
        return;
    }
    if (obj + 0x1000u > 0x84000000u)
        return;
    for (o = 0; o + 12u <= 0x1000u && n < 12; o += 4)
        if (fabsf(ed_f(obj + o) - h->p0[0]) < 12.0f && fabsf(ed_f(obj + o + 4u) - h->p0[1]) < 12.0f && fabsf(ed_f(obj + o + 8u) - h->p0[2]) < 2.0f) {
            fprintf(stderr, "[HOME] soldier %08X: object %08X +%X holds %.2f %.2f %.2f (goal-0x10: %.2f %.2f)\n", (unsigned)s, (unsigned)obj, (unsigned)o, (double)ed_f(obj + o),
                    (double)ed_f(obj + o + 4u), (double)ed_f(obj + o + 8u), (double)ed_f(obj + o - 0x10u), (double)ed_f(obj + o - 0xCu));
            n++;
        }
    for (o = 0; o < 0x2D00u; o += 4)
        if (MEM32(s + o) >= obj && MEM32(s + o) < obj + 0x1000u)
            fprintf(stderr, "[HOME]   soldier+%X -> object+%X\n", (unsigned)o, (unsigned)(MEM32(s + o) - obj));
}
/* Build 41 test: some soldiers came home after 20-30 s although the node at +27C0 held the drop point. Each soldier's +0xC8 object holds a COPY of all the spawner's
 * spawn-point nodes (5 different homes seen for 5 dock workers, node stride 0x80, goal at +0x10 and home at +0x20 of a node); the soldier may pick another one later.
 * So every other node of its own object that looks like a spawn node (home near the birth point, goal already equal to home) gets the drop point too. The scan stays
 * inside the object (bounded by the next soldier's object) and is repeated for a few seconds because the nodes may fill in late. */
static void ed_home_more(EdHome *h, uint32_t node0)
{
    uint32_t o = MEM32(h->s + 0xC8u), len = 0x1000u, n;
    int j, k;
    if (!ed_ptr(o) || o + 0x100u > 0x84000000u)
        return;
    for (j = 0; j < s_ed_nsol; j++) {
        uint32_t o2 = s_ed_sol[j] && s_ed_sol[j] != h->s ? MEM32(s_ed_sol[j] + 0xC8u) : 0u;
        if (ed_ptr(o2) && o2 > o && o2 - o < len)
            len = o2 - o;
    }
    if (o + len > 0x84000000u)
        len = 0x84000000u - o;
    for (n = o + ((node0 - o) & 0x7Fu); n + 0x40u <= o + len && h->npair < 8; n += 0x80u) {   /* same 0x80 grid as the +27C0 node */
        float hx, hy, hz, d;
        uint32_t home = n + 0x20u;
        if (n < o || home == node0 + 0x20u)
            continue;
        for (k = 0; k < h->npair && h->obj0 + h->pair[k] != home; k++)
            ;
        if (k < h->npair)
            continue;
        hx = ed_f(home); hy = ed_f(home + 4u); hz = ed_f(home + 8u);
        d = sqrtf((hx - h->p0[0]) * (hx - h->p0[0]) + (hy - h->p0[1]) * (hy - h->p0[1]));
        if (d < 40.0f && fabsf(hz - h->p0[2]) < 2.5f && fabsf(ed_f(n + 0x10u) - hx) < 0.1f && fabsf(ed_f(n + 0x14u) - hy) < 0.1f && fabsf(ed_f(n + 0x18u) - hz) < 0.1f) {
            h->orig[h->npair][0] = hx;
            h->orig[h->npair][1] = hy;
            h->pair[h->npair++] = home - h->obj0;
            if (s_ed_dbg)
                fprintf(stderr, "[HOME] soldier %08X: another spawn node %08X (+%X in its object) home %.2f %.2f %.2f also set\n", (unsigned)h->s, (unsigned)n, (unsigned)(n - o), (double)hx,
                        (double)hy, (double)hz);
        }
    }
}
/* a world ray from the player's eye towards a point, starting 0.6 m out so it does not begin inside the player. Returns the fraction, -1 on error. */
static float ed_cover_ray(const float *pe, const float *tgt, uint32_t *ob)
{
    float d[3], len, st[3];
    int i;
    for (i = 0; i < 3; i++)
        d[i] = tgt[i] - pe[i];
    len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1.5f)
        return -1.0f;
    for (i = 0; i < 3; i++)
        st[i] = pe[i] + d[i] / len * 0.6f;
    return ed_cast(st, tgt, 0x4006u, ob);
}
static void ed_home_pin(void)
{
    int i, j, c;
    if (!s_ed_nhome || (g_game_frames & 1u))
        return;
    for (i = 0; i < s_ed_nhome; i++) {
        EdHome *h = &s_ed_home[i];
        uint32_t obj;
        if (!ed_alive(h->s)) {
            *h = s_ed_home[--s_ed_nhome];
            i--;
            continue;
        }
        if (h->state == 2)
            continue;
        /* build 40 log: in all 11 soldiers whose node was found, soldier+0x27C0 holds the node's address (node+0x10 = goal triple, node+0x20 = home triple), whatever the
         * offset inside the +0xC8 object. That is the fixed link: no scan. Once found, the absolute node address is kept (h->obj0 = the node). */
        obj = h->state == 1 ? h->obj0 : MEM32(h->s + 0x27C0u);
        if (!ed_ptr(obj) || obj + 0x40u > 0x84000000u)
            continue;
        if (h->state == 1 && MEM32(h->s + 0x27C0u) != obj && !h->c8warn) {
            h->c8warn = 1;
            if (s_ed_dbg)
                fprintf(stderr, "[HOME] soldier %08X: +27C0 changed %08X -> %08X (frame %u)\n", (unsigned)h->s, (unsigned)obj, (unsigned)MEM32(h->s + 0x27C0u), (unsigned)g_game_frames);
        }
        if (h->state == 0) {
            int bad = 0;
            float hx = ed_f(obj + 0x20u), hy = ed_f(obj + 0x24u), d = sqrtf((hx - h->p0[0]) * (hx - h->p0[0]) + (hy - h->p0[1]) * (hy - h->p0[1]));
            if (!(d < 40.0f)) {                 /* not filled yet at birth (zero / garbage): wait, then give up */
                if (++h->tries > 1500) {
                    h->state = 2;
                    if (s_ed_dbg) {
                        fprintf(stderr, "[HOME] soldier %08X: node %08X (from +27C0) holds %.2f %.2f at +20, not a home near the birth point: left alone\n", (unsigned)h->s, (unsigned)obj,
                                (double)hx, (double)hy);
                        ed_home_dump(h, MEM32(h->s + 0xC8u), 0);
                    }
                }
                continue;
            }
            for (j = 0; j < s_ed_nsol; j++)     /* another living soldier on the same node: its home would move too */
                if (s_ed_sol[j] && s_ed_sol[j] != h->s && ed_alive(s_ed_sol[j]) && MEM32(s_ed_sol[j] + 0x27C0u) == obj)
                    bad = 1;
            if (bad) {
                h->state = 2;
                if (s_ed_dbg)
                    fprintf(stderr, "[HOME] soldier %08X: node %08X is shared with another soldier: left alone\n", (unsigned)h->s, (unsigned)obj);
                continue;
            }
            h->state = 1;
            h->wnext = g_game_frames + 420u + (uint32_t)(ed_zrand() * 480.0f);
            h->obj0 = obj;
            h->npair = 1;
            h->pair[0] = 0x20u;
            h->orig[0][0] = hx;
            h->orig[0][1] = hy;
            if (s_ed_dbg)
                fprintf(stderr, "[HOME] soldier %08X: node %08X (+27C0): home was %.2f %.2f %.2f (%.1f m from birth), set to the drop point %.2f %.2f %.2f\n", (unsigned)h->s, (unsigned)obj,
                        (double)hx, (double)hy, (double)ed_f(obj + 0x28u), (double)d, (double)h->tgt[0], (double)h->tgt[1], (double)h->tgt[2]);
        }
        if (h->more < 1500) {                   /* the first ~25-50 s: look for the soldier's other spawn nodes (every 5th pin call) */
            if ((h->more++ % 5) == 0)
                ed_home_more(h, obj);
        }
        if (h->state == 1 && s_ed_nzn > 0 && !h->wander && !h->cover && (g_game_frames & 7u) == 0) {      /* is this a zone soldier, and what may we do with it? */
            int cv = 0;
            if (ed_zone_wander_of(h->s, h->wc, &h->wr, &cv)) {
                uint32_t pcx = game_pc_get();
                (void)pcx;
                h->wander = 1;
                h->cover = cv;
                h->hplast = ed_f(h->s + 0x2C60u + 0x9Cu);
                h->cnext = g_game_frames + 120u;
            }
        }
        if (h->state == 1 && h->cover && g_game_frames >= h->cnext) {
            static uint32_t one_per_frame;
            if (one_per_frame != g_game_frames) {      /* one soldier's search per frame (up to ~50 world rays) */
                float hp = ed_f(h->s + 0x2C60u + 0x9Cu), sp0[3], pe[3], ch[3], dn[3] = { 0.0f, 0.0f, -1.0f };
                uint32_t pc = game_pc_get(), ob;
                int hurt = hp == hp && hp < h->hplast - 0.01f, exposed = 0;
                one_per_frame = g_game_frames;
                h->cnext = g_game_frames + 180u;
                h->hplast = hp;
                sp0[0] = ed_f(h->s + 0x60u); sp0[1] = ed_f(h->s + 0x64u); sp0[2] = ed_f(h->s + 0x68u);
                pe[0] = ed_f(pc + 0x90u); pe[1] = ed_f(pc + 0x94u); pe[2] = ed_f(pc + 0x98u) + 1.6f;
                if (pc >= 0x80000000u && fabsf(pe[0]) + fabsf(pe[1]) > 0.01f) {
                    float dx = pe[0] - sp0[0], dy = pe[1] - sp0[1];
                    if (dx * dx + dy * dy < 30.0f * 30.0f) {        /* the player is near: does the player see this soldier? */
                        float ct[3], f;
                        ct[0] = sp0[0]; ct[1] = sp0[1]; ct[2] = sp0[2] + 1.0f;
                        f = ed_cover_ray(pe, ct, &ob);
                        exposed = f >= 0.0f && (f > 0.96f || (ed_ptr(ob) && MEM32(ob) == 0x317008u));
                    }
                    if ((hurt || exposed) && g_game_frames >= h->cuntil) {
                        int ring, a, best = -1, tested = 0;
                        float bp[3] = { 0, 0, 0 }, bd = 1e9f;
                        float base = ed_zrand() * 6.2831853f;
                        for (ring = 0; ring < 4 && best < 0; ring++) {
                            for (a = 0; a < 4; a++) {
                                float ang = base + (float)a * 1.5707963f + (float)ring * 0.7f, rr = 4.0f + 4.0f * (float)ring, np[3], from[3], hit[3], hp3[3], f2, d, ox, oy;
                                int k, near_other = 0;
                                np[0] = sp0[0] + cosf(ang) * rr;
                                np[1] = sp0[1] + sinf(ang) * rr;
                                from[0] = np[0]; from[1] = np[1]; from[2] = sp0[2] + 1.5f;
                                if (!ed_world_point(from, dn, hit) || fabsf(hit[2] - sp0[2]) > 2.0f)
                                    continue;
                                np[2] = hit[2] + 0.3f;
                                tested++;
                                hp3[0] = np[0]; hp3[1] = np[1]; hp3[2] = hit[2] + 1.0f;
                                f2 = ed_cover_ray(pe, hp3, &ob);
                                if (!(f2 > 0.001f && f2 < 0.95f && !(ed_ptr(ob) && MEM32(ob) == 0x317008u)))
                                    continue;                       /* the player can see that spot (or the ray hit a soldier): not cover */
                                for (k = 0; k < s_ed_nhome; k++) {  /* spread out: not within 3 m of the cover another soldier took */
                                    if (&s_ed_home[k] == h || !s_ed_home[k].cover || g_game_frames >= s_ed_home[k].cuntil)
                                        continue;
                                    ox = s_ed_home[k].tgt[0] - np[0];
                                    oy = s_ed_home[k].tgt[1] - np[1];
                                    if (ox * ox + oy * oy < 9.0f)
                                        near_other = 1;
                                }
                                if (near_other)
                                    continue;
                                d = rr;
                                if (d < bd) {
                                    bd = d;
                                    bp[0] = np[0]; bp[1] = np[1]; bp[2] = np[2];
                                    best = 1;
                                }
                            }
                        }
                        ch[0] = bp[0]; ch[1] = bp[1]; ch[2] = bp[2];
                        if (best >= 0) {
                            int j2;
                            for (j2 = 0; j2 < h->npair; j2++) {
                                uint32_t a2 = obj + h->pair[j2];
                                int c2;
                                if (!((fabsf(ed_f(a2) - h->orig[j2][0]) < 0.1f && fabsf(ed_f(a2 + 4u) - h->orig[j2][1]) < 0.1f) || (fabsf(ed_f(a2) - h->tgt[0]) < 0.1f && fabsf(ed_f(a2 + 4u) - h->tgt[1]) < 0.1f)))
                                    continue;
                                for (c2 = 0; c2 < 3; c2++) {
                                    ed_setf(a2 + 4u * (uint32_t)c2, ch[c2]);
                                    if (fabsf(ed_f(a2 - 0x10u) - h->orig[j2][0]) < 0.1f || fabsf(ed_f(a2 - 0x10u) - h->tgt[0]) < 0.1f)
                                        ed_setf(a2 - 0x10u + 4u * (uint32_t)c2, ch[c2]);
                                }
                            }
                            h->tgt[0] = ch[0]; h->tgt[1] = ch[1]; h->tgt[2] = ch[2];
                            h->cuntil = g_game_frames + 1500u;      /* no wandering for ~25 s */
                            fprintf(stderr, "[COVER] soldier %08X (%s) goes to cover %.1f %.1f %.1f, %.1f m away (%d spots tested)\n", (unsigned)h->s, hurt ? "hurt" : "seen by the player",
                                    (double)ch[0], (double)ch[1], (double)ch[2], (double)bd, tested);
                        } else {
                            fprintf(stderr, "[COVER] soldier %08X (%s): no hidden spot within 16 m (%d spots tested)\n", (unsigned)h->s, hurt ? "hurt" : "seen by the player", tested);
                        }
                    }
                }
            }
        }
        if (h->state == 1 && h->wander && s_ed_nzn > 0 && g_game_frames >= h->wnext) {          /* zone soldiers wander: every 7-15 s a new home point inside the circle (the game walks an idle soldier home) */
            h->wnext = g_game_frames + 420u + (uint32_t)(ed_zrand() * 480.0f);
            if (g_game_frames >= h->cuntil && s_ed_zn_wander_on(h->s)) {
                float ang = ed_zrand() * 6.2831853f, rad = h->wr * sqrtf(ed_zrand()), np[3], from[3], dn[3] = { 0.0f, 0.0f, -1.0f }, hit[3];
                np[0] = h->wc[0] + cosf(ang) * rad;
                np[1] = h->wc[1] + sinf(ang) * rad;
                np[2] = h->tgt[2];
                from[0] = np[0]; from[1] = np[1]; from[2] = h->wc[2] + 1.5f;
                if (ed_world_point(from, dn, hit))
                    np[2] = hit[2] + 0.3f;
                for (j = 0; j < h->npair; j++) {
                    uint32_t a = obj + h->pair[j];
                    if (!((fabsf(ed_f(a) - h->orig[j][0]) < 0.1f && fabsf(ed_f(a + 4u) - h->orig[j][1]) < 0.1f) || (fabsf(ed_f(a) - h->tgt[0]) < 0.1f && fabsf(ed_f(a + 4u) - h->tgt[1]) < 0.1f)))
                        continue;
                    for (c = 0; c < 3; c++) {
                        ed_setf(a + 4u * (uint32_t)c, np[c]);
                        if (fabsf(ed_f(a - 0x10u) - h->orig[j][0]) < 0.1f || fabsf(ed_f(a - 0x10u) - h->tgt[0]) < 0.1f)
                            ed_setf(a - 0x10u + 4u * (uint32_t)c, np[c]);
                    }
                }
                if (s_ed_dbg)
                    fprintf(stderr, "[ZONE] soldier %08X wanders to %.1f %.1f %.1f\n", (unsigned)h->s, (double)np[0], (double)np[1], (double)np[2]);
                h->tgt[0] = np[0]; h->tgt[1] = np[1]; h->tgt[2] = np[2];
            }
        }
        for (j = 0; j < h->npair; j++) {
            uint32_t a = obj + h->pair[j];
            /* only while the triple still is the spawn goal (near the birth point or already ours): a goal the game sets for its own reasons stays */
            if (!((fabsf(ed_f(a) - h->orig[j][0]) < 0.1f && fabsf(ed_f(a + 4u) - h->orig[j][1]) < 0.1f) || (fabsf(ed_f(a) - h->tgt[0]) < 0.1f && fabsf(ed_f(a + 4u) - h->tgt[1]) < 0.1f)))
                continue;
            for (c = 0; c < 3; c++) {
                ed_setf(a + 4u * (uint32_t)c, h->tgt[c]);               /* home */
                if (fabsf(ed_f(a - 0x10u) - h->orig[j][0]) < 0.1f || fabsf(ed_f(a - 0x10u) - h->tgt[0]) < 0.1f)
                    ed_setf(a - 0x10u + 4u * (uint32_t)c, h->tgt[c]);   /* the current goal, when it is the spawn goal too (the game copies home -> goal every frame anyway) */
            }
        }
    }
}
static void ed_trk_start(const EdSlot *e)
{
    EdTrk *t;
    int i;
    if (!s_ed_dbg || !e->found || s_ed_ntrk >= 6)
        return;
    for (i = 0; i < s_ed_ntrk; i++)
        if (s_ed_trk[i].s == e->s)
            return;
    t = &s_ed_trk[s_ed_ntrk++];
    memset(t, 0, sizeof *t);
    t->s = e->s;
    t->h25 = e->h25 == 1 ? ed_h25(e->s) : 0;
    memcpy(t->p0, e->p0, sizeof t->p0);
    t->last[0] = ed_f(e->s + 0x60); t->last[1] = ed_f(e->s + 0x64); t->last[2] = ed_f(e->s + 0x68);
    t->next = g_game_frames + 60u;
    fprintf(stderr, "[HOME] soldier %08X: born at %.2f %.2f %.2f, placed at %.2f %.2f %.2f (team %u), now free (frame %u)\n", (unsigned)e->s, (double)e->p0[0], (double)e->p0[1],
            (double)e->p0[2], (double)e->tgt[0], (double)e->tgt[1], (double)e->tgt[2], (unsigned)MEM32(e->s + 0xD8), (unsigned)g_game_frames);
    ed_scan_near("near birth point", e->s, e->p0, 0.5f, &t->lines);
    ed_home_watch(e->s);
}
static void ed_trk_tick(void)
{
    int i;
    {
        extern uint32_t g_watch_tag;
        g_watch_tag = (uint32_t)g_game_frames;  /* the read watch prints it with each new reader */
    }
    for (i = 0; i < s_ed_ntrk; i++) {
        EdTrk *t = &s_ed_trk[i];
        float p[3], moved;
        if (g_game_frames < t->next)
            continue;
        t->next = g_game_frames + 60u;
        if (!ed_alive(t->s) || ++t->logged > 100) {
            fprintf(stderr, "[HOME] soldier %08X: tracking ends (%s)\n", (unsigned)t->s, ed_alive(t->s) ? "time" : "dead or gone");
            *t = s_ed_trk[--s_ed_ntrk];
            i--;
            if (!s_ed_ntrk && s_ed_hwatch) {
                extern void xbox_WatchDisarm(void);
                xbox_WatchDisarm();             /* prints the per-reader counts */
                s_ed_hwatch = 0;
            }
            continue;
        }
        p[0] = ed_f(t->s + 0x60); p[1] = ed_f(t->s + 0x64); p[2] = ed_f(t->s + 0x68);
        moved = fabsf(p[0] - t->last[0]) + fabsf(p[1] - t->last[1]);
        fprintf(stderr, "[HOME] frame %u soldier %08X at %.2f %.2f %.2f (moved %.2f in 1 s, %.1f m from birth)\n", (unsigned)g_game_frames, (unsigned)t->s, (double)p[0], (double)p[1], (double)p[2], (double)moved,
                (double)sqrtf((p[0] - t->p0[0]) * (p[0] - t->p0[0]) + (p[1] - t->p0[1]) * (p[1] - t->p0[1])));
        if (t->h25)
            fprintf(stderr, "[HOME]   soldier %08X +25C4 field holds %.2f %.2f %.2f\n", (unsigned)t->s, (double)ed_f(t->h25), (double)ed_f(t->h25 + 4), (double)ed_f(t->h25 + 8));
        if (moved > 1.0f && t->goalscans < 3) {   /* walking: does the soldier itself hold the goal (the birth point / the spawn point, a few metres wide)? */
            int ln = 0;
            t->goalscans++;
            ed_scan_near("GOAL while walking (near birth point, 3 m)", t->s, t->p0, 3.0f, &ln);
        }
        if (moved < 0.3f && (fabsf(p[0] - t->p0[0]) > 1.0f || fabsf(p[1] - t->p0[1]) > 1.0f) && !t->stable) {
            t->stable = 1;                      /* stopped somewhere other than where it was dropped: that is D */
            ed_scan_near("near the stop point", t->s, p, 0.5f, &t->lines);
        } else if (moved >= 0.3f) {
            t->stable = 0;
        }
        t->last[0] = p[0]; t->last[1] = p[1]; t->last[2] = p[2];
    }
}
static void ed_slots(void)
{
    int i, fc = game_freecam_active(), any = 0;
    for (i = 0; i < ED_NSLOT; i++) {
        EdSlot *e = &s_ed_slot[i];
        if (!e->s)
            continue;
        any = 1;
        if (!ed_is_soldier(e->s) || e->left <= 0) {
            if (e->left <= 0 && s_ed_dbg) {
                fprintf(stderr, "[EDITOR] %08X released at %.2f %.2f %.2f\n", (unsigned)e->s, (double)ed_f(e->s + 0x60), (double)ed_f(e->s + 0x64),
                        (double)ed_f(e->s + 0x68));
                if (ed_is_soldier(e->s))
                    ed_trk_start(e);
            }
            e->s = 0;
            continue;
        }
        if (!e->found) {                        /* the constructor has not given it a real position yet */
            if (fabsf(ed_f(e->s + 0x60)) > 1.0f || fabsf(ed_f(e->s + 0x64)) > 1.0f) {
                e->found = 1;
                e->p0[0] = ed_f(e->s + 0x60); e->p0[1] = ed_f(e->s + 0x64); e->p0[2] = ed_f(e->s + 0x68);
                ed_home_add(e);
                if (s_ed_dbg) {                 /* before our first write: who holds the birth point? (once per soldier, debug only) */
                    int lines = 0;
                    fprintf(stderr, "[HOME] soldier %08X born at %.2f %.2f %.2f from spawner %08X: who holds that point?\n", (unsigned)e->s, (double)e->p0[0], (double)e->p0[1],
                            (double)e->p0[2], (unsigned)e->sp);
                    fprintf(stderr, "[HOME] soldier %08X team %u: +245C %08X +257C %08X +25C4 %08X +14B4 %08X\n", (unsigned)e->s, (unsigned)MEM32(e->s + 0xD8), (unsigned)MEM32(e->s + 0x245C),
                            (unsigned)MEM32(e->s + 0x257C), (unsigned)MEM32(e->s + 0x25C4), (unsigned)MEM32(e->s + 0x14B4));
                    ed_scan_deep("soldier", e->s, 0x2D00u, e->p0, 0.6f, &lines);
                    lines = 0;
                    if (ed_ptr(e->sp))
                        ed_scan_deep("spawner", e->sp, 0x80u, e->p0, 0.6f, &lines);
                }
                if (e->swapped) {               /* born at the target (ed_spawn): nothing to move; a short fallback pin only when the birth missed it by metres */
                    float dx = e->p0[0] - e->tgt[0], dy = e->p0[1] - e->tgt[1];
                    if (s_ed_dbg)
                        fprintf(stderr, "[HOME] soldier %08X born %.1f m from its target (swap worked: %s)\n", (unsigned)e->s, (double)sqrtf(dx * dx + dy * dy),
                                dx * dx + dy * dy < 4.0f ? "yes" : "NO");
                    e->left = dx * dx + dy * dy < 4.0f ? 0 : 10;
                    if (e->left == 0)
                        e->nhold = 0;
                    else
                        ed_find_holders(e);
                } else {
                    ed_find_holders(e);
                }
            } else {
                if (--e->left < -400)
                    e->s = 0;
                continue;
            }
        }
        if (e->left <= 0) {                     /* born in place: released at once (the loop top logs it and starts the home tracking) */
            if (s_ed_dbg && ed_is_soldier(e->s))
                ed_trk_start(e);
            e->s = 0;
            continue;
        }
        ed_put(e);
        if (e->manual && fc && !e->swapped)
            e->left = 90;                       /* old way (the spawn point could not be swapped): keep it there for as long as the free camera is flying */
        else
            e->left--;
    }
    if (s_ed_ntrk)
        ed_trk_tick();
    ed_home_pin();
    s_ed_anyslot = any || s_ed_ntrk > 0 || s_ed_nhome > 0;
}

/* ---------- the saved list ---------- */
static void ed_path(char *out, size_t n)
{
    snprintf(out, n, "mods\\maps\\%s.txt", s_ed_level[0] ? s_ed_level : "unknown");
}
static void ed_save(void)
{
    char p[64];
    FILE *f;
    int i;
    CreateDirectoryA("mods", NULL);
    CreateDirectoryA("mods\\maps", NULL);
    ed_path(p, sizeof p);
    if (s_ed_npl == 0 && s_ed_npp == 0 && s_ed_nzn == 0) {
        DeleteFileA(p);
        s_ed_apply = 1;                         /* an empty level has nothing to switch off */
        return;
    }
    f = fopen(p, "w");
    if (!f)
        return;
    fprintf(f, "# map editor layout of level %s. soldier <team 2 enemy | 1 friend> <x> <y> <z> <type file or ->   prop <x> <y> <z> <definition path>   zone <x> <y> <z> <radius> <soldiers per wave> <waves> <trigger distance> <wander 0/1> <cover 0/1> <type file>   enabled <0 | 1> (0 = the level starts as the game made it; this file is kept)\n", s_ed_level);
    if (!s_ed_apply)
        fprintf(f, "enabled 0\n");
    for (i = 0; i < s_ed_npl; i++)
        fprintf(f, "soldier %d %.3f %.3f %.3f %s\n", s_ed_pl[i].team, (double)s_ed_pl[i].p[0], (double)s_ed_pl[i].p[1], (double)s_ed_pl[i].p[2],
                s_ed_pl[i].name[0] ? s_ed_pl[i].name : "-");
    for (i = 0; i < s_ed_npp; i++)
        fprintf(f, "prop %.3f %.3f %.3f %s\n", (double)s_ed_pp[i].p[0], (double)s_ed_pp[i].p[1], (double)s_ed_pp[i].p[2], s_ed_pp[i].path);
    for (i = 0; i < s_ed_nzn; i++)
        fprintf(f, "zone %.3f %.3f %.3f %.2f %d %d %.1f %d %d %s\n", (double)s_ed_zn[i].p[0], (double)s_ed_zn[i].p[1], (double)s_ed_zn[i].p[2], (double)s_ed_zn[i].r,
                s_ed_zn[i].count, s_ed_zn[i].waves, (double)s_ed_zn[i].trig, s_ed_zn[i].wander, s_ed_zn[i].cover, s_ed_zn[i].name);
    fclose(f);
}
static void ed_load(void)
{
    char p[64], line[256];
    FILE *f;
    s_ed_npl = 0;
    s_ed_npp = 0;
    s_ed_nzn = 0;
    s_ed_apply = 1;
    ed_path(p, sizeof p);
    f = fopen(p, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f) && s_ed_npl < ED_NPL) {
        EdPlace *e = &s_ed_pl[s_ed_npl];
        char nm[ED_PATH] = "";
        if (line[0] == '#')
            continue;
        if (!strncmp(line, "enabled ", 8)) {
            s_ed_apply = atoi(line + 8) != 0;
            continue;
        }
        if (!strncmp(line, "zone ", 5)) {
            EdZone *z = &s_ed_zn[s_ed_nzn];
            int got = s_ed_nzn < ED_NZN ? sscanf(line, "zone %f %f %f %f %d %d %f %d %d %159s", &z->p[0], &z->p[1], &z->p[2], &z->r, &z->count, &z->waves, &z->trig, &z->wander, &z->cover, z->name) : 0;
            if (got == 10) {
                got = 9;
            } else if (got == 8) {                      /* "... wander type": the 9th token was the type file */
                got = sscanf(line, "zone %f %f %f %f %d %d %f %d %159s", &z->p[0], &z->p[1], &z->p[2], &z->r, &z->count, &z->waves, &z->trig, &z->wander, z->name) == 9 ? 9 : 0;
                z->cover = 1;
            }
            if (got == 7) {                             /* an older line without the wander flag: the 8th token is the type file */
                got = sscanf(line, "zone %f %f %f %f %d %d %f %159s", &z->p[0], &z->p[1], &z->p[2], &z->r, &z->count, &z->waves, &z->trig, z->name) == 8 ? 9 : 0;
                z->wander = 1;
                z->cover = 1;
            }
            if (got == 9 && z->count >= 1 && z->count <= ED_ZMAX && z->waves >= 1 && z->waves <= 9 && z->r >= 1.0f && z->r <= 60.0f)
                s_ed_nzn++;
            else
                fprintf(stderr, "[EDITOR] bad or surplus zone line in %s: %.60s\n", p, line);
        } else if (!strncmp(line, "prop ", 5)) {
            EdProp *pp = &s_ed_pp[s_ed_npp];
            if (s_ed_npp < ED_NPP && sscanf(line, "prop %f %f %f %159s", &pp->p[0], &pp->p[1], &pp->p[2], pp->path) == 4)
                s_ed_npp++;
            else
                fprintf(stderr, "[EDITOR] bad or surplus prop line in %s: %.60s\n", p, line);
        } else if (sscanf(line, "soldier %d %f %f %f %159s", &e->team, &e->p[0], &e->p[1], &e->p[2], nm) >= 4 && (e->team == 1 || e->team == 2)) {
            strcpy(e->name, strcmp(nm, "-") ? nm : "");
            s_ed_npl++;
        } else {
            fprintf(stderr, "[EDITOR] bad line in %s: %.60s\n", p, line);
        }
    }
    fclose(f);
    if (s_ed_npl > ed_cap()) {                  /* a longer file (made before the limit): keep it as .full, use the first lines */
        char bak[80];
        snprintf(bak, sizeof bak, "%s.full", p);
        CopyFileA(p, bak, TRUE);
        fprintf(stderr, "[EDITOR] %s has %d lines, more than the limit %d: using the first %d (the whole file is kept as %s)\n", p, s_ed_npl, ed_cap(),
                ed_cap(), bak);
        s_ed_npl = ed_cap();
    }
}

static uint32_t s_ed_pc_seen;                   /* the player object of the run the editor state belongs to */
static int s_ed_hud_seen;                       /* the mission HUD was built once since the level load (a second build in the same level = a restart) */
/* the world is new (a level load, or a mission restart): the lists of the old world are dead */
static void ed_world_reset(void)
{
    memset(s_ed_slot, 0, sizeof s_ed_slot);
    s_ed_nhome = 0;
    s_ed_nsp = s_ed_nsol = 0;
    s_ed_nplaced = 0;
    s_ed_anyslot = 0;
    s_ed_last_sol = 0;
    memset(s_ed_pl_sol, 0, sizeof s_ed_pl_sol);
    s_ed_ncat = 0;
    s_ed_sel = -1;
    s_ed_pc_seen = 0;
    s_ed_hud_seen = 0;
    s_ed_selprop[0] = 0;
    s_ed_pr = 0;
    memset(s_ed_pp_obj, 0, sizeof s_ed_pp_obj);
    ed_zone_reset_runtime();
}
/* is this object still in the world's creation list (0x3A0338, node + 0x18 = the object, node + 0xC = the next node)? */
static int ed_in_world(uint32_t o)
{
    uint32_t n = MEM32(0x3A0338);
    int guard = 0;
    while (n >= 0x10000u && n < 0x90000000u && guard++ < 16384) {
        if (MEM32(n + 0x18) == o)
            return 1;
        n = MEM32(n + 0xC);
    }
    return 0;
}
/* A new world whose spawners have already registered (a mission restart): what belongs to the dead world goes, the spawners and the catalog
 * stay (the rebuilt spawners ask for their types before the HUD is built, and may sit at the old addresses). */
static void ed_soft_reset(void)
{
    int i, j = 0;
    memset(s_ed_slot, 0, sizeof s_ed_slot);
    s_ed_nhome = 0;
    s_ed_nplaced = 0;
    s_ed_anyslot = 0;
    s_ed_last_sol = 0;
    memset(s_ed_pl_sol, 0, sizeof s_ed_pl_sol);
    memset(s_ed_pp_obj, 0, sizeof s_ed_pp_obj);     /* the old world's pickups are gone */
    ed_zone_reset_runtime();
    for (i = 0; i < s_ed_nsol; i++)             /* soldiers the old world made: their memory may be anything now */
        if (s_ed_sol[i] && !ed_in_world(s_ed_sol[i]))
            s_ed_sol[i] = 0;
    for (i = 0; i < s_ed_nsp; i++)              /* spawners that are gone (the spawner table has only so many places) */
        if (MEM32(s_ed_sp[i]) == 0x30ED54u) {
            if (i != j) {
                s_ed_sp[j] = s_ed_sp[i];
                memcpy(s_ed_arg[j], s_ed_arg[i], sizeof s_ed_arg[j]);
                memcpy(s_ed_name[j], s_ed_name[i], sizeof s_ed_name[j]);
            }
            j++;
        }
    s_ed_nsp = j;
}
/* the saved layout (already in s_ed_pl) is to be placed again once the new world runs */
static void ed_rearm(void)
{
    s_ed_t0 = 0;
    s_ed_replay = s_ed_npl > 0 && s_ed_apply ? 1 : 0;
    s_ed_replay_i = 0;
    s_ed_gap = 0;
    s_ed_tries = 0;
    s_ed_pr = s_ed_npp > 0 && s_ed_apply ? 1 : 0;
    s_ed_pr_i = 0;
    s_ed_pr_t0 = 0;
    s_ed_pr_tries = 0;
}
/* a level starts loading (rs_open_hook in recomp_settings.c, path lower-cased; loading.viv or level_a.str of the level's folder) */
void game_editor_levelload(const char *lowpath)
{
    char id[24];
    const char *e = strstr(lowpath, "\\level_a.str"), *b;
    size_t n;
    (void)ed_on();
    if (!e)
        e = strstr(lowpath, "\\loading.viv");   /* a mission restart may open only this one: the layout must come back then too */
    ed_world_reset();
    if (strstr(lowpath, "\\level_a.str")) {
        s_pt_resn = 0;
        s_pt_probed = 0;
        s_pt_pt0 = 0;
        s_ptpl_n = 0;                          /* a new level: the templates hold pointers into the old level's data. (A restart may keep or rebuild the nodes: the templates are then refreshed by the nodes themselves.) */
    }
    if (!e)
        return;
    for (b = e; b > lowpath && b[-1] != '\\'; b--)
        ;
    n = (size_t)(e - b);
    if (n == 0 || n >= sizeof id)
        return;
    memcpy(id, b, n);
    id[n] = 0;
    strcpy(s_ed_level, id);
    ed_load();
    ed_rearm();
    fprintf(stderr, "[EDITOR] level %s: %d saved soldiers (opened %s)\n", s_ed_level, s_ed_npl, e + 1);
}

/* ---------- making a soldier ---------- */
static void ed_call_spawner(uint32_t sp, int slot, const char *path)
{
    uint32_t s_eax = g_eax, s_ecx = g_ecx, s_edx = g_edx, s_ebx = g_ebx, s_esi = g_esi, s_edi = g_edi, s_ebp = g_ebp, s_sehebp = g_seh_ebp, s_esp = esp;
    RecompXmm s_x[8];
    double s_fps[8];
    int s_fpt = g_fp_top, k, a;
    s_x[0] = g_xmm0; s_x[1] = g_xmm1; s_x[2] = g_xmm2; s_x[3] = g_xmm3; s_x[4] = g_xmm4; s_x[5] = g_xmm5; s_x[6] = g_xmm6; s_x[7] = g_xmm7;
    for (k = 0; k < 8; k++)
        s_fps[k] = g_fp_stack[k];
    esp = ((esp - 0x400u) & ~0xFu);
    s_ed_name_ptr = 0;
    if (path && path[0]) {                      /* the wanted type file lives on the stack above the callee's frames for the whole (synchronous) call */
        size_t n = strlen(path);
        if (n > ED_PATH - 1)
            n = ED_PATH - 1;
        esp -= 0xB0u;                           /* 16-byte aligned, room for ED_PATH + terminator */
        for (k = 0; (size_t)k < n; k++)
            MEM8(esp + (uint32_t)k) = (uint8_t)path[k];
        MEM8(esp + (uint32_t)n) = 0;
        s_ed_name_ptr = esp;
    }
    for (a = 7; a >= 0; a--)                    /* the arguments the game itself passed last time (argument 1 = this spawner's interface) */
        PUSH32(esp, a == 0 ? sp + 8u : slot >= 0 ? s_ed_arg[slot][a] : 0u);
    PUSH32(esp, 0x0018A000u);                   /* return address, only read by logging hooks */
    s_ed_cur_sp = sp;
    s_ed_self_call = 1;
    RECOMP_ABI_CALL(0x000A1470u, sub_000A1470);
    s_ed_self_call = 0;
    s_ed_name_ptr = 0;
    g_eax = s_eax; g_ecx = s_ecx; g_edx = s_edx; g_ebx = s_ebx; g_esi = s_esi; g_edi = s_edi;
    g_ebp = s_ebp; g_seh_ebp = s_sehebp; esp = s_esp;
    g_xmm0 = s_x[0]; g_xmm1 = s_x[1]; g_xmm2 = s_x[2]; g_xmm3 = s_x[3]; g_xmm4 = s_x[4]; g_xmm5 = s_x[5]; g_xmm6 = s_x[6]; g_xmm7 = s_x[7];
    for (k = 0; k < 8; k++)
        g_fp_stack[k] = s_fps[k];
    g_fp_top = s_fpt;
}

/* remember a soldier of ours. A full table first drops entries whose object is no longer a soldier, then takes the place of a dead one; a living one is never
 * overwritten (it would lose its dropped kill / refund messages and the zone code would think it was gone) */
static void ed_placed_add(uint32_t self)
{
    int k;
    if (s_ed_nplaced >= ED_NPLACED)
        for (k = 0; k < s_ed_nplaced;) {
            if (!ed_is_soldier(s_ed_placed[k]))
                s_ed_placed[k] = s_ed_placed[--s_ed_nplaced];
            else
                k++;
        }
    if (s_ed_nplaced >= ED_NPLACED) {
        for (k = 0; k < s_ed_nplaced; k++)
            if (!ed_alive(s_ed_placed[k])) {
                s_ed_placed[k] = self;
                return;
            }
        fprintf(stderr, "[EDITOR] the table of placed soldiers is full of living ones: %08X not remembered\n", (unsigned)self);
        return;
    }
    s_ed_placed[s_ed_nplaced++] = self;
}
/* the constructor hook: inside our call, the new soldier is the one to place */
static void ed_ctor_seen(uint32_t self)
{
    int i;
    if (!s_ed_in_spawn && ed_ptr(self)) {       /* a real soldier built at an address one of ours used to have (the pool reuses them): it is not ours */
        int k, hit = 0;
        for (k = 0; k < s_ed_nplaced && k < ED_NPLACED; k++)
            if (s_ed_placed[k] == self) {
                s_ed_placed[k] = s_ed_placed[--s_ed_nplaced];
                hit = 1;
                break;
            }
        for (k = 0; k < ED_NPL; k++)
            if (s_ed_pl_sol[k] == self) {
                s_ed_pl_sol[k] = 0;
                hit = 1;
            }
        for (k = 0; k < ED_NSLOT; k++)
            if (s_ed_slot[k].s == self) {
                s_ed_slot[k].s = 0;
                hit = 1;
            }
        if (s_ed_last_sol == self)
            s_ed_last_sol = 0;
        if (hit) {
            static int said;
            if (said++ < 10)
                fprintf(stderr, "[EDITOR] a real soldier was built at the old address %08X of an editor soldier: forgotten\n", (unsigned)self);
        }
    }
    if (!s_ed_in_spawn || !ed_ptr(self) || s_ed_answered)
        return;
    for (i = 0; i < ED_NSLOT && s_ed_slot[i].s; i++)
        ;
    if (i == ED_NSLOT)
        i = 0;                                  /* all busy: the oldest slot is reused */
    memset(&s_ed_slot[i], 0, sizeof s_ed_slot[i]);
    s_ed_slot[i].s = self;
    memcpy(s_ed_slot[i].tgt, s_ed_cur_tgt, sizeof s_ed_cur_tgt);
    s_ed_slot[i].left = 90;
    s_ed_slot[i].sp = s_ed_cur_sp;
    s_ed_slot[i].swapped = s_ed_cur_swapped;
    s_ed_slot[i].manual = s_ed_cur_manual;
    s_ed_answered = 1;
    s_ed_cur_slot = i;
    s_ed_anyslot = 1;
    ed_placed_add(self);
}

static int ed_kill(uint32_t s);
static void ed_kill_body(uint32_t s) { (void)ed_kill(s); }

/* one soldier of this team at tgt; prefer a spawner whose last type file is want (replay). Returns the soldier or 0. */
static uint32_t ed_spawn(int team, const float *tgt, const char *want, int manual, int record)
{
    int i, n = 0, pick = -1, ci = -1, slot;
    const char *use = NULL;
    uint32_t sp, pool0, spawned0;
    if (game_pc_get() < 0x80000000u)
        return 0;
    {   /* The game never holds this many soldiers at once; its fixed-size lists (event listener arrays and the like) may not survive it: the crash of 2026-11-20 13:26
         * came with 54 editor soldiers, inside the death notice of a soldier. So only so many of ours may exist (bodies count until the delete), and the saved
         * layout may not be longer than that either (else the replay would keep adding soldiers as the player kills them). */
        int n = 0, dead = 0, k, cap = ed_cap();
        uint32_t oldest = 0;
        for (k = 0; k < s_ed_nplaced && k < ED_NPLACED; k++)
            if (ed_alive(s_ed_placed[k])) {
                n++;
            } else if (ed_is_soldier(s_ed_placed[k])) {         /* a body of ours: it takes a slot of the game's lists until it is deleted */
                dead++;
                if (!oldest)
                    oldest = s_ed_placed[k];
            }
        if (dead > 12 && oldest)
            ed_kill_body(oldest);
        if (n >= cap || (record && s_ed_npl >= cap)) {
            char m[200];
            s_ed_capped = 1;
            snprintf(m, sizeof m, "Editor: limit reached (%d alive, %d placed, at most %d): remove some first (aim and press Numpad7)", n, s_ed_npl, cap);
            if (manual)
                recomp_osd_show(m);
            return 0;
        }
    }
    if (!team)                                /* the window passes 0: the folder says (German = enemy, the rest = friend) */
        team = (want && strstr(want, "\\German")) ? 2 : 1;
    if (want && want[0]) {                      /* a type was asked for: it must be one a spawner of this level has asked for (its definition is loaded) */
        ci = ed_cat_find(want);
        if (ci < 0 && s_ed_replay == 2 && s_ed_tries < 30)
            return 0;                           /* a saved layout: its spawners may not have ticked yet, wait a while before falling back to any type */
        if (ci < 0 && strchr(want, '\\') && ed_anytype())
            use = want;                         /* experiment: ask for a type no spawner of this level has asked for (its definition may not be loaded) */
    }
    if (ci >= 0) {
        int p = ed_sp_slot(s_ed_cat_sp[ci], 0);
        if (p >= 0 && MEM32(s_ed_sp[p]) == 0x30ED54u) {
            pick = p;                           /* the spawner that originally asked for this type: right team, right post */
            use = s_ed_cat[ci];
            n = 1;
            if (game_editor_spawner_team(s_ed_sp[p]) == 1 || game_editor_spawner_team(s_ed_sp[p]) == 2)
                team = game_editor_spawner_team(s_ed_sp[p]);
        }
    }
    if (pick < 0) {
        for (i = 0; i < s_ed_nsp; i++)
            if (MEM32(s_ed_sp[i]) == 0x30ED54u && game_editor_spawner_team(s_ed_sp[i]) == team)
                n++;
        if (!n) {
            if (s_ed_dbg)
                fprintf(stderr, "[EDITOR] no %s spawner known yet\n", team == 2 ? "enemy" : "friend");
            return 0;
        }
        {
            int want_k = s_ed_rot++ % n, k = 0;
            for (i = 0; i < s_ed_nsp; i++)
                if (MEM32(s_ed_sp[i]) == 0x30ED54u && game_editor_spawner_team(s_ed_sp[i]) == team && k++ == want_k) {
                    pick = i;
                    break;
                }
        }
    }
    if (pick < 0)
        return 0;
    sp = s_ed_sp[pick];
    pool0 = MEM32(sp + 0x48);
    spawned0 = MEM32(sp + 0x40);
    MEM32(sp + 0x48) = 1u;                      /* exactly one soldier: a big pool would let the handler's loop make a whole wave */
    MEM32(sp + 0x40) = MEM32(sp + 0x44);        /* the handler stops when spawned - killed >= MaxAlive: show it "nobody alive", and give the old count back below
                                                 * (our soldiers are invisible to the spawner's counts, else the second one of a spawner is never made) */
    game_editor_boost(sp, 60);
    s_ed_in_spawn = 1;
    s_ed_cur_team = team;
    s_ed_cur_record = record;
    s_ed_cur_manual = manual;
    s_ed_answered = 0;
    memcpy(s_ed_cur_tgt, tgt, sizeof s_ed_cur_tgt);
    s_ed_cur_swapped = 0;                       /* (the spawn-point swap of build 31 was removed: the owner chain differs per spawner, writes could hit unrelated objects) */
    ed_call_spawner(sp, pick, use);
    s_ed_in_spawn = 0;
    game_editor_boost(0, 0);
    MEM32(sp + 0x48) = pool0;                   /* the spawner's own pool and spawned count are left as they were; the kill / refund messages of our soldiers are dropped (game_spawnerrm_filter) */
    MEM32(sp + 0x40) = spawned0;
    if (!s_ed_answered) {
        if (s_ed_dbg)
            fprintf(stderr, "[EDITOR] spawner %08X made nothing (spawned %u killed %u pool %d)\n", (unsigned)sp, (unsigned)MEM32(sp + 0x40),
                    (unsigned)MEM32(sp + 0x44), (int)MEM32(sp + 0x48));
        return 0;
    }
    slot = s_ed_cur_slot;
    if (record && s_ed_npl < ED_NPL) {
        EdPlace *e = &s_ed_pl[s_ed_npl];
        e->team = team;
        memcpy(e->p, tgt, sizeof e->p);
        strcpy(e->name, use ? use : s_ed_name[pick]);
        s_ed_pl_sol[s_ed_npl] = s_ed_slot[slot].s;
        s_ed_npl++;
        ed_save();
    }
    if (s_ed_dbg)
        fprintf(stderr, "[EDITOR] %s soldier %08X from spawner %08X (%s%s) to %.2f %.2f %.2f, its team field %u\n", team == 2 ? "enemy" : "friend",
                (unsigned)s_ed_slot[slot].s, (unsigned)sp, use ? "asked for " : "", use ? use : s_ed_name[pick][0] ? s_ed_name[pick] : "type unknown yet",
                (double)tgt[0], (double)tgt[1], (double)tgt[2], (unsigned)MEM32(s_ed_slot[slot].s + 0xD8));
    return s_ed_slot[slot].s;
}

/* forget a soldier that is gone (or about to be) everywhere the editor keeps it */
static void ed_forget(uint32_t s)
{
    int i;
    for (i = 0; i < s_ed_nsol; i++)
        if (s_ed_sol[i] == s)
            s_ed_sol[i] = 0;
    for (i = 0; i < ED_NSLOT; i++)
        if (s_ed_slot[i].s == s)
            s_ed_slot[i].s = 0;
    for (i = 0; i < ED_NPL; i++)
        if (s_ed_pl_sol[i] == s)
            s_ed_pl_sol[i] = 0;
    if (s_ed_last_sol == s)
        s_ed_last_sol = 0;
    /* NOT removed from s_ed_placed: the destructor that is about to run sends the refund message, and the filter needs to see this soldier then */
}
/* Take a soldier out of the world the way the game itself does when it removes one: find its creation node in the list at 0x3A0338
 * (node + 0x18 = the object, node + 0xC = the next node), then run the delete command sub_000388F0 on the node (it unlinks the node, tells the
 * factory to destroy the object, and the soldier's destructor runs). The refund message the destructor sends to the spawner is dropped for
 * editor soldiers (game_spawnerrm_filter). Returns 1 when the delete ran. */
static int ed_delete(uint32_t s)
{
    uint32_t n = MEM32(0x3A0338), cmd = 0;
    int guard = 0;
    uint32_t s_eax = g_eax, s_ecx = g_ecx, s_edx = g_edx, s_ebx = g_ebx, s_esi = g_esi, s_edi = g_edi, s_ebp = g_ebp, s_sehebp = g_seh_ebp, s_esp = esp;
    RecompXmm s_x[8];
    double s_fps[8];
    int s_fpt = g_fp_top, k;
    if (!ed_is_soldier(s))
        return 0;
    while (n >= 0x10000u && n < 0x90000000u && guard++ < 16384) {
        if (MEM32(n + 0x18) == s) {
            cmd = n;
            break;
        }
        n = MEM32(n + 0xC);
    }
    if (!cmd) {
        if (s_ed_dbg)
            fprintf(stderr, "[EDITOR] no creation node found for soldier %08X (walked %d)\n", (unsigned)s, guard);
        return 0;
    }
    s_x[0] = g_xmm0; s_x[1] = g_xmm1; s_x[2] = g_xmm2; s_x[3] = g_xmm3; s_x[4] = g_xmm4; s_x[5] = g_xmm5; s_x[6] = g_xmm6; s_x[7] = g_xmm7;
    for (k = 0; k < 8; k++)
        s_fps[k] = g_fp_stack[k];
    ed_forget(s);
    esp = ((esp - 0x400u) & ~0xFu);
    PUSH32(esp, 0x0018A000u);
    g_ecx = cmd;
    s_ed_deleting = 1;
    RECOMP_ABI_CALL(0x000388F0u, sub_000388F0);
    s_ed_deleting = 0;
    for (k = 0; k < s_ed_nplaced && k < ED_NPLACED; k++)    /* the refund message has been sent and dropped: the address may be reused by a real soldier */
        if (s_ed_placed[k] == s) {
            s_ed_placed[k] = s_ed_placed[--s_ed_nplaced];
            break;
        }
    g_eax = s_eax; g_ecx = s_ecx; g_edx = s_edx; g_ebx = s_ebx; g_esi = s_esi; g_edi = s_edi;
    g_ebp = s_ebp; g_seh_ebp = s_sehebp; esp = s_esp;
    g_xmm0 = s_x[0]; g_xmm1 = s_x[1]; g_xmm2 = s_x[2]; g_xmm3 = s_x[3]; g_xmm4 = s_x[4]; g_xmm5 = s_x[5]; g_xmm6 = s_x[6]; g_xmm7 = s_x[7];
    for (k = 0; k < 8; k++)
        g_fp_stack[k] = s_fps[k];
    g_fp_top = s_fpt;
    if (s_ed_dbg)
        fprintf(stderr, "[EDITOR] deleted soldier %08X (node %08X)\n", (unsigned)s, (unsigned)cmd);
    return 1;
}
/* remove one soldier: the real delete, else (no node found) health 0 */
static int ed_kill(uint32_t s)
{
    if (!ed_is_soldier(s))
        return 0;
    if (ed_delete(s))
        return 1;
    if (!ed_alive(s))
        return 0;
    ed_setf(s + 0x2C60u + 0x9C, 0.0f);
    return 1;
}
/* the soldier nearest to the middle of the screen, inside about 10 degrees (a soldier is a thin target at a distance) */
static uint32_t ed_aimed(const float *eye, const float *fwd, float mincos)
{
    uint32_t best = 0;
    float bc = mincos;                          /* 0.985 = cos 10 degrees (the screen centre); the mouse ray is much more exact */
    int i;
    for (i = 0; i < s_ed_nsol; i++) {
        uint32_t s = s_ed_sol[i];
        float dx, dy, dz, d, c;
        if (!ed_alive(s))
            continue;
        dx = ed_f(s + 0x60) - eye[0]; dy = ed_f(s + 0x64) - eye[1]; dz = ed_f(s + 0x68) + 0.9f - eye[2];   /* aim at the chest */
        d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d < 0.3f || d > 200.0f)
            continue;
        c = (dx * fwd[0] + dy * fwd[1] + dz * fwd[2]) / d;
        if (c > bc) {
            bc = c;
            best = s;
        }
    }
    if (!best) {                                /* no living one: a body of one of ours (it still counts against the limit until it is deleted) */
        for (i = 0; i < s_ed_nplaced && i < ED_NPLACED; i++) {
            uint32_t s = s_ed_placed[i];
            float dx, dy, dz, d, c;
            if (!ed_is_soldier(s))
                continue;
            dx = ed_f(s + 0x60) - eye[0]; dy = ed_f(s + 0x64) - eye[1]; dz = ed_f(s + 0x68) + 0.3f - eye[2];
            d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d < 0.3f || d > 200.0f)
                continue;
            c = (dx * fwd[0] + dy * fwd[1] + dz * fwd[2]) / d;
            if (c > bc) {
                bc = c;
                best = s;
            }
        }
    }
    return best;
}
/* the placed soldiers that are alive: the number the window shows and the limit is about (a dead one frees its place, 8 -> 7 of 16) */
static int ed_placed_count(void)
{
    int n = 0, k;
    for (k = 0; k < s_ed_nplaced && k < ED_NPLACED; k++)
        if (ed_alive(s_ed_placed[k]))
            n++;
    return n;
}
/* a placed soldier that died leaves the saved layout too (else the saved lines would keep the count, and the limit, full) */
static void ed_reap_dead(void)
{
    int j, any = 0;
    for (j = 0; j < s_ed_npl; j++) {
        uint32_t s = s_ed_pl_sol[j];
        if (!s || !ed_is_soldier(s) || ed_alive(s) || !game_editor_is_placed(s))
            continue;
        memmove(&s_ed_pl[j], &s_ed_pl[j + 1], (size_t)(s_ed_npl - j - 1) * sizeof s_ed_pl[0]);
        memmove(&s_ed_pl_sol[j], &s_ed_pl_sol[j + 1], (size_t)(s_ed_npl - j - 1) * sizeof s_ed_pl_sol[0]);
        s_ed_pl_sol[s_ed_npl - 1] = 0;
        s_ed_npl--;
        if (s_ed_replay == 2 && j < s_ed_replay_i)
            s_ed_replay_i--;                    /* the replay index follows the lines that moved up */
        j--;
        any = 1;
    }
    if (any)
        ed_save();
}
static int ed_shown(void)                       /* the placed count the window shows: the living placed soldiers, plus the saved lines a running replay has not made yet */
{
    int n = ed_placed_count();
    if (s_ed_replay != 0 && s_ed_npl > 0) {
        int left = s_ed_replay == 2 ? s_ed_npl - s_ed_replay_i : s_ed_npl;
        if (left > 0)
            n += left;
    }
    return n;
}

/* How many of the soldiers the editor placed this run are still in the world's creation list. */
static int ed_tracked_live(void)
{
    uint32_t n = MEM32(0x3A0338), t[ED_NPL];
    int k, nt = 0, live = 0, guard = 0;
    for (k = 0; k < ED_NPL; k++)
        if (s_ed_pl_sol[k])
            t[nt++] = s_ed_pl_sol[k];
    if (!nt)
        return 0;
    while (n >= 0x10000u && n < 0x90000000u && guard++ < 16384) {
        uint32_t o = MEM32(n + 0x18);
        for (k = 0; k < nt; k++)
            if (t[k] == o) {
                live++;
                break;
            }
        n = MEM32(n + 0xC);
    }
    return live;
}
/* A mission restart may not open any file the load hook knows. A new world shows itself by a new player object or by the spawners of the
 * old world being gone; with none of our soldiers left in the world either, it is a restart (a kill never does this: the player object
 * and the spawners stay): the saved layout is placed again. Twice a second at most. */
static void ed_run_watch(void)
{
    static float lp[3];
    uint32_t pc = game_pc_get();
    int pc_new, sp_gone, jump = 0;
    float p[3];
    if (s_ed_replay != 0 || s_ed_npl == 0 || (g_game_frames % 30u) != 0 || pc < 0x80000000u)
        return;
    p[0] = ed_f(pc + 0x60u); p[1] = ed_f(pc + 0x64u); p[2] = ed_f(pc + 0x68u);
    if (s_ed_pc_seen) {                         /* a restart puts the player back at the start: a jump of 60 m between two looks */
        float dx = p[0] - lp[0], dy = p[1] - lp[1];
        jump = dx * dx + dy * dy > 3600.0f;
    }
    pc_new = s_ed_pc_seen && pc != s_ed_pc_seen;
    sp_gone = s_ed_nsp > 0 && MEM32(s_ed_sp[0]) != 0x30ED54u;
    lp[0] = p[0]; lp[1] = p[1]; lp[2] = p[2];
    s_ed_pc_seen = pc;
    if ((pc_new || (sp_gone && jump)) && ed_tracked_live() == 0) {
        fprintf(stderr, "[EDITOR] new world seen without a level load (%s): the saved layout of %s (%d soldiers) is placed again\n",
                pc_new ? "new player object" : "spawners gone and the player jumped", s_ed_level, s_ed_npl);
        ed_soft_reset();
        s_ed_pc_seen = pc;
        ed_rearm();
    }
}
/* The mission HUD is built (rs_open_hook: DATA\HUD\SP\hud2.xml). Once per level load; a second time in the same level is a restart (or a
 * reload after a death) that did not open level_a.str: the saved layout is placed again when none of our soldiers is left. */
void game_editor_hudload(void)
{
    if (!s_ed_level[0] || !strcmp(s_ed_level, "1_99"))
        return;
    if (!s_ed_hud_seen) {
        s_ed_hud_seen = 1;
        return;
    }
    if (ed_tracked_live() != 0)
        return;
    fprintf(stderr, "[EDITOR] mission HUD built a second time in level %s (restart): the saved layout (%d soldiers) is placed again\n", s_ed_level,
            s_ed_npl);
    ed_soft_reset();
    ed_rearm();
}

/* The one delete: the soldier in the middle of the screen leaves the world; when it was one of the placed ones its saved line goes too, so the
 * counter in the window falls and a slot is free again. */
/* a placed prop still stands where it was put (its object is alive, same kind, same place): the pool reuses addresses, so this is checked before
 * the object is touched */
static int ed_prop_obj_ok(int i)
{
    uint32_t o = s_ed_pp_obj[i];
    return ed_ptr(o) && MEM32(o) == 0x317248u && fabsf(ed_f(o + 0x60) - s_ed_pp[i].p[0]) < 0.6f && fabsf(ed_f(o + 0x64) - s_ed_pp[i].p[1]) < 0.6f &&
           fabsf(ed_f(o + 0x68) - s_ed_pp[i].p[2]) < 0.8f;
}
/* "remove" a pickup: the creator hands the object its matrix with the virtual method +0xEC (sub_00037CD0 does it right after making it); the same
 * call with the place far below the world takes the pickup, its glow and its pickup test out of play. No destructor: nothing is known about what
 * else still points at the object. */
static void ed_prop_move_away(uint32_t o)
{
    uint32_t s_eax = g_eax, s_ecx = g_ecx, s_edx = g_edx, s_ebx = g_ebx, s_esi = g_esi, s_edi = g_edi, s_ebp = g_ebp, s_sehebp = g_seh_ebp, s_esp = esp, buf, mat, target;
    RecompXmm s_x[8];
    double s_fps[8];
    int s_fpt = g_fp_top, k;
    recomp_func_t fn;
    target = MEM32(MEM32(o) + 0xECu);
    fn = recomp_lookup(target);
    if (!fn)
        return;
    s_x[0] = g_xmm0; s_x[1] = g_xmm1; s_x[2] = g_xmm2; s_x[3] = g_xmm3; s_x[4] = g_xmm4; s_x[5] = g_xmm5; s_x[6] = g_xmm6; s_x[7] = g_xmm7;
    for (k = 0; k < 8; k++)
        s_fps[k] = g_fp_stack[k];
    esp = ((esp - 0x800u) & ~0xFu);
    buf = esp - 0x200u;
    mat = buf + 0x80u;
    for (k = 0; k < 16; k++)
        ed_setf(mat + 4u * (uint32_t)k, 0.0f);
    ed_setf(mat + 0x00u, 1.0f);
    ed_setf(mat + 0x14u, 1.0f);
    ed_setf(mat + 0x28u, 1.0f);
    ed_setf(mat + 0x3Cu, 1.0f);
    ed_setf(mat + 0x30u, ed_f(o + 0x60));
    ed_setf(mat + 0x34u, ed_f(o + 0x64));
    ed_setf(mat + 0x38u, -10000.0f);
    esp = buf - 0x40u;
    PUSH32(esp, mat);
    PUSH32(esp, 0x0018A000u);
    g_ecx = o;
    RECOMP_ABI_CALL(target, fn);
    g_eax = s_eax; g_ecx = s_ecx; g_edx = s_edx; g_ebx = s_ebx; g_esi = s_esi; g_edi = s_edi;
    g_ebp = s_ebp; g_seh_ebp = s_sehebp; esp = s_esp;
    g_xmm0 = s_x[0]; g_xmm1 = s_x[1]; g_xmm2 = s_x[2]; g_xmm3 = s_x[3]; g_xmm4 = s_x[4]; g_xmm5 = s_x[5]; g_xmm6 = s_x[6]; g_xmm7 = s_x[7];
    for (k = 0; k < 8; k++)
        g_fp_stack[k] = s_fps[k];
    g_fp_top = s_fpt;
}
/* what is under a ray (eye, unit dir; mincos = how exact a soldier must be hit): our soldier or our prop, the nearer one goes */
static void ed_delete_aimed(const float *eye, const float *fwd, float mincos)
{
    char msg[160];
    uint32_t s = ed_aimed(eye, fwd, mincos);
    int j, line = -1, pl = -1;
    float st = 1e9f, pt = 1e9f;
    if (s) {
        float dx = ed_f(s + 0x60) - eye[0], dy = ed_f(s + 0x64) - eye[1], dz = ed_f(s + 0x68) + 0.9f - eye[2];
        st = dx * fwd[0] + dy * fwd[1] + dz * fwd[2];
    }
    for (j = 0; j < s_ed_npp; j++) {           /* our props: the ray passes within 0.8 m of the pickup's middle */
        float dx, dy, dz, t, perp2;
        if (!ed_prop_obj_ok(j))
            continue;
        dx = ed_f(s_ed_pp_obj[j] + 0x60) - eye[0]; dy = ed_f(s_ed_pp_obj[j] + 0x64) - eye[1]; dz = ed_f(s_ed_pp_obj[j] + 0x68) + 0.3f - eye[2];
        t = dx * fwd[0] + dy * fwd[1] + dz * fwd[2];
        perp2 = dx * dx + dy * dy + dz * dz - t * t;
        if (t > 0.3f && t < 200.0f && perp2 < 0.64f && t < pt) {
            pt = t;
            pl = j;
        }
    }
    if (pl >= 0 && pt < st) {                   /* a prop is nearer than any soldier on the ray */
        ed_prop_move_away(s_ed_pp_obj[pl]);
        memmove(&s_ed_pp[pl], &s_ed_pp[pl + 1], (size_t)(s_ed_npp - pl - 1) * sizeof s_ed_pp[0]);
        memmove(&s_ed_pp_obj[pl], &s_ed_pp_obj[pl + 1], (size_t)(s_ed_npp - pl - 1) * sizeof s_ed_pp_obj[0]);
        s_ed_pp_obj[s_ed_npp - 1] = 0;
        s_ed_npp--;
        ed_save();
        snprintf(msg, sizeof msg, "Editor: prop removed (%d of %d props saved)", s_ed_npp, ED_NPP);
        recomp_osd_show(msg);
        return;
    }
    if (!s && s_ed_nzn > 0) {                   /* no soldier, no prop: the ground inside a spawn zone's circle removes that zone (and its soldiers) */
        float zp[3], bd = 1e9f;
        int zi, best = -1;
        if (ed_world_point(eye, fwd, zp)) {
            for (zi = 0; zi < s_ed_nzn; zi++) {
                float dx = zp[0] - s_ed_zn[zi].p[0], dy = zp[1] - s_ed_zn[zi].p[1], d = sqrtf(dx * dx + dy * dy), lim = s_ed_zn[zi].r > 1.5f ? s_ed_zn[zi].r : 1.5f;
                if (d <= lim && d < bd) {
                    bd = d;
                    best = zi;
                }
            }
        }
        if (best >= 0) {
            ed_zone_remove(best);
            snprintf(msg, sizeof msg, "Editor: spawn zone removed (%d of %d zones left)", s_ed_nzn, ED_NZN);
            recomp_osd_show(msg);
            return;
        }
    }
    if (!s) {
        recomp_osd_show("Editor: nothing of yours there (it removes the soldiers, props and spawn zones you placed)");
        return;
    }
    for (j = 0; j < s_ed_npl; j++)              /* the saved line of this soldier, looked up before the delete forgets it */
        if (s_ed_pl_sol[j] == s) {
            line = j;
            break;
        }
    if (!ed_kill(s)) {
        recomp_osd_show("Editor: could not remove that soldier");
        return;
    }
    (void)ed_zone_note_deleted(s);
    if (line >= 0) {
        memmove(&s_ed_pl[line], &s_ed_pl[line + 1], (size_t)(s_ed_npl - line - 1) * sizeof s_ed_pl[0]);
        memmove(&s_ed_pl_sol[line], &s_ed_pl_sol[line + 1], (size_t)(s_ed_npl - line - 1) * sizeof s_ed_pl_sol[0]);
        s_ed_pl_sol[s_ed_npl - 1] = 0;
        s_ed_npl--;
        ed_save();
    }
    j = ed_shown();
    snprintf(msg, sizeof msg, "Editor: soldier removed (%d of %d placed, %d free)", j, ed_cap(), ed_cap() > j ? ed_cap() - j : 0);
    recomp_osd_show(msg);
}
/* every placed soldier leaves the world and the saved list is emptied (window button) */
static void ed_clear_all(void)
{
    char msg[140];
    int j, k, n = 0;
    for (j = 0; j < s_ed_npl; j++)
        if (s_ed_pl_sol[j] && game_editor_is_placed(s_ed_pl_sol[j]) && ed_kill(s_ed_pl_sol[j]))
            n++;
    memset(s_ed_pl_sol, 0, sizeof s_ed_pl_sol);
    s_ed_npl = 0;
    s_ed_replay = 0;
    ed_zone_clear();
    j = s_ed_npp;
    for (k = 0; k < s_ed_npp; k++)              /* the props standing in the world are taken out of play too */
        if (ed_prop_obj_ok(k))
            ed_prop_move_away(s_ed_pp_obj[k]);
    memset(s_ed_pp_obj, 0, sizeof s_ed_pp_obj);
    s_ed_npp = 0;
    s_ed_pr = 0;
    ed_save();
    if (j > 0)
        snprintf(msg, sizeof msg, "Editor: all cleared (%d soldiers and %d props removed)", n, j);
    else
        snprintf(msg, sizeof msg, "Editor: all cleared (%d soldiers removed, 0 of %d placed)", n, ed_cap());
    recomp_osd_show(msg);
}
/* the placed soldiers leave the world and the saved list is placed again from the start (window button: the way back when a restart brought nothing) */
static void ed_replay_now(void)
{
    int j, n = 0;
    for (j = 0; j < s_ed_npl; j++)
        if (s_ed_pl_sol[j] && game_editor_is_placed(s_ed_pl_sol[j]) && ed_kill(s_ed_pl_sol[j]))
            n++;
    memset(s_ed_pl_sol, 0, sizeof s_ed_pl_sol);
    s_ed_replay = s_ed_npl > 0 ? 2 : 0;
    s_ed_replay_i = 0;
    s_ed_gap = 0;
    s_ed_tries = 0;
    if (s_ed_dbg)
        fprintf(stderr, "[EDITOR] replay now: %d soldiers removed, %d lines to place\n", n, s_ed_npl);
}
/* The window's "Edits" button: this level's saved edits on / off (kept in the level's file). Off = the level starts as the game made it
 * (the file stays). Turning on in a running level places the saved edits now; turning off leaves what stands until the next start. */
static void ed_apply_toggle(void)
{
    int j, props_up = 0;
    char msg[160];
    if (s_ed_npl == 0 && s_ed_npp == 0 && s_ed_nzn == 0) {       /* nothing saved: the switch stays On (a flag kept Off would swallow the next thing placed) */
        recomp_osd_show("Editor: nothing saved in this level yet");
        return;
    }
    s_ed_apply = !s_ed_apply;
    ed_save();
    if (!s_ed_apply) {
        recomp_osd_show("Editor: edits OFF for this level. It starts as the game made it next time you enter or restart");
        return;
    }
    snprintf(msg, sizeof msg, "Editor: edits ON for this level (%d soldiers, %d props, %d zones)", s_ed_npl, s_ed_npp, s_ed_nzn);
    recomp_osd_show(msg);
    ed_replay_now();
    for (j = 0; j < s_ed_npp; j++)
        if (s_ed_pp_obj[j])
            props_up = 1;
    if (!props_up && s_ed_npp > 0) {
        s_ed_pr = 1;
        s_ed_pr_i = 0;
        s_ed_pr_t0 = 0;
        s_ed_pr_tries = 0;
    }
}

/* WORLD CAST: the game's own physics ray (sub_002236B0, used for bullets, the soldier's ground probe ...). Layout read from three callers
 * (sub_000FA8C0, sub_00182660, sub_000E83C0): this = [0x3DEA20] (ecx), arg1 = two vec4 (x y z 0) at +0 / +0x10, arg2 = R: byte 1 at R-0x10,
 * the collision mask at R-0xC, the hit fraction at R+0x14 (set to 1.0 before the call), the hit object at R+0x20 (set to 0). Which vec4 is
 * "from" is not proven, so ed_world_point tries several orders and masks and believes the point most casts agree on. Returns the fraction or -1. */
static float ed_cast(const float *a, const float *b, uint32_t mask, uint32_t *obj)
{
    uint32_t s_eax = g_eax, s_ecx = g_ecx, s_edx = g_edx, s_ebx = g_ebx, s_esi = g_esi, s_edi = g_edi, s_ebp = g_ebp, s_sehebp = g_seh_ebp, s_esp = esp;
    RecompXmm s_x[8];
    double s_fps[8];
    int s_fpt = g_fp_top, k;
    uint32_t world = MEM32(0x3DEA20u), buf, q, r;
    float f;
    *obj = 0;
    if (!ed_ptr(world))
        return -1.0f;
    s_x[0] = g_xmm0; s_x[1] = g_xmm1; s_x[2] = g_xmm2; s_x[3] = g_xmm3; s_x[4] = g_xmm4; s_x[5] = g_xmm5; s_x[6] = g_xmm6; s_x[7] = g_xmm7;
    for (k = 0; k < 8; k++)
        s_fps[k] = g_fp_stack[k];
    esp = ((esp - 0x800u) & ~0xFu);
    buf = esp - 0x200u;
    for (k = 0; k < 0x200; k += 4)
        MEM32(buf + (uint32_t)k) = 0;
    q = buf;
    r = buf + 0x30u;                            /* same layout as st_ray in blood_stain.c (proven live): byte at q+0x20, mask at q+0x24 */
    for (k = 0; k < 3; k++) {
        ed_setf(q + 4u * (uint32_t)k, a[k]);
        ed_setf(q + 0x10u + 4u * (uint32_t)k, b[k]);
    }
    ed_setf(q + 0x0Cu, MEMF(0x3210B0u));
    ed_setf(q + 0x1Cu, MEMF(0x3210B0u));
    MEM8(q + 0x20u) = 1;
    MEM32(q + 0x24u) = mask;
    ed_setf(r + 0x14u, 1.0f);
    MEM32(r + 0x20u) = 0;
    esp = buf - 0x40u;
    PUSH32(esp, r);
    PUSH32(esp, q);
    PUSH32(esp, 0x0018A000u);
    g_ecx = world;
    RECOMP_ABI_CALL(0x002236B0u, sub_002236B0);
    f = ed_f(r + 0x14u);
    *obj = MEM32(r + 0x20u);
    g_eax = s_eax; g_ecx = s_ecx; g_edx = s_edx; g_ebx = s_ebx; g_esi = s_esi; g_edi = s_edi;
    g_ebp = s_ebp; g_seh_ebp = s_sehebp; esp = s_esp;
    g_xmm0 = s_x[0]; g_xmm1 = s_x[1]; g_xmm2 = s_x[2]; g_xmm3 = s_x[3]; g_xmm4 = s_x[4]; g_xmm5 = s_x[5]; g_xmm6 = s_x[6]; g_xmm7 = s_x[7];
    for (k = 0; k < 8; k++)
        g_fp_stack[k] = s_fps[k];
    g_fp_top = s_fpt;
    return f == f ? f : -1.0f;
}
/* The first thing the mouse ray touches in the world (floor, a crate top, a wall): 1 and the point in out, or 0 (the caller falls back to a plane). */
static int ed_world_point(const float *eye, const float *dir, float *out)
{
    static unsigned drops;
    static const uint32_t masks[3] = { 0x4006u, 0x4003u, 0x4001u };   /* 0x4006 = the mask blood stains use (floor, walls, props) */
    float far_[3];
    int i, m, got = 0;
    int dbg = s_ed_dbg && drops < 20;
    if (!ed_ptr(game_pc_get()))
        return 0;
    for (i = 0; i < 3; i++)
        far_[i] = eye[i] + dir[i] * 300.0f;
    for (m = 0; m < 3 && !got; m++) {
        uint32_t obj;
        float f = ed_cast(eye, far_, masks[m], &obj);
        int ok = f > 0.001f && f < 0.999f && !(ed_ptr(obj) && MEM32(obj) == 0x317008u);   /* a soldier is not ground */
        if (dbg)
            fprintf(stderr, "[EDITOR] cast mask %04X: fraction %.4f object %08X (vtable %08X)%s\n", (unsigned)masks[m], (double)f, (unsigned)obj,
                    ed_ptr(obj) ? (unsigned)MEM32(obj) : 0u, ok ? " valid" : "");
        if (ok) {
            for (i = 0; i < 3; i++)
                out[i] = eye[i] + (far_[i] - eye[i]) * f;
            got = 1;
        }
    }
    if (dbg) {
        drops++;
        if (got)
            fprintf(stderr, "[EDITOR] world cast: point %.2f %.2f %.2f\n", (double)out[0], (double)out[1], (double)out[2]);
        else
            fprintf(stderr, "[EDITOR] world cast: no hit, the plane is used\n");
    }
    return got;
}

/* PROPS RESEARCH (RECOMP_EDITOR_LOG=1): entry hooks in gen (marker PROP-TRACE) on the functions that probably build a pickup from its definition.
 * id 1 = sub_000FE120 (looks a definition up by hash in [0x3DB8F0], else loads it, then makes the object), id 2 = sub_000FD160 (the pickup
 * constructor, class vtable 0x317248), id 3 = sub_000FC380 (fills a made object). Logs ecx, the first 6 arguments, a readable string if an argument
 * points at one, and the code-looking words of the stack (the callers). First 150 lines only. */
/* every pickup/object base constructor call is remembered with the code words of its stack (who made it): Numpad9 near an object then names its maker */
#define PT_N 8192
static struct { uint32_t obj, chain[6]; } s_pt[PT_N];
static unsigned s_pt_n;
static void ed_ctor_near(const float *eye)
{
    unsigned i, j, shown = 0;
    int k;
    for (i = s_pt_n > PT_N ? s_pt_n - PT_N : 0; i < s_pt_n; i++) {
        uint32_t o = s_pt[i % PT_N].obj;
        int later = 0;
        if (!ed_ptr(o))
            continue;
        for (j = i + 1; j < s_pt_n && !later; j++)      /* only the latest construction at an address */
            later = s_pt[j % PT_N].obj == o;
        if (later)
            continue;
        {
            float x = ed_f(o + 0x60), y = ed_f(o + 0x64), z = ed_f(o + 0x68);
            if (x == x && y == y && z == z && fabsf(x - eye[0]) < 8.0f && fabsf(y - eye[1]) < 8.0f && fabsf(z - eye[2]) < 8.0f && shown < 40) {
                shown++;
                fprintf(stderr, "[PROPNEAR] obj %08X vtable %08X +8 %08X pos %.2f %.2f %.2f maker", (unsigned)o, (unsigned)MEM32(o), (unsigned)MEM32(o + 8), (double)x, (double)y, (double)z);
                for (k = 0; k < 6; k++)
                    fprintf(stderr, " %05X", (unsigned)s_pt[i % PT_N].chain[k]);
                fprintf(stderr, "\n");
            }
        }
    }
    fprintf(stderr, "[PROPNEAR] done: %d objects near, %u constructions recorded\n", (int)shown, s_pt_n);
}
void game_prop_trace(int id, uint32_t self, uint32_t sp)
{
    static unsigned n, seen_n;
    static uint32_t seen[64][2], seen_c[64];
    uint32_t k, shown = 0, chain[6] = { 0 };
    char line[700];
    int len;
    if (id == 2 && s_pt_making && !s_pt_inner)
        s_pt_inner = self;                      /* the pickup our own make call is building (always on: the delete needs it) */
    {   /* the research logging below crashed a Russian level (a stray argument read as a pointer); the prop maker was found long ago, so it is opt-in now */
        static int on = -1;
        if (on < 0)
            on = getenv("RECOMP_PROP_TRACE") != NULL;
        if (!s_ed_dbg || !on)
            return;
    }
    if (id != 2)
        return;
    for (k = 0; k < 64 && shown < 6; k++) {
        uint32_t w = MEM32(sp + 4u * k);
        if (w >= 0x1000u && w < 0x2F0000u)
            chain[shown++] = w;
    }
    s_pt[s_pt_n % PT_N].obj = self;
    memcpy(s_pt[s_pt_n % PT_N].chain, chain, sizeof chain);
    s_pt_n++;
    for (k = 0; k < seen_n; k++)                /* log the first 5 constructions per distinct maker */
        if (seen[k][0] == chain[0] && seen[k][1] == chain[1])
            break;
    if (k == seen_n) {
        if (seen_n >= 64)
            return;
        seen[seen_n][0] = chain[0];
        seen[seen_n][1] = chain[1];
        seen_c[seen_n++] = 0;
    }
    if (seen_c[k]++ >= 5 || n >= 400)
        return;
    n++;
    shown = 0;
    len = snprintf(line, sizeof line, "[PROP] #%u fn%d ecx %08X args", n, id, (unsigned)self);
    for (k = 1; k <= 6; k++) {
        uint32_t a = MEM32(sp + 4u * k);
        len += snprintf(line + len, sizeof line - (size_t)len, " %08X", (unsigned)a);
        if ((a >= 0x10000u && a < 0x04000000u - 64u) || (a >= 0x80000000u && a < 0x84000000u - 64u)) {   /* guest memory is the low 64 MB and the heap at 0x8xxxxxxx only: a stray argument like 0x77448691 is unmapped (crash 2026-11-20 in Russia) */
            const char *s = (const char *)XBOX_PTR(a);
            int ok = 0, i;
            for (i = 0; i < 40 && s[i] >= 32 && s[i] < 127; i++)
                ;
            ok = i >= 4 && (i == 40 || s[i] == 0);
            if (ok)
                len += snprintf(line + len, sizeof line - (size_t)len, "(\"%.40s\")", s);
        }
    }
    len += snprintf(line + len, sizeof line - (size_t)len, " | stack");
    for (k = 0; k < 64 && shown < 10; k++) {
        uint32_t w = MEM32(sp + 4u * k);
        if (w >= 0x1000u && w < 0x2F0000u) {
            len += snprintf(line + len, sizeof line - (size_t)len, " %05X", (unsigned)w);
            shown++;
        }
    }
    fprintf(stderr, "%s\n", line);
}

/* PROP MAKER. The level's powerup node (sub_000A2C30) makes its pickup with sub_00037CD0(matrix, hash, ...), cdecl, which reads about 11 argument
 * dwords (the tail is leftovers of the node's own stack: record pointers). Both hooks are gen hand-edits (marker PROP-TRACE: recomp_0020.c
 * loc_000A2C7D = the definition path string the node is about to hash, recomp_0006.c loc_00037CD0 = the call). Every kind of powerup the
 * level makes is remembered as a TEMPLATE (path, hash, the argument dwords, the matrix); ed_make_prop then makes more of that kind with the same
 * call, our own matrix (identity rotation, position at +0x30: rows right / forward / up / position, 16-byte aligned) and hash. The call is
 * synchronous and complete: model, green plus and pickup all work (confirmed 2026-11-20). A template needs the level to have made one of
 * that kind (the arguments point into the level's own data), so only kinds that exist in the current level are offered. */
typedef struct { uint32_t hash, cnt, a[12]; float m[16]; char path[ED_PATH]; } PropTpl;
#define PT_MAX 48
static PropTpl s_ptpl[PT_MAX];
static char s_pt_lastpath[ED_PATH];
/* every pickup kind ever seen in any level (hash + definition path), kept in mods\maps\props_catalog.txt like the soldier catalog: the Props tab
 * can show the kinds of other levels greyed out, and the cross-level probe (Numpad4, debug only) takes its foreign hashes from it */
#define PT_GMAX 160
static struct { uint32_t hash, cnt; char path[ED_PATH]; } s_pt_g[PT_GMAX];   /* cnt = the kind's own quantity argument (ammo, 1 for a medkit), 0 = not known yet */
static int s_pt_gn, s_pt_gloaded;
static void ed_pt_save_catalog(void)
{
    int i;
    FILE *f;
    CreateDirectoryA("mods", NULL);
    CreateDirectoryA("mods\\maps", NULL);
    f = fopen("mods\\maps\\props_catalog.txt", "w");
    if (f) {
        for (i = 0; i < s_pt_gn; i++)
            fprintf(f, "%08X %u %s\n", (unsigned)s_pt_g[i].hash, (unsigned)s_pt_g[i].cnt, s_pt_g[i].path);
        fclose(f);
    }
}
static void ed_pt_gload(void)
{
    char line[ED_PATH + 40];
    FILE *f;
    if (s_pt_gloaded)
        return;
    s_pt_gloaded = 1;
    f = fopen("mods\\maps\\props_catalog.txt", "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f) && s_pt_gn < PT_GMAX) {
        unsigned h, c = 0;
        char pa[ED_PATH];
        if (sscanf(line, "%x %u %159s", &h, &c, pa) == 3 || (c = 0, sscanf(line, "%x %159s", &h, pa) == 2)) {   /* the old format had no count */
            if (strstr(pa, "MG42"))                     /* dropped 2026-11-20: its pickup gives "OUT OF AMMO" and no shots (needs experiments) */
                continue;
            s_pt_g[s_pt_gn].hash = h;
            s_pt_g[s_pt_gn].cnt = c;
            strcpy(s_pt_g[s_pt_gn].path, pa);
            s_pt_gn++;
        }
    }
    fclose(f);
}
static void ed_pt_remember(uint32_t hash, const char *path, uint32_t cnt)
{
    int i;
    ed_pt_gload();
    for (i = 0; i < s_pt_gn; i++)
        if (s_pt_g[i].hash == hash) {
            if (s_pt_g[i].cnt != cnt && cnt) {          /* a count learnt now (older files had none) */
                s_pt_g[i].cnt = cnt;
                ed_pt_save_catalog();
            }
            return;
        }
    if (s_pt_gn >= PT_GMAX)
        return;
    s_pt_g[s_pt_gn].hash = hash;
    s_pt_g[s_pt_gn].cnt = cnt;
    strncpy(s_pt_g[s_pt_gn].path, path, ED_PATH - 1);
    s_pt_g[s_pt_gn].path[ED_PATH - 1] = 0;
    s_pt_gn++;
    ed_pt_save_catalog();
}
void game_prop_path(uint32_t p)
{
    int i;
    s_pt_lastpath[0] = 0;
    if (p < 0x10000u || p >= 0x84000000u - ED_PATH || (p >= 0x04000000u - ED_PATH && p < 0x80000000u))
        return;
    for (i = 0; i < ED_PATH - 1; i++) {
        uint8_t c = MEM8(p + (uint32_t)i);
        if (c == 0)
            break;
        if (c < 32 || c > 126)
            return;
        s_pt_lastpath[i] = (char)c;
    }
    if (i < 3 || i >= ED_PATH - 1)
        s_pt_lastpath[0] = 0;
    else
        s_pt_lastpath[i] = 0;
}
void game_prop37_trace(uint32_t sp)
{
    uint32_t ret = MEM32(sp), mat = MEM32(sp + 4u), hash = MEM32(sp + 8u), k;
    int i, ti = -1;
    if (ret == 0x000A2CD8u && !s_pt_lastpath[0] && s_ed_dbg) {
        static int warned;
        if (warned++ < 5)
            fprintf(stderr, "[PROP37] the powerup node called the creator but no readable definition path was captured (hash %08X)\n", (unsigned)hash);
    }
    if (ret != 0x000A2CD8u && s_ed_dbg) {      /* the other callers (ammo? lives?): who and what, a few distinct ones */
        static uint32_t seen_h[16];
        static int nseen;
        int q;
        for (q = 0; q < nseen && seen_h[q] != hash; q++)
            ;
        if (q == nseen && nseen < 16) {
            seen_h[nseen++] = hash;
            fprintf(stderr, "[PROP37] other caller: return %08X hash %08X (not a powerup node)\n", (unsigned)ret, (unsigned)hash);
        }
    }
    if (ret != 0x000A2CD8u || !s_pt_lastpath[0] || !((mat >= 0x10000u && mat < 0x04000000u - 64u) || (mat >= 0x80000000u && mat < 0x84000000u - 64u)))
        return;                                 /* only the powerup node's own call (the other callers make other things) */
    for (i = 0; i < s_ptpl_n; i++)
        if (s_ptpl[i].hash == hash)
            ti = i;
    if (ti < 0) {
        if (s_ptpl_n >= PT_MAX)
            return;
        ti = s_ptpl_n++;
        s_ptpl[ti].cnt = 0;
        if (s_ed_dbg) {
            fprintf(stderr, "[PROP37] new kind %s hash %08X (kind %d of this level) args", s_pt_lastpath, (unsigned)hash, s_ptpl_n);
            for (k = 2; k < 12; k++)
                fprintf(stderr, " %08X", (unsigned)MEM32(sp + 4u + 4u * k));
            fprintf(stderr, "\n");
        }
    }
    ed_pt_remember(hash, s_pt_lastpath, MEM32(sp + 4u + 4u * 5u));   /* (cheap when known) also teaches an older catalog line its count */
    s_ptpl[ti].hash = hash;
    s_ptpl[ti].cnt++;
    for (k = 0; k < 12; k++)
        s_ptpl[ti].a[k] = MEM32(sp + 4u + 4u * k);
    for (k = 0; k < 16; k++)
        s_ptpl[ti].m[k] = ed_f(mat + 4u * k);
    strcpy(s_ptpl[ti].path, s_pt_lastpath);
}
static int ed_prop_find(const char *path)
{
    int i;
    for (i = 0; i < s_ptpl_n; i++)
        if (!_stricmp(s_ptpl[i].path, path))
            return i;
    return -1;
}
/* one more pickup of template ti at pos. Returns what the creator returned (0 = nothing made). */
static uint32_t ed_make_prop_h(int ti, uint32_t hash_override, uint32_t count_override, const float *pos)
{
    uint32_t buf, mat, made, hash = hash_override ? hash_override : s_ptpl[ti].hash;
    unsigned n0;
    int k;
    uint32_t s_eax = g_eax, s_ecx = g_ecx, s_edx = g_edx, s_ebx = g_ebx, s_esi = g_esi, s_edi = g_edi, s_ebp = g_ebp, s_sehebp = g_seh_ebp, s_esp = esp;
    RecompXmm s_x[8];
    double s_fps[8];
    int s_fpt = g_fp_top;
    s_x[0] = g_xmm0; s_x[1] = g_xmm1; s_x[2] = g_xmm2; s_x[3] = g_xmm3; s_x[4] = g_xmm4; s_x[5] = g_xmm5; s_x[6] = g_xmm6; s_x[7] = g_xmm7;
    for (k = 0; k < 8; k++)
        s_fps[k] = g_fp_stack[k];
    esp = ((esp - 0x800u) & ~0xFu);
    buf = esp - 0x200u;
    mat = buf + 0x80u;                          /* 16-byte aligned: esp was aligned, so are these offsets */
    for (k = 0; k < 16; k++)
        ed_setf(mat + 4u * (uint32_t)k, 0.0f);
    ed_setf(mat + 0x00u, 1.0f);
    ed_setf(mat + 0x14u, 1.0f);
    ed_setf(mat + 0x28u, 1.0f);
    ed_setf(mat + 0x3Cu, 1.0f);
    for (k = 0; k < 3; k++)
        ed_setf(mat + 0x30u + 4u * (uint32_t)k, pos[k]);
    esp = buf - 0x40u;
    n0 = s_pt_n;
    for (k = 11; k >= 2; k--)                   /* the template's own arguments (record pointers, flags); the kind's own quantity replaces the template's (a[5]) */
        PUSH32(esp, k == 5 && count_override ? count_override : s_ptpl[ti].a[k]);
    PUSH32(esp, hash);
    PUSH32(esp, mat);
    PUSH32(esp, 0x0018A000u);
    s_pt_inner = 0;
    s_pt_making = 1;
    RECOMP_ABI_CALL(0x00037CD0u, sub_00037CD0);
    s_pt_making = 0;
    s_pt_last_inner = s_pt_inner;
    made = g_eax;
    g_eax = s_eax; g_ecx = s_ecx; g_edx = s_edx; g_ebx = s_ebx; g_esi = s_esi; g_edi = s_edi;
    g_ebp = s_ebp; g_seh_ebp = s_sehebp; esp = s_esp;
    g_xmm0 = s_x[0]; g_xmm1 = s_x[1]; g_xmm2 = s_x[2]; g_xmm3 = s_x[3]; g_xmm4 = s_x[4]; g_xmm5 = s_x[5]; g_xmm6 = s_x[6]; g_xmm7 = s_x[7];
    for (k = 0; k < 8; k++)
        g_fp_stack[k] = s_fps[k];
    g_fp_top = s_fpt;
    if (s_ed_dbg)
        fprintf(stderr, "[EDITOR] prop %s made: returned %08X at %.2f %.2f %.2f (%u object constructions inside the call)\n", s_ptpl[ti].path, (unsigned)made,
                (double)pos[0], (double)pos[1], (double)pos[2], s_pt_n - n0);
    return made;
}
static uint32_t ed_make_prop(int ti, const float *pos)
{
    return ed_make_prop_h(ti, 0, 0, pos);
}
/* place a prop of the given path for real; save = also keep it in the saved list. 0 = could not (no template, or the limit). */
static int ed_prop_global(const char *path)      /* index in the all-levels catalog, -1 = unknown */
{
    int i;
    for (i = 0; i < s_pt_gn; i++)
        if (!_stricmp(s_pt_g[i].path, path))
            return i;
    return -1;
}
/* Does the level hold the definition of this pickup kind? The creator claims a pickup slot (flag |= 2 at sub_00037CD0) BEFORE it looks the kind up and
 * never frees it when the lookup fails, so a kind the level does not have must never reach it (2026-11-20: weapons of other levels returned 0 with
 * Revive / Document / Medkit fine: the weapon pickups' definitions live in their own level's archive). The same two lookups the creator makes:
 * sub_000FDEB0 (thiscall on [0x3DB8F0], the definition store) and sub_000F44E0 (the weapon table). Answers are kept for the level; a "no" is asked again after 120 frames. */
static int ed_kind_resident(uint32_t hash)
{
    uint32_t r1, r2, s_eax = g_eax, s_ecx = g_ecx, s_edx = g_edx, s_ebx = g_ebx, s_esi = g_esi, s_edi = g_edi, s_ebp = g_ebp, s_sehebp = g_seh_ebp, s_esp = esp;
    RecompXmm s_x[8];
    double s_fps[8];
    int s_fpt = g_fp_top, k;
    if (MEM32(0x003DB8F0u) < 0x80000000u)               /* no definition store yet (between levels) */
        return 0;
    for (k = 0; k < s_pt_resn; k++)
        if (s_pt_res[k].hash == hash) {
            if (s_pt_res[k].ok || g_game_frames - s_pt_res[k].frame < 120u)
                return s_pt_res[k].ok;
            break;
        }
    if (k == s_pt_resn) {
        if (s_pt_resn >= 160)
            return 0;
        s_pt_resn++;
    }
    s_x[0] = g_xmm0; s_x[1] = g_xmm1; s_x[2] = g_xmm2; s_x[3] = g_xmm3; s_x[4] = g_xmm4; s_x[5] = g_xmm5; s_x[6] = g_xmm6; s_x[7] = g_xmm7;
    for (k = 0; k < 8; k++)
        s_fps[k] = g_fp_stack[k];
    esp = ((esp - 0x800u) & ~0xFu);
    PUSH32(esp, hash);
    PUSH32(esp, 0x0018A000u);
    g_ecx = MEM32(0x003DB8F0u);
    RECOMP_ABI_CALL(0x000FDEB0u, sub_000FDEB0);
    r1 = g_eax;
    esp = ((s_esp - 0x800u) & ~0xFu);
    PUSH32(esp, hash);
    PUSH32(esp, 0x0018A000u);
    RECOMP_ABI_CALL(0x000F44E0u, sub_000F44E0);
    r2 = g_eax;
    g_eax = s_eax; g_ecx = s_ecx; g_edx = s_edx; g_ebx = s_ebx; g_esi = s_esi; g_edi = s_edi;
    g_ebp = s_ebp; g_seh_ebp = s_sehebp; esp = s_esp;
    g_xmm0 = s_x[0]; g_xmm1 = s_x[1]; g_xmm2 = s_x[2]; g_xmm3 = s_x[3]; g_xmm4 = s_x[4]; g_xmm5 = s_x[5]; g_xmm6 = s_x[6]; g_xmm7 = s_x[7];
    for (k = 0; k < 8; k++)
        g_fp_stack[k] = s_fps[k];
    g_fp_top = s_fpt;
    for (k = 0; k < s_pt_resn && s_pt_res[k].hash != hash; k++)
        ;
    if (k == s_pt_resn)
        k = s_pt_resn - 1;
    s_pt_res[k].hash = hash;
    s_pt_res[k].ok = r1 != 0 || r2 != 0;
    s_pt_res[k].frame = g_game_frames;
    if (s_ed_dbg)
        fprintf(stderr, "[EDITOR] RESIDENT %08X: definition store %08X, weapon table %08X -> %s\n", (unsigned)hash, (unsigned)r1, (unsigned)r2, s_pt_res[k].ok ? "yes" : "no");
    return s_pt_res[k].ok;
}
/* WEAPON TABLE (2026-11-20 log): a weapon pickup is made from the level's weapon table, entries of 0x164 bytes at [[0x3DB654]+0x20C] (count at +0x210), the
 * pickup's hash = the first dword of the entry (sub_000F44E0). The table holds every weapon the level uses (player, enemies, pickups: 1_2 has 45Auto,
 * MP45Auto, Karabiner, Thompson, MG42, MP40, Brit, Stick), more than the level's own pickup nodes ever make. Every entry is offered as a pickup kind:
 * its name (+0xE0 string) gives the Powerups\Player<name>.xdd path the Props tab and the saved file use. */
static void ed_pt_sync_wtable(void)
{
    uint32_t t, n, base, i;
    if (game_pc_get() < 0x80000000u)
        return;
    t = MEM32(0x003DB654u);
    if (t < 0x80000000u || t >= 0x84000000u - 0x300u)
        return;
    n = MEM32(t + 0x210u);
    base = MEM32(t + 0x20Cu);
    if (n == 0 || n > 64 || base < 0x80000000u || base + n * 0x164u > 0x84000000u)
        return;
    ed_pt_gload();
    if (s_ed_dbg) {                                     /* once per table size: which weapons the level has (a spliced-in weapon shows here) */
        static uint32_t s_wt_base, s_wt_n;
        if (s_wt_base != base || s_wt_n != n) {
            s_wt_base = base;
            s_wt_n = n;
            for (i = 0; i < n; i++) {
                uint32_t e = base + i * 0x164u, np = MEM32(e + 0xE0u);
                char nm[32];
                int l = 0;
                if (np >= 0x10000u && np < 0x84000000u - 40u)
                    for (; l < 31 && MEM8(np + (uint32_t)l) >= 33 && MEM8(np + (uint32_t)l) <= 126; l++)
                        nm[l] = (char)MEM8(np + (uint32_t)l);
                nm[l] = 0;
                fprintf(stderr, "[WTABLE] %u/%u hash %08X '%s'\n", (unsigned)i, (unsigned)n, (unsigned)MEM32(e), nm);
                {   /* every nonzero dword of the 0x164-byte entry: a resource that did not resolve shows as 0 / a wild value in one weapon only */
                    uint32_t o;
                    char ln[400];
                    int ll = 0;
                    for (o = 0; o < 0x164u; o += 4u) {
                        uint32_t w = MEM32(e + o);
                        if (!w)
                            continue;
                        ll += snprintf(ln + ll, sizeof ln - (size_t)ll, " %X:%X", (unsigned)o, (unsigned)w);
                        if (ll > 340) {
                            fprintf(stderr, "[WENTRY] %s%s\n", nm, ln);
                            ll = 0;
                        }
                    }
                    if (ll)
                        fprintf(stderr, "[WENTRY] %s%s\n", nm, ln);
                }
            }
        }
    }
    for (i = 0; i < n; i++) {
        uint32_t e = base + i * 0x164u, h = MEM32(e), np = MEM32(e + 0xE0u);
        char nm[32], path[ED_PATH];
        int l, ok = 1;
        if (!h || np < 0x10000u || np >= 0x84000000u - 40u)
            continue;
        for (l = 0; l < 31; l++) {
            uint8_t c = MEM8(np + (uint32_t)l);
            if (!c)
                break;
            if (c < 33 || c > 126) {
                ok = 0;
                break;
            }
            nm[l] = (char)c;
        }
        if (!ok || l < 2 || l >= 31)
            continue;
        nm[l] = 0;
        if (!_stricmp(nm, "MG42"))                      /* see ed_pt_gload */
            continue;
        snprintf(path, sizeof path, "Z:\\MOH4\\data\\Shared\\definitions\\Powerups\\Player%s.xdd", nm);
        ed_pt_remember(h, path, 0);
    }
}
static uint32_t ed_place_prop(const char *path, const float *pos, int save)
{
    int ti = ed_prop_find(path);
    uint32_t made, hash = 0, cnt = 0;
    if (ti < 0 && ed_prop_global(path) < 0)
        ed_pt_sync_wtable();
    if (ti < 0) {                                       /* a kind of ANOTHER level: the creator with one of this level's templates (its arguments are
                                                         * only record pointers and flags), the kind's own hash and quantity - if this level holds its definition */
        int gi = ed_prop_global(path);
        if (gi < 0 || s_ptpl_n <= 0 || !ed_kind_resident(s_pt_g[gi].hash))
            return 0;
        ti = 0;
        hash = s_pt_g[gi].hash;
        cnt = s_pt_g[gi].cnt ? s_pt_g[gi].cnt : 24u;    /* a quantity not learnt yet (a weapon only the table showed): a normal magazine's worth */
    }
    if (save && s_ed_npp >= ED_NPP) {
        fprintf(stderr, "[EDITOR] prop limit of %d reached\n", ED_NPP);
        return 0;
    }
    made = ed_make_prop_h(ti, hash, cnt, pos);
    if (made && save) {
        EdProp *e = &s_ed_pp[s_ed_npp];
        s_ed_pp_obj[s_ed_npp++] = s_pt_last_inner;
        memcpy(e->p, pos, sizeof e->p);
        strncpy(e->path, path, sizeof e->path - 1);
        e->path[sizeof e->path - 1] = 0;
        ed_save();
    }
    return made;
}

/* PROPS RESEARCH: every aligned float triple in the guest heap within 1.5 m (horizontal) / 3 m (vertical) of the camera, except exact copies of
 * the camera position. With the camera a metre above a pickup, the pickup's own position fields show up here; for each hit the nearest word below
 * it (up to 0x200 bytes) that looks like a vtable pointer (in the XBE data, its first entry a code address) is logged as the object's start. */
static void ed_heap_scan(const float *eye)
{
    uint32_t a, hits = 0;
    for (a = 0x80000000u; a < 0x84000000u - 0x20u && hits < 25; a += 4) {
        float x = ed_f(a), y = ed_f(a + 4), z;
        if (!(fabsf(x - eye[0]) < 1.5f && fabsf(y - eye[1]) < 1.5f))
            continue;
        z = ed_f(a + 8);
        if (!(fabsf(z - eye[2]) < 3.0f))
            continue;
        if (x == eye[0] && y == eye[1])
            continue;
        {
            uint32_t b, vt = 0, off = 0;
            for (b = a; b + 0x200u > a && b >= 0x80000004u; b -= 4) {
                uint32_t w = MEM32(b);
                if (w >= 0x2F0000u && w < 0x340000u && MEM32(w) >= 0x10000u && MEM32(w) < 0x2E0000u) {
                    vt = w;
                    off = a - b;
                    break;
                }
            }
            fprintf(stderr, "[CENSUS] float triple at %08X: %.2f %.2f %.2f (camera %.2f %.2f %.2f), object start %08X vtable %08X (field offset 0x%X)\n",
                    (unsigned)a, (double)x, (double)y, (double)z, (double)eye[0], (double)eye[1], (double)eye[2], (unsigned)(a - off), (unsigned)vt,
                    (unsigned)off);
        }
        hits++;
    }
    fprintf(stderr, "[CENSUS] heap scan done, %u hits\n", (unsigned)hits);
}

/* PROPS RESEARCH: where does the game keep a placed entity's name / definition path? Finds the string in the guest heap, then every word that
 * points at its start or at a point inside it (level records point into the string table), and logs the object each referrer sits in. */
static void ed_str_scan(const char *needle)
{
    uint32_t a, lim = 0x84000000u - 0x40u, nstr = 0, hits = 0;
    size_t nl = strlen(needle);
    for (a = 0x80000000u; a < lim && nstr < 4; a++) {
        uint32_t s, r;
        size_t k;
        if (MEM8(a) != (uint8_t)needle[0])
            continue;
        for (k = 1; k < nl && MEM8(a + (uint32_t)k) == (uint8_t)needle[k]; k++)
            ;
        if (k < nl)
            continue;
        for (s = a; s > 0x80000000u && MEM8(s - 1) != 0 && a - s < 160; s--)    /* back to the start of the string */
            ;
        nstr++;
        fprintf(stderr, "[CENSUS] string \"%s\" found at %08X (starts %08X)\n", needle, (unsigned)a, (unsigned)s);
        for (r = 0x80000000u; r < 0x84000000u - 0x20u && hits < 16; r += 4) {
            uint32_t w = MEM32(r);
            if (w >= s && w <= s + (uint32_t)(a - s) + 1u && w != 0) {      /* a word pointing to the start of the string or into it */
                uint32_t b, vt = 0, off = 0;
                for (b = r; b + 0x200u > r && b >= 0x80000004u; b -= 4) {
                    uint32_t v = MEM32(b);
                    if (v >= 0x2F0000u && v < 0x340000u && MEM32(v) >= 0x10000u && MEM32(v) < 0x2E0000u) {
                        vt = v;
                        off = r - b;
                        break;
                    }
                }
                fprintf(stderr, "[CENSUS]   referrer at %08X -> %08X: object start %08X vtable %08X (field 0x%X)\n", (unsigned)r, (unsigned)w,
                        (unsigned)(r - off), (unsigned)vt, (unsigned)off);
                hits++;
            }
        }
    }
    fprintf(stderr, "[CENSUS] \"%s\": %u copies, %u referrers\n", needle, (unsigned)nstr, (unsigned)hits);
}

/* ---------- spawn zones (see the declarations at the top) ---------- */
static float ed_zrand(void)                     /* 0..1, a small generator of our own (the game's is not ours to disturb) */
{
    static uint32_t st = 0x9E3779B9u;
    st = st * 1664525u + 1013904223u;
    return (float)(st >> 8) / 16777216.0f;
}
static int ed_zone_alive(int zi)
{
    int k, n = 0;
    for (k = 0; k < ED_ZMAX; k++)
        if (s_zn_sol[zi][k] && game_editor_is_placed(s_zn_sol[zi][k]) && ed_is_soldier(s_zn_sol[zi][k]) && ed_alive(s_zn_sol[zi][k]))
            n++;
    return n;
}
static void ed_zone_clear(void)                 /* the zones go with their soldiers (window Clear button) */
{
    int zi, k;
    for (zi = 0; zi < s_ed_nzn; zi++)
        for (k = 0; k < ED_ZMAX; k++)
            if (s_zn_sol[zi][k] && game_editor_is_placed(s_zn_sol[zi][k]))
                (void)ed_kill(s_zn_sol[zi][k]);
    s_ed_nzn = 0;
    ed_zone_reset_runtime();
}
static void ed_zone_remove(int zi)
{
    int k, n;
    for (k = 0; k < ED_ZMAX; k++)
        if (s_zn_sol[zi][k] && game_editor_is_placed(s_zn_sol[zi][k]))
            (void)ed_kill(s_zn_sol[zi][k]);
    n = s_ed_nzn - zi - 1;
    memmove(&s_ed_zn[zi], &s_ed_zn[zi + 1], (size_t)n * sizeof s_ed_zn[0]);
    memmove(s_zn_sol[zi], s_zn_sol[zi + 1], (size_t)n * sizeof s_zn_sol[0]);
    memmove(&s_zn_killed[zi], &s_zn_killed[zi + 1], (size_t)n * sizeof s_zn_killed[0]);
    memmove(&s_zn_pend[zi], &s_zn_pend[zi + 1], (size_t)n * sizeof s_zn_pend[0]);
    memmove(&s_zn_force[zi], &s_zn_force[zi + 1], (size_t)n * sizeof s_zn_force[0]);
    memmove(&s_zn_fail[zi], &s_zn_fail[zi + 1], (size_t)n * sizeof s_zn_fail[0]);
    memmove(&s_zn_need[zi], &s_zn_need[zi + 1], (size_t)n * sizeof s_zn_need[0]);
    memmove(&s_zn_live[zi], &s_zn_live[zi + 1], (size_t)n * sizeof s_zn_live[0]);
    memmove(&s_zn_k0[zi], &s_zn_k0[zi + 1], (size_t)n * sizeof s_zn_k0[0]);
    s_ed_nzn--;
    memset(s_zn_sol[s_ed_nzn], 0, sizeof s_zn_sol[0]);
    s_zn_killed[s_ed_nzn] = s_zn_pend[s_ed_nzn] = s_zn_force[s_ed_nzn] = s_zn_fail[s_ed_nzn] = 0;
    s_zn_need[s_ed_nzn] = s_zn_live[s_ed_nzn] = s_zn_k0[s_ed_nzn] = 0;
    ed_save();
}
/* a soldier of ours was deleted by the user: if it belonged to a zone it counts as killed (else the zone would make it again) */
static int ed_cover_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_ZONE_COVER");
        on = e && *e == '1';
    }
    return on;
}
static int s_ed_zn_wander_on(uint32_t soldier)
{
    int zi, k;
    for (zi = 0; zi < s_ed_nzn; zi++)
        for (k = 0; k < ED_ZMAX; k++)
            if (s_zn_sol[zi][k] == soldier)
                return s_ed_zn[zi].wander;
    return 0;
}
static int ed_zone_wander_of(uint32_t soldier, float *c, float *r, int *cover)
{
    int zi, k;
    for (zi = 0; zi < s_ed_nzn; zi++)
        for (k = 0; k < ED_ZMAX; k++)
            if (s_zn_sol[zi][k] == soldier) {
                memcpy(c, s_ed_zn[zi].p, 12);
                *r = s_ed_zn[zi].r;
                *cover = s_ed_zn[zi].cover && ed_cover_enabled();      /* parked: the combat AI ignores the home point while it fights (his test 2026-11-21); RECOMP_ZONE_COVER=1 turns it back on */
                return s_ed_zn[zi].wander || *cover;     /* 1 = this soldier is moved by the editor (wander and/or cover) */
            }
    return 0;
}
static int ed_zone_note_deleted(uint32_t soldier)
{
    int zi, k;
    for (zi = 0; zi < s_ed_nzn; zi++)
        for (k = 0; k < ED_ZMAX; k++)
            if (s_zn_sol[zi][k] == soldier) {
                s_zn_sol[zi][k] = 0;
                s_zn_killed[zi]++;
                return 1;
            }
    return 0;
}
static int ed_zone_add(const float *p, const char *name)
{
    EdZone *z;
    char m[200];
    if (!name || !name[0]) {
        recomp_osd_show("Editor: click a soldier type in the Characters tab first, then drag the zone into the world");
        return 0;
    }
    if (s_ed_nzn >= ED_NZN) {
        snprintf(m, sizeof m, "Editor: the limit of %d zones is reached (Clear removes them, or right click a zone)", ED_NZN);
        recomp_osd_show(m);
        return 0;
    }
    z = &s_ed_zn[s_ed_nzn];
    memset(z, 0, sizeof *z);
    z->p[0] = p[0]; z->p[1] = p[1]; z->p[2] = p[2];
    z->r = s_zn_def_r;
    z->trig = s_zn_def_trig;
    z->count = s_zn_def_count;
    z->waves = s_zn_def_waves;
    z->wander = s_zn_def_wander;
    z->cover = s_zn_def_cover;
    strncpy(z->name, name, ED_PATH - 1);
    memset(s_zn_sol[s_ed_nzn], 0, sizeof s_zn_sol[0]);
    s_zn_killed[s_ed_nzn] = 0;
    s_zn_pend[s_ed_nzn] = 0;
    s_zn_force[s_ed_nzn] = 1;                   /* the first wave comes at once, so the zone shows what it does */
    s_zn_fail[s_ed_nzn] = 0;
    s_zn_need[s_ed_nzn] = s_zn_live[s_ed_nzn] = s_zn_k0[s_ed_nzn] = 0;
    s_ed_nzn++;
    ed_save();
    snprintf(m, sizeof m, "Editor: zone %d made: %d x %s, %d waves, radius %.0f m, wakes at %.0f m", s_ed_nzn, z->count, ed_base(name), z->waves, (double)z->r, (double)z->trig);
    recomp_osd_show(m);
    return 1;
}
static int ed_zone_type_ok(const char *name)
{
    int ci = ed_cat_find(name), p = ci >= 0 ? ed_sp_slot(s_ed_cat_sp[ci], 0) : -1;
    return p >= 0 && MEM32(s_ed_sp[p]) == 0x30ED54u;
}
static void ed_zone_tick(const float *eye, int fc_on)
{
    uint32_t pc = game_pc_get();
    int zi, k;
    float px, py;
    if (!s_ed_nzn || !s_ed_apply || pc < 0x80000000u)
        return;
    if (!s_zn_t0)
        s_zn_t0 = g_game_frames ? g_game_frames : 1u;
    if (g_game_frames - s_zn_t0 < 360u || s_ed_nsp == 0 || (g_game_frames % 4u) != 0)
        return;                                 /* the level must have run a while and its spawners must have asked for their types */
    px = ed_f(pc + 0x90u);                      /* the player object keeps the live position at +0x90 (found by the aim measurement); +0x60 is NOT it (a soldier layout) */
    py = ed_f(pc + 0x94u);
    if (!(fabsf(px) > 0.001f || fabsf(py) > 0.001f)) {
        px = ed_f(pc + 0x60u);
        py = ed_f(pc + 0x64u);
    }
    for (zi = 0; zi < s_ed_nzn; zi++) {
        EdZone *z = &s_ed_zn[zi];
        int alive = 0, quota = z->count * z->waves, near_;
        float dx = px - z->p[0], dy = py - z->p[1];
        for (k = 0; k < ED_ZMAX; k++) {
            uint32_t s = s_zn_sol[zi][k];
            if (!s)
                continue;
            if (!game_editor_is_placed(s) || !ed_is_soldier(s)) {       /* unloaded by the game, or its address was reused: not a kill */
                s_zn_sol[zi][k] = 0;
                fprintf(stderr, "[ZONE] %d: soldier %08X gone (placed %d, soldier %d): not counted as a kill\n", zi + 1, (unsigned)s, game_editor_is_placed(s), ed_is_soldier(s));
            } else if (!ed_alive(s)) {                                  /* shot: counts against the quota (the body is cleaned up by ed_spawn's body limit) */
                s_zn_sol[zi][k] = 0;
                s_zn_killed[zi]++;
                fprintf(stderr, "[ZONE] %d: soldier %08X killed (%d of %d)\n", zi + 1, (unsigned)s, s_zn_killed[zi], quota);
            } else {
                alive++;
            }
        }
        near_ = s_zn_force[zi] || dx * dx + dy * dy <= z->trig * z->trig;
        if (!near_ && fc_on && eye) {                   /* the free camera counts too: the editor flies it, the player's body may stand far away */
            float ex = eye[0] - z->p[0], ey = eye[1] - z->p[1];
            near_ = ex * ex + ey * ey <= z->trig * z->trig;
        }
        {
            int inside = near_ && !s_zn_force[zi];      /* inside the wake distance (player, or the free camera while it flies) */
            if (alive == 0 && s_zn_pend[zi] == 0 && s_zn_live[zi]) {        /* the wave is over */
                s_zn_live[zi] = 0;
                if (s_zn_killed[zi] > s_zn_k0[zi]) {    /* shot down: the next wave waits until the player has been OUT of the wake distance */
                    s_zn_need[zi] = 1;
                    fprintf(stderr, "[ZONE] %d: wave over (killed %d of %d): the next one needs you to leave the wake distance (%.0f m) first\n", zi + 1, s_zn_killed[zi], quota,
                            (double)z->trig);
                }
            }
            if (s_zn_need[zi] && !inside) {
                s_zn_need[zi] = 0;
                fprintf(stderr, "[ZONE] %d: you left the wake distance, the next wave is armed\n", zi + 1);
            }
        }
        if (s_zn_pend[zi] == 0 && alive == 0 && s_zn_killed[zi] < quota && near_ && !s_zn_need[zi]) {
            s_zn_pend[zi] = quota - s_zn_killed[zi] < z->count ? quota - s_zn_killed[zi] : z->count;
            s_zn_fail[zi] = 0;
            s_zn_live[zi] = 1;
            s_zn_k0[zi] = s_zn_killed[zi];
            fprintf(stderr, "[ZONE] %d: wave starts, %d soldiers (killed so far %d of %d)\n", zi + 1, s_zn_pend[zi], s_zn_killed[zi], quota);
        } else if (alive == 0 && s_zn_pend[zi] == 0 && s_zn_killed[zi] < quota && !s_zn_need[zi] && (g_game_frames % 240u) == 0) {
            float d = sqrtf(dx * dx + dy * dy);          /* a wave is due but nobody is near: say so (every 4 s) */
            char m[120];
            fprintf(stderr, "[ZONE] %d: next wave waits, you are %.0f m from it (wake distance %.0f m)\n", zi + 1, (double)d, (double)z->trig);
            snprintf(m, sizeof m, "Zone %d: next wave waits (%.0f m away, wakes at %.0f m)", zi + 1, (double)d, (double)z->trig);
            recomp_osd_show(m);
        }
        if (s_zn_pend[zi] > 0 && near_) {
            int free_k = -1;
            for (k = 0; k < ED_ZMAX; k++)
                if (!s_zn_sol[zi][k]) {
                    free_k = k;
                    break;
                }
            if (free_k < 0) {
                s_zn_pend[zi] = 0;
            } else if (!ed_zone_type_ok(z->name)) {                     /* the type is not loaded in this world yet (or its spawner is gone): wait, never fall back to another type */
                if ((g_game_frames % 240u) == 0)
                    fprintf(stderr, "[ZONE] %d waits: type %s is not loaded in this world / its spawner is gone\n", zi + 1, ed_base(z->name));
            } else {
                float ang = ed_zrand() * 6.2831853f, rad = z->r * sqrtf(ed_zrand()), pt[3], from[3], dn[3] = { 0.0f, 0.0f, -1.0f }, hit[3];
                uint32_t made;
                pt[0] = z->p[0] + cosf(ang) * rad;
                pt[1] = z->p[1] + sinf(ang) * rad;
                pt[2] = z->p[2] + 0.3f;
                from[0] = pt[0]; from[1] = pt[1]; from[2] = z->p[2] + 1.5f;
                if (ed_world_point(from, dn, hit))      /* the floor under that point */
                    pt[2] = hit[2] + 0.3f;
                s_ed_capped = 0;
                made = ed_spawn(0, pt, z->name, 0, 0);
                if (made) {
                    fprintf(stderr, "[ZONE] %d: soldier %08X made at %.1f %.1f %.1f (%d still to make)\n", zi + 1, (unsigned)made, (double)pt[0], (double)pt[1], (double)pt[2],
                            s_zn_pend[zi] - 1);
                    s_zn_sol[zi][free_k] = made;
                    s_zn_pend[zi]--;
                    s_zn_fail[zi] = 0;
                    if (s_zn_pend[zi] == 0)
                        s_zn_force[zi] = 0;
                } else if (s_ed_capped || ++s_zn_fail[zi] >= 30) {       /* the limit of editor soldiers, or the spawner keeps refusing: give this batch up, try again later */
                    s_zn_pend[zi] = 0;
                    s_zn_force[zi] = 0;
                    fprintf(stderr, "[ZONE] %d batch dropped (%s)\n", zi + 1, s_ed_capped ? "limit of editor soldiers" : "spawner made nothing");
                }
            }
        }
    }
}

/* SPAWNER CENSUS (Numpad9 with RECOMP_EDITOR_LOG=1): every spawner object (vtable 0x30ED54) in the guest heap, not only the ones whose handler ran.
 * Per spawner: address, counters (+0x40 spawned, +0x44 killed, +0x48 pool), team, type file if known, the locator (+0x1D8 -> +0x60 position), the record
 * (+0x1B8 -> +0x20 spawn point) and a dump of that record, plus every object the spawner points at that holds something that looks like a world position
 * (these are the spawn points / post nodes: do they form a circle, a line, or scattered points?). The scan stops at 0x84000000 minus the read size. */
static int ed_pos_like(uint32_t a)
{
    float x = ed_f(a), y = ed_f(a + 4), z = ed_f(a + 8);
    return x == x && y == y && z == z && fabsf(x) < 3000.0f && fabsf(y) < 3000.0f && fabsf(z) < 600.0f && (fabsf(x) > 0.5f || fabsf(y) > 0.5f)
           && fabsf(x) + fabsf(y) > 2.0f && fabsf(x) > 1e-3f && fabsf(y) > 1e-3f;
}
static void ed_spawner_census(const float *eye)
{
    uint32_t a, lim = 0x84000000u - 0x400u, found[400];
    int n = 0, i;
    for (a = 0x80000000u; a < lim && n < 400; a += 4)
        if (MEM32(a) == 0x30ED54u)
            found[n++] = a;
    fprintf(stderr, "[SPAWNERS] %d spawner objects in the heap (camera %.1f %.1f %.1f)\n", n, (double)eye[0], (double)eye[1], (double)eye[2]);
    for (i = 0; i < n; i++) {
        uint32_t sp = found[i], f, o;
        int slot = ed_sp_slot(sp, 0), lines = 0;
        fprintf(stderr, "[SPAWNERS] #%d %08X: +8 %08X spawned %u killed %u pool %u team %d type '%s'\n", i, (unsigned)sp, (unsigned)MEM32(sp + 8u), (unsigned)MEM32(sp + 0x40u),
                (unsigned)MEM32(sp + 0x44u), (unsigned)MEM32(sp + 0x48u), game_editor_spawner_team(sp), slot >= 0 ? s_ed_name[slot] : "(never asked for a type)");
        for (f = 0x30; f < 0x80u && lines < 8; f += 4) {         /* objects this spawner points at that hold a world-position-like triple (its own 0x80 bytes only: the next object starts there) */
            uint32_t b = MEM32(sp + f);
            if (!ed_ptr(b) || b == sp)
                continue;
            for (o = 0; o < 0x200u && b + o + 12u < 0x84000000u; o += 4)
                if (ed_pos_like(b + o)) {
                    fprintf(stderr, "[SPAWNERS] #%d spawner+%02X -> %08X +%03X: %.2f %.2f %.2f\n", i, (unsigned)f, (unsigned)b, (unsigned)o, (double)ed_f(b + o),
                            (double)ed_f(b + o + 4u), (double)ed_f(b + o + 8u));
                    lines++;
                    break;
                }
        }
    }
    {
        char m[96];
        snprintf(m, sizeof m, "Editor: %d spawners found (see the log)", n);
        recomp_osd_show(m);
    }
}

/* PROPS RESEARCH (Numpad9, free camera on): a census of the world's creation list. Per object kind (the vtable word) how many there are, and every
 * object within 8 m of the camera with its address and the floats at +0x60..+0x68 (where the soldier keeps its position). Fly next to a medkit /
 * weapon pickup / mounted gun and press it: the log shows which object kind the pickup is, which tells which constructor (and creator) makes it. */
static void ed_census(const float *eye)
{
    uint32_t n = MEM32(0x3A0338), vt[64];
    int cnt[64], nk = 0, guard = 0, k, near_n = 0;
    while (n >= 0x10000u && n < 0x90000000u && guard++ < 16384) {
        uint32_t o = MEM32(n + 0x18), v;
        if (ed_ptr(o)) {
            float x = ed_f(o + 0x60), y = ed_f(o + 0x64), z = ed_f(o + 0x68);
            v = MEM32(o);
            for (k = 0; k < nk && vt[k] != v; k++)
                ;
            if (k == nk && nk < 64) {
                vt[nk] = v;
                cnt[nk++] = 0;
            }
            if (k < nk)
                cnt[k]++;
            if (x == x && y == y && z == z && fabsf(x - eye[0]) < 8.0f && fabsf(y - eye[1]) < 8.0f && fabsf(z - eye[2]) < 8.0f && near_n < 40) {
                fprintf(stderr, "[CENSUS] near: object %08X vtable %08X at %.2f %.2f %.2f (camera %.2f %.2f %.2f) node %08X\n", (unsigned)o, (unsigned)v,
                        (double)x, (double)y, (double)z, (double)eye[0], (double)eye[1], (double)eye[2], (unsigned)n);
                near_n++;
            }
        }
        n = MEM32(n + 0xC);
    }
    fprintf(stderr, "[CENSUS] %d nodes, %d object kinds, %d objects within 8 m\n", guard, nk, near_n);
    for (k = 0; k < nk; k++)
        fprintf(stderr, "[CENSUS] kind vtable %08X x %d\n", (unsigned)vt[k], cnt[k]);
    ed_str_scan("JF_WeaponPowerup_medkit_01");
    ed_str_scan("Powerups\\Medkit.xdd");
    ed_heap_scan(eye);
    recomp_osd_show("Editor: census written to the log");
}

/* ---------- once a frame (from fc_view, recomp_manual.c): eye / fwd of the free camera, valid when fc_on ---------- */
static void ed_ui_frame(const float *eye, const float *fwd, int fc_on);   /* map_editor_ui.c, included at the end */
static volatile LONG s_ui_show;                 /* the window is (to be) up */
void game_editor_tick(const float *eye, const float *fwd, int fc_on)
{
    static int dn[6], vk[6];
    static const int k_def[6] = { VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD5, VK_NUMPAD9 };
    int i, k;
    ed_run_watch();
    if (!ed_on() && s_ed_replay == 0 && !s_ed_anyslot && s_ed_pr == 0 && s_ed_nzn == 0) {
        if (s_ui_show)
            ed_ui_frame(eye, fwd, fc_on);       /* the editor was switched off while its window was up: take it down */
        return;                                 /* editor off and nothing to do: one test a frame */
    }
    ed_ui_frame(eye, fwd, fc_on);
    if (!vk[0]) {
        const char *e = getenv("RECOMP_EDITOR_KEYS");
        for (i = 0; i < 6; i++)
            vk[i] = k_def[i];
        if (e && e[0]) {
            int a[6], got = sscanf(e, "%d,%d,%d,%d,%d,%d", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]);
            for (i = 0; i < got && i < 6; i++)
                if (a[i] > 0 && a[i] < 256)
                    vk[i] = a[i];
        }
    }
    ed_slots();
    if (s_ed_enabled && s_ed_npl > 0 && (g_game_frames % 15u) == 0)
        ed_reap_dead();
    if (s_ed_replay == 1) {                     /* wait until the level has been running for a few seconds and its spawners have ticked */
        if (!s_ed_t0 && game_pc_get() >= 0x80000000u)
            s_ed_t0 = g_game_frames ? g_game_frames : 1u;
        if (s_ed_t0 && g_game_frames - s_ed_t0 > 360u && s_ed_nsp > 0) {
            s_ed_replay = 2;
            s_ed_replay_i = 0;
            s_ed_gap = 0;
        }
    }
    ed_zone_tick(eye, fc_on);
    if (s_ed_dbg && (g_game_frames & 255u) == 0)        /* (s_ed_t0 is only set while a replay waits: not usable here) */
        ed_pt_sync_wtable();                            /* logs [WTABLE] when the weapon table changes */
    if (s_ed_replay == 2 && --s_ed_gap <= 0) {  /* one soldier every few frames: each makes a small hitch */
        s_ed_gap = 4;
        if (s_ed_replay_i >= s_ed_npl) {
            s_ed_replay = 0;
            if (s_ed_dbg)
                fprintf(stderr, "[EDITOR] replay done\n");
        } else {
            EdPlace *e = &s_ed_pl[s_ed_replay_i];
            uint32_t made;
            s_ed_capped = 0;
            made = ed_spawn(e->team, e->p, e->name, 0, 0);
            if (made) {
                s_ed_pl_sol[s_ed_replay_i] = made;
                s_ed_replay_i++;
                s_ed_tries = 0;
            } else if (s_ed_capped) {           /* the limit: the replay ends here (it must not wait and refill as the player kills soldiers) */
                s_ed_replay = 0;
                fprintf(stderr, "[EDITOR] replay stopped at line %d: the limit of %d soldiers\n", s_ed_replay_i + 1, ed_cap());
            } else if (++s_ed_tries >= 60) {    /* a spawner's team is learned only once it has made a request: try again for a while, then skip the line */
                fprintf(stderr, "[EDITOR] replay line %d could not be made, skipped\n", s_ed_replay_i + 1);
                s_ed_replay_i++;
                s_ed_tries = 0;
            } else {
                s_ed_gap = 60;
            }
        }
    }
    if (s_ed_dbg && s_ed_enabled && fc_on) {    /* NUMPAD4 (research): the cross-level probe, the first template's arguments with a hash of a kind only another level made */
        static int fdn, fnext;
        int down = (GetAsyncKeyState(VK_NUMPAD4) & 0x8000) != 0;
        if (down && !fdn) {
            int n, tries;
            for (tries = 0; tries < s_pt_gn; tries++) {
                int cand = (fnext + tries) % s_pt_gn, t, have = 0;
                for (t = 0; t < s_ptpl_n; t++)
                    if (s_ptpl[t].hash == s_pt_g[cand].hash)
                        have = 1;
                if (!have && s_ptpl_n > 0) {
                    float pt[3];
                    uint32_t r;
                    for (n = 0; n < 3; n++)
                        pt[n] = eye[n] + fwd[n] * 4.0f;
                    fprintf(stderr, "[EDITOR] PROBE: foreign kind %s hash %08X with template %s's arguments ...\n", s_pt_g[cand].path, (unsigned)s_pt_g[cand].hash, s_ptpl[0].path);
                    fflush(stderr);
                    r = ed_make_prop_h(0, s_pt_g[cand].hash, s_pt_g[cand].cnt, pt);
                    fprintf(stderr, "[EDITOR] PROBE: the creator returned %08X (inner object %08X)\n", (unsigned)r, (unsigned)s_pt_last_inner);
                    fnext = cand + 1;
                    break;
                }
            }
            if (tries >= s_pt_gn)
                fprintf(stderr, "[EDITOR] PROBE: no foreign kind known (%d in the global catalog, %d in this level)\n", s_pt_gn, s_ptpl_n);
        }
        fdn = down;
    }
    if (s_ed_dbg && s_ed_enabled && fc_on) {    /* NUMPAD1 (research): a Medkit where the crosshair points */
        static int pdn;
        int down = (GetAsyncKeyState(VK_NUMPAD1) & 0x8000) != 0;
        if (down && !pdn) {
            float pt[3], dd[3];
            for (i = 0; i < 3; i++)
                dd[i] = fwd[i];
            if (!ed_world_point(eye, dd, pt))
                for (i = 0; i < 3; i++)
                    pt[i] = eye[i] + fwd[i] * 5.0f;
            pt[2] += 0.2f;
            if (s_ptpl_n > 0) {                 /* research: the first kind this level made (a medkit if it has one) */
                int t, pick = 0;
                for (t = 0; t < s_ptpl_n; t++)
                    if (strstr(s_ptpl[t].path, "edkit"))
                        pick = t;
                ed_make_prop(pick, pt);
            }
        }
        pdn = down;
    }
    if (s_ed_dbg && !s_pt_probed && s_ptpl_n > 0 && game_pc_get() >= 0x80000000u) {   /* debug log: which catalog kinds does THIS level hold (once, a while after the start) */
        if (!s_pt_pt0)
            s_pt_pt0 = g_game_frames ? g_game_frames : 1u;
        if (g_game_frames - s_pt_pt0 > 400u) {
            int q;
            s_pt_probed = 1;
            for (q = 0; q < s_pt_gn; q++)
                ed_kind_resident(s_pt_g[q].hash);
        }
    }
    if (s_ed_pr == 1 && s_ed_enabled) {                         /* the saved props: wait until the level has run a while (its powerup nodes have made their pickups) */
        if (!s_ed_pr_t0 && game_pc_get() >= 0x80000000u)
            s_ed_pr_t0 = g_game_frames ? g_game_frames : 1u;
        if (s_ed_pr_t0 && g_game_frames - s_ed_pr_t0 > 300u) {
            s_ed_pr = 2;
            s_ed_pr_i = 0;
            s_ed_pr_tries = 0;
        }
    }
    if (s_ed_pr == 2 && s_ed_enabled) {                         /* one prop per frame is fine (no soldier-sized hitch), a missing template is waited for a few seconds */
        if (s_ed_pr_i >= s_ed_npp) {
            s_ed_pr = 0;
            if (s_ed_dbg)
                fprintf(stderr, "[EDITOR] prop replay done (%d props)\n", s_ed_npp);
        } else if (ed_place_prop(s_ed_pp[s_ed_pr_i].path, s_ed_pp[s_ed_pr_i].p, 0)) {
            s_ed_pp_obj[s_ed_pr_i] = s_pt_last_inner;
            s_ed_pr_i++;
            s_ed_pr_tries = 0;
        } else if (++s_ed_pr_tries >= 600) {
            fprintf(stderr, "[EDITOR] prop %s could not be placed again (the level made none of that kind): skipped\n", s_ed_pp[s_ed_pr_i].path);
            s_ed_pr_i++;
            s_ed_pr_tries = 0;
        }
    }
    for (k = 0; k < 6; k++) {
        int down = s_ed_enabled && fc_on && (GetAsyncKeyState(vk[k]) & 0x8000) != 0;
        if (down && !dn[k]) {
            float tgt[3];
            char msg[256];
            for (i = 0; i < 3; i++)
                tgt[i] = eye[i] + fwd[i] * 6.0f;
            if (k <= 1) {                       /* place: NUMPAD2 = the chosen type (else any enemy), NUMPAD3 = the next friendly type (else any friend) */
                const char *want = NULL;
                int team = k == 0 ? 2 : 1, c;
                uint32_t s;
                if (k == 0) {
                    if (s_ed_sel >= 0 && s_ed_sel < s_ed_ncat)
                        want = s_ed_cat[s_ed_sel];
                } else {
                    static int fr;
                    for (c = 0; c < s_ed_ncat && !want; c++) {
                        int j = (fr + c) % s_ed_ncat;
                        int p = ed_sp_slot(s_ed_cat_sp[j], 0);
                        if (p >= 0 && game_editor_spawner_team(s_ed_sp[p]) == 1) {
                            want = s_ed_cat[j];
                            fr = j + 1;
                        }
                    }
                }
                s = ed_spawn(team, tgt, want, 1, 1);
                if (s) {
                    s_ed_last_sol = s;
                    snprintf(msg, sizeof msg, "Editor: %s placed: %s (%d of %d placed)", team == 2 ? "enemy" : "friend",
                             want ? ed_base(want) : "spawner's own type", ed_shown(), ed_cap());
                    recomp_osd_show(msg);
                } else {
                    int nsp = 0, t;
                    for (t = 0; t < s_ed_nsp; t++)
                        if (MEM32(s_ed_sp[t]) == 0x30ED54u && game_editor_spawner_team(s_ed_sp[t]) == team)
                            nsp++;
                    snprintf(msg, sizeof msg, "Editor: could not make a %s here (%d spawners of that team known, %d types, %d spawners in all)",
                             team == 2 ? "enemy" : "friend", nsp, s_ed_ncat, s_ed_nsp);
                    recomp_osd_show(msg);
                }
            } else if (k == 2) {                /* the one delete key: the soldier in the middle of the screen */
                ed_delete_aimed(eye, fwd, 0.985f);
            } else if (k == 5) {                /* NUMPAD9: props research census (only with RECOMP_EDITOR_LOG=1) */
                if (s_ed_dbg) {
                    ed_spawner_census(eye);
                    ed_ctor_near(eye);
                    ed_census(eye);
                }
            } else {                            /* NUMPAD8 / NUMPAD5: next / previous soldier type of this level's catalog */
                if (s_ed_ncat == 0) {
                    recomp_osd_show("Editor: no soldier type known yet (walk around until enemies appear)");
                } else {
                    int p, st = 0;
                    s_ed_sel = k == 3 ? (s_ed_sel + 1) % s_ed_ncat : (s_ed_sel <= 0 ? s_ed_ncat - 1 : s_ed_sel - 1);
                    s_ed_selprop[0] = 0;
                    p = ed_sp_slot(s_ed_cat_sp[s_ed_sel], 0);
                    if (p >= 0)
                        st = game_editor_spawner_team(s_ed_sp[p]);
                    snprintf(msg, sizeof msg, "Editor type %d/%d: %s (%s)", s_ed_sel + 1, s_ed_ncat, ed_base(s_ed_cat[s_ed_sel]),
                             st == 2 ? "enemy" : st == 1 ? "friend" : "team unknown");
                    recomp_osd_show(msg);
                }
            }
        }
        dn[k] = down;
    }
}

#include "map_editor_ui.c"
