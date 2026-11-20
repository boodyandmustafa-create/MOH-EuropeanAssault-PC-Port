/*
 * game_sfx.c -- sounds of our own, played beside the game's audio (compiled as part of game_patches.c, after ai_tuning.c).
 *
 *   Pain / scream sounds   A soldier (enemy or friend) is hit by a bullet -> a pain sound; the hit that kills him -> a scream.
 *                          The player's health float (player + 0x3B0) drops -> a pain sound. The files are the user's own:
 *                            mods\sounds\pain\soldier\*.wav   soldier hit, still alive   (also mods\<pack>\sounds\pain\soldier)
 *                            mods\sounds\pain\death\*.wav     soldier killed (falls back to the soldier folder when empty)
 *                            mods\sounds\pain\player\*.wav    the player is hurt
 *                          Any PCM or float WAV (8/16/24/32 bit, mono or stereo, any rate) is read once at the first hit and
 *                          converted to 48 kHz stereo. With no files nothing plays. A random clip is chosen, never the same one
 *                          twice in a row. The sound goes to the effect voices of the XAudio2 backend (xa2_sfx_play), which
 *                          XAudio2 mixes on its own thread: no cost for the game thread beyond one call per hit.
 *                          A soldier's sound is quieter the farther he is (position at object + 0x90, like the player's).
 *   Scope sway             While a scope is up and the player's health is under the game's own low-health mark (a quarter of the
 *                          maximum: the constant at 0x2FF224 that sub_00118640 compares health / max against), the aim moves in a
 *                          slow wave. game_sway_take returns the CHANGE of that wave since the last call, added to the look
 *                          update's input (aim_inject_yaw / aim_inject_pitch), so the wave comes from the clock, not from the
 *                          frame rate, never drifts and fades in and out over a third of a second.
 *
 *   RECOMP_PAIN_SOUNDS=0/1 (ini pain_sounds)  RECOMP_PAIN_VOLUME=10..100 (ini pain_volume)
 *   RECOMP_SCOPE_SWAY=0/1 (ini scope_sway)    RECOMP_SCOPE_SWAY_STRENGTH=1..5 (ini scope_sway_strength)
 */
extern int  xa2_sfx_play(const int16_t *stereo48k, int frames, float vol, float pan, int loop);
extern void xa2_sfx_stop(int handle);
extern int  xa2_sfx_breath_busy(void);
extern int  xa2_sfx_breath_play(const int16_t *stereo48k, int frames, float vol);
extern void xa2_sfx_breath_volume(float vol);
extern void xa2_sfx_breath_stop(void);
void game_breath_stop(void);
extern int  game_scope_up(void);                 /* recomp_manual.c */
extern float game_zoom_scale(void);

#define SFX_MAXCLIPS 24
#define SFX_RATE     48000

typedef struct { int16_t *pcm; int frames; } SfxClip;
typedef struct { SfxClip c[SFX_MAXCLIPS]; int n, last; } SfxPool;
enum { SP_PLAYER, SP_SOLDIER, SP_DEATH, SP_BREATH, SP_COUNT };
static SfxPool s_sp[SP_COUNT];
static int     s_sfx_loaded;

static unsigned sfx_u16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned sfx_u32(const unsigned char *p) { return sfx_u16(p) | (sfx_u16(p + 2) << 16); }

/* one sample of frame idx, channel c, as a float in -1..1 */
static float sfx_smp(const unsigned char *d, int idx, int ch, int c, int bits, int is_float)
{
    const unsigned char *p = d + ((size_t)idx * (size_t)ch + (size_t)c) * (size_t)(bits / 8);
    if (is_float) {
        float v;
        memcpy(&v, p, 4);
        return v;
    }
    switch (bits) {
    case 8:  return ((float)p[0] - 128.0f) / 128.0f;
    case 16: return (float)(int16_t)sfx_u16(p) / 32768.0f;
    case 24: return (float)((int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24)) >> 8) / 8388608.0f;
    default: return (float)((double)(int32_t)sfx_u32(p) / 2147483648.0);
    }
}

static int sfx_load_wav(const char *path, SfxClip *out)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long sz, pos;
    unsigned tag = 0, ch = 0, rate = 0, bits = 0;
    const unsigned char *data = NULL;
    size_t dlen = 0;
    int is_float, n_in, n_out, i, ok = 0;
    int16_t *pcm;

    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 44 || sz > 40L * 1024 * 1024 || !(b = (unsigned char *)malloc((size_t)sz))) {
        fclose(f);
        return 0;
    }
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(b);
        return 0;
    }
    fclose(f);
    if (memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) {
        free(b);
        return 0;
    }
    for (pos = 12; pos + 8 <= sz;) {
        size_t len = sfx_u32(b + pos + 4), body = (size_t)pos + 8;
        if (len > (size_t)sz - body)
            len = (size_t)sz - body;                    /* a data chunk with a wrong size: take what is there */
        if (!memcmp(b + pos, "fmt ", 4) && len >= 16) {
            tag = sfx_u16(b + body);
            ch = sfx_u16(b + body + 2);
            rate = sfx_u32(b + body + 4);
            bits = sfx_u16(b + body + 14);
            if (tag == 0xFFFEu && len >= 26)
                tag = sfx_u16(b + body + 24);           /* extensible: the sub-format's first word */
        } else if (!memcmp(b + pos, "data", 4)) {
            data = b + body;
            dlen = len;
            break;
        }
        pos = (long)(body + len + (len & 1u));
    }
    is_float = tag == 3u && bits == 32u;
    if (!data || !((tag == 1u && (bits == 8u || bits == 16u || bits == 24u || bits == 32u)) || is_float) || ch < 1u || ch > 2u ||
        rate < 4000u || rate > 192000u) {
        free(b);
        return 0;
    }
    n_in = (int)(dlen / (ch * bits / 8u));
    if (n_in < 2) {
        free(b);
        return 0;
    }
    n_out = (int)((double)n_in * SFX_RATE / rate);
    if (n_out > SFX_RATE * 20)
        n_out = SFX_RATE * 20;                          /* a hit sound is never longer */
    pcm = (int16_t *)malloc((size_t)n_out * 4u);
    if (pcm) {
        for (i = 0; i < n_out; i++) {
            double src = (double)i * rate / SFX_RATE;
            int i0 = (int)src, i1;
            float fr, l0, l1, r0, r1, l, r;
            if (i0 > n_in - 1)
                i0 = n_in - 1;
            i1 = i0 + 1 < n_in ? i0 + 1 : i0;
            fr = (float)(src - i0);
            l0 = sfx_smp(data, i0, (int)ch, 0, (int)bits, is_float);
            l1 = sfx_smp(data, i1, (int)ch, 0, (int)bits, is_float);
            r0 = ch > 1u ? sfx_smp(data, i0, (int)ch, 1, (int)bits, is_float) : l0;
            r1 = ch > 1u ? sfx_smp(data, i1, (int)ch, 1, (int)bits, is_float) : l1;
            l = l0 + (l1 - l0) * fr;
            r = r0 + (r1 - r0) * fr;
            l = l > 1.0f ? 1.0f : l < -1.0f ? -1.0f : l;
            r = r > 1.0f ? 1.0f : r < -1.0f ? -1.0f : r;
            pcm[i * 2] = (int16_t)(l * 32767.0f);
            pcm[i * 2 + 1] = (int16_t)(r * 32767.0f);
        }
        out->pcm = pcm;
        out->frames = n_out;
        ok = 1;
    }
    free(b);
    return ok;
}

static void sfx_scan_dir(int pool, const char *dir)
{
    char pat[MAX_PATH + 8], path[MAX_PATH * 2 + 8];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    snprintf(pat, sizeof pat, "%s\\*.wav", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (s_sp[pool].n >= SFX_MAXCLIPS)
            break;
        snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
        if (sfx_load_wav(path, &s_sp[pool].c[s_sp[pool].n]))
            s_sp[pool].n++;
        else
            fprintf(stderr, "[PAIN] %s: not a plain PCM / float WAV, skipped\n", path);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* mods\sounds\<sub> and mods\<pack>\sounds\<sub> */
static void sfx_scan_pool(int pool, const char *sub)
{
    const char *e = getenv("RECOMP_MODS"), *d = getenv("RECOMP_MODS_DIR");
    char root[MAX_PATH], path[MAX_PATH * 2 + 8], pat[MAX_PATH + 8];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    if (e && e[0] == '0')
        return;
    snprintf(root, sizeof root, "%s", d && d[0] ? d : "mods");
    snprintf(path, sizeof path, "%s\\sounds\\%s", root, sub);
    sfx_scan_dir(pool, path);
    snprintf(pat, sizeof pat, "%s\\*", root);
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') {
                snprintf(path, sizeof path, "%s\\%s\\sounds\\%s", root, fd.cFileName, sub);
                sfx_scan_dir(pool, path);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
}

static void sfx_load_all(void)
{
    s_sfx_loaded = 1;
    srand((unsigned)GetTickCount());
    sfx_scan_pool(SP_PLAYER, "pain\\player");
    sfx_scan_pool(SP_SOLDIER, "pain\\soldier");
    sfx_scan_pool(SP_DEATH, "pain\\death");
    sfx_scan_pool(SP_BREATH, "breath");
    fprintf(stderr, "[PAIN] files: %d player, %d soldier, %d death (mods\\sounds\\pain\\player | soldier | death); [BREATH] %d file(s) in mods\\sounds\\breath\n",
            s_sp[SP_PLAYER].n, s_sp[SP_SOLDIER].n, s_sp[SP_DEATH].n, s_sp[SP_BREATH].n);
}

static int sfx_play_pool(int pool, float vol)
{
    SfxPool *p = &s_sp[pool];
    int k;

    if (!p->n)
        return 0;
    k = rand() % p->n;
    if (p->n > 1 && k == p->last)
        k = (k + 1 + rand() % (p->n - 1)) % p->n;        /* never the same clip twice in a row */
    p->last = k;
    return xa2_sfx_play(p->c[k].pcm, p->c[k].frames, vol, 0.0f, 0) != 0;
}

/* ---- settings ---- */
static int s_pain_on = -1, s_pain_vol = -1, s_sway_on = -1, s_sway_str = -1, s_br_on = -1, s_br_vol = -1;

static void sfx_cfg(void)
{
    const char *e;
    if (s_pain_on >= 0)
        return;
    e = getenv("RECOMP_PAIN_SOUNDS");
    s_pain_on = !(e && e[0] == '0');
    e = getenv("RECOMP_PAIN_VOLUME");
    s_pain_vol = e && e[0] ? atoi(e) : 80;
    s_pain_vol = s_pain_vol < 10 ? 10 : s_pain_vol > 100 ? 100 : s_pain_vol;
    e = getenv("RECOMP_SCOPE_SWAY");
    s_sway_on = !(e && e[0] == '0');
    e = getenv("RECOMP_SCOPE_SWAY_STRENGTH");
    s_sway_str = e && e[0] ? atoi(e) : 3;
    s_sway_str = s_sway_str < 1 ? 1 : s_sway_str > 5 ? 5 : s_sway_str;
    e = getenv("RECOMP_BREATH");
    s_br_on = !(e && e[0] == '0');
    e = getenv("RECOMP_BREATH_VOLUME");
    s_br_vol = e && e[0] ? atoi(e) : 70;
    s_br_vol = s_br_vol < 10 ? 10 : s_br_vol > 100 ? 100 : s_br_vol;
}
int  game_breath_get(void)          { sfx_cfg(); return s_br_on; }
void game_breath_set(int on)        { sfx_cfg(); s_br_on = on ? 1 : 0; }
int  game_breathvol_get(void)       { sfx_cfg(); return s_br_vol; }
void game_breathvol_set(int v)      { sfx_cfg(); s_br_vol = v < 10 ? 10 : v > 100 ? 100 : v; }
int  game_pain_get(void)            { sfx_cfg(); return s_pain_on; }
void game_pain_set(int on)          { sfx_cfg(); s_pain_on = on ? 1 : 0; }
int  game_painvol_get(void)         { sfx_cfg(); return s_pain_vol; }
void game_painvol_set(int v)        { sfx_cfg(); s_pain_vol = v < 10 ? 10 : v > 100 ? 100 : v; }
int  game_sway_get(void)            { sfx_cfg(); return s_sway_on; }
void game_sway_set(int on)          { sfx_cfg(); s_sway_on = on ? 1 : 0; }
int  game_swaystr_get(void)         { sfx_cfg(); return s_sway_str; }
void game_swaystr_set(int v)        { sfx_cfg(); s_sway_str = v < 1 ? 1 : v > 5 ? 5 : v; }

/* ---- breathing (called every frame from sprint_apply with the sprint state) ----
 * Starts after about 1.5 s of continuous sprinting, goes on while the player is worn out (the stamina is spent and has not
 * recovered to half), and fades out over 0.6 s when that ends. One clip of mods\sounds\breath at a time, chosen at random and
 * never the same twice in a row; when a clip ends and the breathing is still wanted, the next one follows. */
static float s_br_run_t, s_br_vol_now;

void game_breath_stop(void)
{
    if (s_br_vol_now > 0.0f || xa2_sfx_breath_busy()) {
        xa2_sfx_breath_stop();
        s_br_vol_now = 0.0f;
    }
    s_br_run_t = 0.0f;
}

void game_breath_tick(int running, int tired, float dt)
{
    int want;
    float target, base;

    if (s_br_on < 0)
        sfx_cfg();
    if (!s_sfx_loaded)
        sfx_load_all();
    if (!s_br_on || !s_sp[SP_BREATH].n || recomp_menu_up()) {
        if (s_br_vol_now > 0.0f)
            game_breath_stop();
        return;
    }
    s_br_run_t = running ? s_br_run_t + dt : 0.0f;
    want = s_br_run_t >= 1.5f || tired;
    target = want ? 1.0f : 0.0f;
    if (s_br_vol_now < target) {
        s_br_vol_now += dt / 0.4f;
        if (s_br_vol_now > target) s_br_vol_now = target;
    } else if (s_br_vol_now > target) {
        s_br_vol_now -= dt / 0.6f;
        if (s_br_vol_now < target) s_br_vol_now = target;
    }
    base = (float)s_br_vol / 100.0f;
    if (want && !xa2_sfx_breath_busy()) {
        SfxPool *p = &s_sp[SP_BREATH];
        int k = rand() % p->n;
        if (p->n > 1 && k == p->last)
            k = (k + 1 + rand() % (p->n - 1)) % p->n;
        p->last = k;
        xa2_sfx_breath_play(p->c[k].pcm, p->c[k].frames, s_br_vol_now * base);
        return;
    }
    if (s_br_vol_now > 0.0f)
        xa2_sfx_breath_volume(s_br_vol_now * base);
    else if (xa2_sfx_breath_busy())
        game_breath_stop();                                         /* faded out: cut the rest of the clip */
}

/* ---- soldier hits (queued by game_meter_add, played from the per-frame poll) ---- */
static struct { uint32_t victim; int death; } s_pq[8];
static int s_pqn;
static struct { uint32_t victim; DWORD t; } s_pcd[16];             /* last pain per victim */

void game_pain_event(uint32_t victim, int death)
{
    if (s_pain_on < 0)
        sfx_cfg();
    if (!s_pain_on || s_pqn >= 8)
        return;
    s_pq[s_pqn].victim = victim;
    s_pq[s_pqn].death = death;
    s_pqn++;
}

/* quieter the farther the soldier is; 0.6 when a position cannot be read */
static float pain_gain(uint32_t victim)
{
    uint32_t pc = game_pc_get();
    float a[3], b[3], d, g;
    int i;

    if (pc < 0x80000000u)
        return 0.6f;
    for (i = 0; i < 3; i++) {
        if (!dm_rdf(pc + 0x90u + 4u * (uint32_t)i, &a[i]) || !dm_rdf(victim + 0x90u + 4u * (uint32_t)i, &b[i]) ||
            !(fabsf(a[i]) < 1e5f) || !(fabsf(b[i]) < 1e5f))
            return 0.6f;
    }
    d = sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
    g = 1.15f - d / 45.0f;
    return g < 0.12f ? 0.12f : g > 1.0f ? 1.0f : g;
}

/* the player's health: updated every poll; the peak seen stands in for the maximum (the game reads it through a vtable slot) */
static float s_hp, s_peak, s_hp_prev, s_hp_drop;
static uint32_t s_hp_pc;
extern void nv2a_gpu_weapon_trigger(int kind, float strength);   /* nv2a_gpu.c: blood on the first-person weapon and hands */

static void hp_update(uint32_t pc, int *dropped)
{
    float hp;
    *dropped = 0;
    if (pc < 0x80000000u || !dm_rdf(pc + 0x3B0u, &hp) || !(hp > -1000.0f) || !(hp < 100000.0f))
        return;
    if (pc != s_hp_pc) {
        s_hp_pc = pc;
        s_peak = s_hp_prev = hp;
    } else {
        if (hp < s_hp_prev - 0.5f) {
            *dropped = 1;
            s_hp_drop = s_hp_prev - hp;
        }
        s_hp_prev = hp;
    }
    if (hp > s_peak)
        s_peak = hp;
    s_hp = hp;
}

void game_pain_poll(void)
{
    static DWORD last_play, last_player;
    uint32_t pc = game_pc_get();
    DWORD now = GetTickCount();
    int i, dropped, up;
    float vol;
    static int logged;

    hp_update(pc, &dropped);
    if (dropped) {                                                  /* the player was hit: blood spots on the gun and hands (its own switch: Weapon lighting) */
        float m = s_peak > 20.0f ? s_peak : 100.0f;
        nv2a_gpu_weapon_trigger(0, 0.3f + 2.0f * s_hp_drop / m);
        { extern void game_stain_player_hurt(float, float); game_stain_player_hurt(s_hp_drop, m); }   /* BLOOD-STAIN */
    }
    sfx_cfg();
    up = recomp_menu_up();
    if (!s_sfx_loaded)
        sfx_load_all();
    if (up)
        game_breath_stop();                                         /* Recomp Settings is open: no breathing under it */
    if (!s_pain_on || up) {
        s_pqn = 0;
        return;
    }
    vol = (float)s_pain_vol / 100.0f;
    for (i = 0; i < s_pqn; i++) {
        uint32_t v = s_pq[i].victim;
        int death = s_pq[i].death, k, slot = -1, pool;
        float g;
        if (!death) {
            for (k = 0; k < 16; k++) {
                if (s_pcd[k].victim == v) { slot = k; break; }
            }
            if (slot >= 0 && (int)(now - s_pcd[slot].t) < 350)
                continue;                                          /* machine-gun fire: one sound per soldier per 350 ms */
            if ((int)(now - last_play) < 70)
                continue;
            if (slot < 0) {
                for (k = 0; k < 16; k++)
                    if (!s_pcd[k].victim || (int)(now - s_pcd[k].t) > 5000) { slot = k; break; }
                if (slot < 0)
                    slot = (int)(now & 15u);
            }
            s_pcd[slot].victim = v;
            s_pcd[slot].t = now;
        }
        g = pain_gain(v);
        pool = death && s_sp[SP_DEATH].n ? SP_DEATH : SP_SOLDIER;
        if (sfx_play_pool(pool, vol * g))
            last_play = now;
        if (logged < 16) {
            logged++;
            fprintf(stderr, "[PAIN] soldier %08X %s gain %.2f -> %s\n", v, death ? "killed" : "hit", (double)g,
                    s_sp[pool].n ? "played" : "no files");
        }
    }
    s_pqn = 0;
    if (dropped && (int)(now - last_player) > 500) {
        last_player = now;
        sfx_play_pool(SP_PLAYER, vol);
        if (logged < 16) {
            logged++;
            fprintf(stderr, "[PAIN] player hurt, health %g of %g -> %s\n", (double)s_hp, (double)s_peak,
                    s_sp[SP_PLAYER].n ? "played" : "no files");
        }
    }
}

/* ---- scope sway (see the file header) ---- */
extern volatile int g_mkb_aim_held;                  /* mkb_input.c: the Aim button (left trigger / right mouse) is down */
extern float game_view_fov_deg(void);                /* recomp_manual.c */

/* the player's maximum health: the difficulty's entry of the max-health table (Green / Veteran / Elite / Hero = 0x3764B0..BC,
 * difficulty index at [[0x3888C0] + 0xE70]); the highest health seen when that cannot be read */
static float sfx_hp_max(void)
{
    uint32_t base = 0, diff = 99;
    float mx = 0.0f;
    if (dm_rd(0x3888C0u, &base) && base >= 0x100000u && base < 0x8000000u && dm_rd(base + 0xE70u, &diff) && diff <= 3u &&
        dm_rdf(0x3764B0u + 4u * diff, &mx) && mx > 1.0f && mx < 100000.0f)
        return mx;
    return s_peak;
}

void game_sway_take(float *dyaw, float *dpitch)
{
    static LARGE_INTEGER freq, last;
    static float env, py, pp;
    LARGE_INTEGER now;
    double t, dt;
    int active;
    float amp, zs, pos_y, pos_p, target;

    *dyaw = 0.0f;
    *dpitch = 0.0f;
    sfx_cfg();
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    t = (double)now.QuadPart / (double)freq.QuadPart;
    dt = last.QuadPart ? (double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart : 0.0;
    last = now;
    if (dt > 0.1)
        dt = 0.1;
    /* aiming = a scope is up (the game's fov under 60) or the Aim button is held: pistols and SMGs have no scope, their
     * aim does not change the fov, so the button is what tells; low health = under a quarter of the maximum */
    active = s_sway_on && (game_scope_up() || g_mkb_aim_held);
    if (active) {
        float mx = sfx_hp_max();
        active = mx > 1.0f && s_hp > 0.0f && s_hp / mx < 0.25f;
    }
    if (!active && env == 0.0f && py == 0.0f && pp == 0.0f)
        return;
    target = active ? 1.0f : 0.0f;
    if (env < target) {
        env += (float)(dt / 0.35);
        if (env > target) env = target;
    } else if (env > target) {
        env -= (float)(dt / 0.35);
        if (env < target) env = target;
    }
    zs = game_zoom_scale();                                        /* a closer zoom needs a smaller turn for the same shake on screen */
    amp = 0.0035f * (float)s_sway_str * (zs > 0.2f ? zs : 0.2f);   /* radians: strength 3 = about 0.6 degrees */
    {   /* a wide view (no scope) shows the same angle much smaller: up to twice as much there */
        float fv = game_view_fov_deg() / 35.0f;
        amp *= fv < 1.0f ? 1.0f : fv > 2.0f ? 2.0f : fv;
    }
    pos_y = env * amp * (float)(sin(6.2831853 * 0.37 * t) + 0.35 * sin(6.2831853 * 0.91 * t + 1.3));
    pos_p = env * amp * 0.7f * (float)(sin(6.2831853 * 0.53 * t + 0.8) + 0.30 * sin(6.2831853 * 1.10 * t));
    *dyaw = pos_y - py;
    *dpitch = pos_p - pp;
    py = pos_y;
    pp = pos_p;
}
