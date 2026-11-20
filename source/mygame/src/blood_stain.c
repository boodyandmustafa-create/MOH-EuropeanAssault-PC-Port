/* BLOOD STAINS (2026-11-20): blood on walls and floor when a soldier is hit, when one dies, and when the player is hurt.
 * Included at the end of game_patches.c (macros, g_* registers and the guest memory helpers live there).
 *
 * It rides on the game's own bullet-hole system (DecalManager, see memory moh-blood-decals-stains.md):
 *   - the manager [0x3C5A78] keeps type records (0x34 bytes, vector at +0x124..+0x128, key at +0, texture object at +4,
 *     size floats at +8/+0xC and a scale range at +0x10/+0x14); a node of a type is taken by sub_000AA880 (thiscall ecx =
 *     manager, args key + &record, ret 8) and filled by sub_000AAD50 (args node, record, hit, flip; ret 16), the same two
 *     calls sub_000AB580 makes for a bullet hit. A hit record is only a normal (+0..8) and a position (+0x10..0x18).
 *   - one rarely used type (key 9DDC3F55, texture "brick", 32x32 P8, used by one material that almost never gets hit) is
 *     taken for blood: its pixels + palette are rewritten in place with a blood splat (the GPU reads the game's memory,
 *     tested live 2026-11-20 by tinting a palette), in the blood colour of the Blood colour setting.
 *   - where to put a stain comes from the game's own world ray sub_002236B0 (thiscall ecx = Havok world [0x3DEA20],
 *     args query + result, ret 8; same layout as the bullet's test in sub_000FA8C0): along the bullet for a wall, straight
 *     down for the floor.
 *   - the node pool (100 nodes at 0x3C3FE8, shared by all bullet holes) is replaced by a bigger one (hand edit in
 *     sub_000AA880, marker BLOOD-STAIN): game_decal_base() / game_decal_cap().
 * Every pointer read from the game is checked first; a ray result that does not look right is dropped (no stain).
 * Env: RECOMP_BLOOD_STAINS 0..3 (Off / Low = deaths only / Normal / High), RECOMP_STAIN_SCALE (size multiplier, default 1),
 * RECOMP_STAIN_LOG=1 (log every ray and spawn, default: the first 40). */

#define ST_KEY       0x9DDC3F55u
#define ST_CAP_DEF   1500u                                /* node pool size (RECOMP_STAIN_NODES / ini stain_nodes: 100..8000) */
#define ST_CAP_OLD   100u

#include "blood_atlas_tab.h"   /* StEnt + the UV rectangles of mods\blood\atlas.png (tools_dbg\make_blood_atlas.py) */

extern uint32_t *stain_atlas_load(uint32_t *w, uint32_t *h);
extern void texmod_downsample(const uint32_t *src, uint32_t sw, uint32_t sh, uint32_t *dst);
extern volatile const uint32_t *volatile g_stain_atlas;
extern volatile uint32_t g_stain_atlas_w, g_stain_atlas_h, g_stain_atlas_total;

extern void game_blood_stain_rgb(int rgb[3]);
static int st_atlas_ready(const int rgb[3]);

static int s_st_atlas_on;                                 /* the hi-res atlas is loaded and published to the GPU side */

static int s_st_level = -1;
static float s_st_scale = 1.0f;

static void st_init(void)
{
    const char *e = getenv("RECOMP_BLOOD_STAINS"), *s = getenv("RECOMP_STAIN_SCALE");
    s_st_level = e && *e ? atoi(e) : 2;
    if (s_st_level < 0) s_st_level = 0;
    if (s_st_level > 3) s_st_level = 3;
    if (s && atof(s) > 0.05) s_st_scale = (float)atof(s);
}

int game_stain_get(void)
{
    if (s_st_level < 0) st_init();
    return s_st_level;
}

void game_stain_set(int l)
{
    if (s_st_level < 0) st_init();
    s_st_level = l < 0 ? 0 : l > 3 ? 3 : l;
    fprintf(stderr, "[STAIN] blood stains level %d\n", s_st_level);
}

static int st_ok(uint32_t a)
{
    return (a >= 0x10000u && a < 0x00800000u) || (a >= 0x80010000u && a < 0x83FF0000u);
}

static int st_log_on(void)
{
    static int on = -1, n;
    if (on < 0) {
        const char *e = getenv("RECOMP_STAIN_LOG");
        on = e && *e == '1';
    }
    return on || n++ < 40;
}

static uint32_t s_st_rng = 0x1234567u;
static float st_rand(void)
{
    s_st_rng = s_st_rng * 1664525u + 1013904223u;
    return (float)((s_st_rng >> 8) & 0xFFFFFFu) / 16777216.0f;
}

/* ---- node pool: the game keeps 100 decal nodes; stains need more room ---- */
static uint32_t s_dec_base = 0x3C3FE8u, s_dec_cap = ST_CAP_OLD;
static int s_dec_tried;

static uint32_t s_st_nodes_set;                           /* 0 = not set from the menu: env / default */

static uint32_t st_cap_new(void)
{
    const char *e = getenv("RECOMP_STAIN_NODES");
    long v = s_st_nodes_set ? (long)s_st_nodes_set : e && *e ? atol(e) : (long)ST_CAP_DEF;
    return (uint32_t)(v < 100 ? 100 : v > 8000 ? 8000 : v);
}

uint32_t game_stain_nodes(void) { return st_cap_new(); }

/* Recomp Settings "Stain limit": the pool is made when a level loads (the nodes live in lists, so it cannot be resized
 * under them), so a new value applies from the next level load */
void game_stain_nodes_set(uint32_t n)
{
    s_st_nodes_set = n < 100 ? 100 : n > 8000 ? 8000 : n;
    fprintf(stderr, "[STAIN] stain limit %u (applies at the next level load)\n", (unsigned)s_st_nodes_set);
}

int game_stain_nodes_pending(void)
{
    return s_dec_tried && s_dec_cap > ST_CAP_OLD && st_cap_new() != s_dec_cap;
}

static void st_pool(void)
{
    uint32_t s_eax = g_eax, s_ecx = g_ecx, s_edx = g_edx, s_ebx = g_ebx, s_esi = g_esi, s_edi = g_edi,
             s_ebp = g_ebp, s_esp = esp, p, cap = st_cap_new();
    if (s_dec_tried)
        return;
    s_dec_tried = 1;
    if (game_stain_get() <= 0)
        return;
    PUSH32(esp, 0x400u);
    PUSH32(esp, cap * 0x44u);
    PUSH32(esp, 0x310620u);
    PUSH32(esp, 0x000AB4E7u);
    RECOMP_ABI_CALL(0x00155010u, sub_00155010);
    p = g_eax;
    g_eax = s_eax; g_ecx = s_ecx; g_edx = s_edx; g_ebx = s_ebx; g_esi = s_esi; g_edi = s_edi;
    g_ebp = s_ebp; esp = s_esp;
    {
        int rgb[3];
        game_blood_stain_rgb(rgb);
        s_st_atlas_on = st_atlas_ready(rgb);                      /* the picture is read now, not in the middle of a firefight */
    }
    if (st_ok(p) && !(p & 3u)) {
        memset((void *)XBOX_PTR(p), 0, cap * 0x44u);
        s_dec_base = p;
        s_dec_cap = cap;
        fprintf(stderr, "[STAIN] decal node pool: %u nodes at %08X (the game's own: %u)\n", (unsigned)s_dec_cap, (unsigned)p,
                (unsigned)ST_CAP_OLD);
    } else {
        fprintf(stderr, "[STAIN] decal node pool: allocation failed (%08X), the game's own %u nodes stay\n", (unsigned)p,
                (unsigned)ST_CAP_OLD);
    }
}

/* a new manager = a new level: its nodes start from nothing, so the pool is made again (the old one is not freed: if the
 * game's heap was reset with the level its memory is not ours any more, and 21 KB is not worth the risk) */
static uint32_t s_dec_mgr;

static void st_pool_level(void)
{
    uint32_t m = MEM32(0x3C5A78);
    if (m != s_dec_mgr) {
        s_dec_mgr = m;
        s_dec_tried = 0;
        s_dec_base = 0x3C3FE8u;
        s_dec_cap = ST_CAP_OLD;
    }
    st_pool();
}

uint32_t game_decal_cap(void)  { st_pool_level(); return s_dec_cap; }
uint32_t game_decal_base(void) { st_pool_level(); return s_dec_base; }

/* ---- the blood splat texture (32x32, 8 bit palette, Xbox swizzled) ---- */
static uint8_t s_st_pix[1024];
static uint32_t s_st_pal[256];
static int s_st_rgb[3] = { -1, -1, -1 };
static uint32_t s_st_n;                                   /* the size (16 or 32) the arrays above were made for */

static uint32_t st_morton(uint32_t x, uint32_t y)
{
    uint32_t z = 0, b;
    for (b = 0; b < 5; b++)
        z |= ((x >> b) & 1u) << (2 * b) | ((y >> b) & 1u) << (2 * b + 1);
    return z;
}

static float st_smooth(float e0, float e1, float x)
{
    float t = (x - e0) / (e1 - e0);
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    return t * t * (3.0f - 2.0f * t);
}

/* smooth value noise on an integer lattice, 0..1 */
static float st_hash2(int x, int y)
{
    uint32_t h = (uint32_t)(x * 374761393 + y * 668265263);
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xFFFFu) / 65535.0f;
}

static float st_noise(float x, float y)
{
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - (float)ix, fy = y - (float)iy;
    float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
    float a = st_hash2(ix, iy), b = st_hash2(ix + 1, iy), c = st_hash2(ix, iy + 1), d = st_hash2(ix + 1, iy + 1);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}

static int st_idx(float v, uint32_t zm)                  /* thickness 0..1 -> palette index (0 = clear, else zm..255) */
{
    int i;
    if (v < 0.002f)
        return 0;
    i = (int)((float)zm + v * (255.0f - (float)zm) + 0.5f);
    return i < (int)zm ? (int)zm : i > 255 ? 255 : i;
}

static void st_make_tex(const int rgb[3], uint32_t n)
{
    /* A splash: an uneven pool with a ragged edge and a few thin tongues, small round and stretched drops round it.
     * The pixel value is how thick the blood is. The game blends decals as src + dst * (1 - alpha) (premultiplied: the
     * original bullet holes have black where they are clear), so the palette colour is multiplied by its alpha.
     *
     * THE SQUARE (cause found 2026-11-20 with the [STAINTEX] log): the texture has one level, but its palette does NOT
     * start right after the pixels: the GPU's palette address is pixels + n*n + 0x40 (a 64 byte gap). Writing the palette
     * at +0 shifted every colour by 16 entries, and the "clear" index 0 showed entry 16 (a faint colour) = the square.
     * (A mip chain stored in the palette bytes was tried first: wrong guess, levels = 1. The code is kept, zm = 0.) */
    static const float dx[9] = { 0.62f, -0.70f, 0.20f, -0.30f, 0.80f, -0.58f, 0.05f, 0.45f, -0.82f };
    static const float dy[9] = { 0.38f, 0.28f, -0.76f, -0.68f, -0.22f, 0.70f, 0.84f, -0.58f, -0.12f };
    static const float dr[9] = { 0.075f, 0.05f, 0.09f, 0.045f, 0.04f, 0.065f, 0.04f, 0.055f, 0.035f };
    static const float tg[5] = { 0.4f, 1.9f, 3.0f, 4.3f, 5.5f };       /* tongue angles */
    static const float tl[5] = { 0.22f, 0.30f, 0.16f, 0.26f, 0.20f };  /* how far they reach beyond the pool */
    static const float tw[5] = { 0.16f, 0.12f, 0.20f, 0.14f, 0.18f };  /* their width (radians) */
    float lvl[1024], nxt[256];
    uint32_t chain = 0, m, zm, off = 0, lw;
    uint8_t *pb = (uint8_t *)s_st_pal;
    int x, y, k;
    (void)chain; (void)m;
    zm = 0;                                                   /* the texture has ONE level (measured, [STAINTEX]): no mip bytes */
    memset(s_st_pix, 0, sizeof s_st_pix);
    for (y = 0; y < (int)n; y++)
        for (x = 0; x < (int)n; x++) {
            float u = ((float)x + 0.5f) / (0.5f * (float)n) - 1.0f, v = ((float)y + 0.5f) / (0.5f * (float)n) - 1.0f;
            float r = sqrtf(u * u + v * v), th = atan2f(v, u);
            float r0 = 0.34f + 0.16f * (st_noise(2.4f * cosf(th) + 7.0f, 2.4f * sinf(th) + 3.0f) - 0.5f) * 2.0f
                     + 0.05f * (st_noise(7.0f * cosf(th) + 1.0f, 7.0f * sinf(th) + 9.0f) - 0.5f) * 2.0f;
            float a, edge;
            for (k = 0; k < 5; k++) {
                float da = fabsf(th - tg[k]);
                if (da > 3.14159f) da = 6.28318f - da;
                r0 += tl[k] * expf(-(da * da) / (tw[k] * tw[k]));
            }
            a = st_smooth(r0 + 0.045f, r0 - 0.06f, r);
            for (k = 0; k < 9; k++) {
                float ddx = u - dx[k], ddy = v - dy[k], d = sqrtf(ddx * ddx + ddy * ddy), rad = atan2f(dy[k], dx[k]);
                float along = ddx * cosf(rad) + ddy * sinf(rad), across = -ddx * sinf(rad) + ddy * cosf(rad);
                float e = sqrtf((along / 1.6f) * (along / 1.6f) + across * across);   /* stretched away from the centre */
                float b = st_smooth(dr[k] + 0.03f, dr[k] - 0.025f, k & 1 ? e : d);
                if (b > a) a = b;
            }
            a *= 0.86f + 0.14f * st_noise(5.0f * u + 11.0f, 5.0f * v + 5.0f);       /* thicker and thinner patches */
            edge = st_smooth(1.0f, 0.82f, fmaxf(fabsf(u), fabsf(v)));            /* nothing reaches the texture's border */
            a *= edge;
            lvl[y * (int)n + x] = a;
            s_st_pix[st_morton((uint32_t)x, (uint32_t)y)] = (uint8_t)st_idx(a, zm);
        }
    for (k = 0; k < 256; k++) {
        float t = k >= (int)zm ? (float)(k - (int)zm) / (255.0f - (float)zm) : 0.0f, al = powf(t, 0.75f) * 0.96f;
        float core = t * t;                                              /* thick blood is dark, the thin edge is bright red */
        int c[3], j;
        for (j = 0; j < 3; j++) {
            float rim = (float)rgb[j] * (j == 0 ? 1.15f : 1.35f), dk = (float)rgb[j] * 0.45f;
            float col = rim + (dk - rim) * core;                         /* straight alpha: the decals blend SRC_ALPHA / ONE_MINUS_SRC_ALPHA */
            c[j] = (int)(col + 0.5f);
            if (c[j] > 255) c[j] = 255;
        }
        s_st_pal[k] = ((uint32_t)(al * 255.0f + 0.5f) << 24) | ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | (uint32_t)c[2];
    }
    for (lw = n; lw > 1 && zm; lw /= 2) {                     /* (mip chain, unused: zm = 0) */
        uint32_t hw = lw / 2;
        for (y = 0; y < (int)hw; y++)
            for (x = 0; x < (int)hw; x++) {
                float s = lvl[(2 * y) * (int)lw + 2 * x] + lvl[(2 * y) * (int)lw + 2 * x + 1]
                        + lvl[(2 * y + 1) * (int)lw + 2 * x] + lvl[(2 * y + 1) * (int)lw + 2 * x + 1];
                nxt[y * (int)hw + x] = s * 0.25f;
                pb[off + st_morton((uint32_t)x, (uint32_t)y)] = (uint8_t)st_idx(s * 0.25f, zm);
            }
        memcpy(lvl, nxt, (size_t)hw * hw * sizeof(float));
        off += hw * hw;
    }
    s_st_n = n;
}

/* ---- the hi-res atlas: the user's blood pictures (mods\blood\atlas.png, 2048x1024), tinted with the blood colour and given
 * a full mip chain; the GPU side (nv2a_pb_exec.c texdec_stain_hires) draws the decal texture from it ---- */
static uint32_t *s_at_src;
static uint32_t s_at_w, s_at_h;
static int s_at_state, s_at_rgb[3] = { -1, -1, -1 };      /* state: 0 not tried, 1 loaded, 2 built for s_at_rgb, -1 no atlas */

static int st_atlas_ready(const int rgb[3])
{
    uint32_t total = 0, w, h, n = 0, lo[12], lw[12], lh[12], i, k, *buf;
    float c[3], mx = (float)(rgb[0] > rgb[1] ? (rgb[0] > rgb[2] ? rgb[0] : rgb[2]) : (rgb[1] > rgb[2] ? rgb[1] : rgb[2])), s, lut[256];
    if (s_at_state == 0) {
        s_at_src = stain_atlas_load(&s_at_w, &s_at_h);
        if (s_at_src && s_at_w == 2048 && s_at_h == 1024) {
            s_at_state = 1;
        } else {
            if (s_at_src)
                fprintf(stderr, "[STAIN] atlas has the wrong size (%ux%u, it must be 2048x1024)\n", (unsigned)s_at_w, (unsigned)s_at_h);
            free(s_at_src);
            s_at_src = NULL;
            s_at_state = -1;
        }
    }
    if (s_at_state < 0)
        return 0;
    if (s_at_state == 2 && rgb[0] == s_at_rgb[0] && rgb[1] == s_at_rgb[1] && rgb[2] == s_at_rgb[2])
        return 1;
    w = s_at_w;
    h = s_at_h;
    for (;;) {                                                    /* the layout the GPU side expects (texdec_relayout) */
        lw[n] = w; lh[n] = h; lo[n] = total;
        total += w * h;
        n++;
        if ((w <= 1 && h <= 1) || n >= 12)
            break;
        w = w > 1 ? w >> 1 : 1;
        h = h > 1 ? h >> 1 : 1;
    }
    buf = (uint32_t *)malloc((size_t)total * 4);
    if (!buf)
        return s_at_state == 2;
    if (mx < 8.0f) { c[0] = 1.0f; c[1] = 0.0f; c[2] = 0.0f; s = 1.0f; }
    else { c[0] = rgb[0] / mx; c[1] = rgb[1] / mx; c[2] = rgb[2] / mx; s = mx / 77.0f; }
    if (s < 0.45f) s = 0.45f;
    if (s > 2.2f) s = 2.2f;
    for (k = 0; k < 256; k++)
        lut[k] = powf((float)k / 255.0f, 1.1f) * 0.82f * s;     /* the pictures' red channel is how dark the blood is */
    for (i = 0; i < s_at_w * s_at_h; i++) {
        uint32_t p = s_at_src[i], a = p >> 24, r = (p >> 16) & 255u, g = (p >> 8) & 255u, b = p & 255u, o = a << 24;
        float v, spec;
        int j;
        if (!a) { buf[i] = 0; continue; }
        v = lut[r];
        spec = (float)(g < b ? g : b) / 255.0f;                   /* the white shine of wet drops */
        for (j = 0; j < 3; j++) {
            float x = c[j] * v * (1.0f - spec) + spec * (v + 0.15f > 1.0f ? 1.0f : v + 0.15f) * 0.85f;
            int q = (int)(x * 255.0f + 0.5f);
            o |= (uint32_t)(q > 255 ? 255 : q) << (16 - 8 * j);
        }
        buf[i] = o;
    }
    for (k = 1; k < n; k++)
        texmod_downsample(buf + lo[k - 1], lw[k - 1], lh[k - 1], buf + lo[k]);
    g_stain_atlas_w = s_at_w;
    g_stain_atlas_h = s_at_h;
    g_stain_atlas_total = total;
    MemoryBarrier();
    g_stain_atlas = buf;                                          /* the old buffer is left (the GPU side may be reading it) */
    s_at_rgb[0] = rgb[0]; s_at_rgb[1] = rgb[1]; s_at_rgb[2] = rgb[2];
    s_at_state = 2;
    fprintf(stderr, "[STAIN] atlas ready (blood colour %d %d %d)\n", rgb[0], rgb[1], rgb[2]);
    return 1;
}

extern void game_blood_stain_rgb(int rgb[3]);

extern volatile uint32_t g_stain_tex_pix, g_stain_tex_pal;    /* nv2a_pb_exec.c: logs what the GPU decodes of this texture */

/* a record whose texture is a palettised 16x16 or 32x32 picture (SHPX object, header 7B 40 .. at tex+tex[+0x14]): its size, else 0 */
static uint32_t st_rec_size(uint32_t rec, uint32_t *hdr_out)
{
    uint32_t tex = MEM32(rec + 4), hdr, w, h;
    if (!st_ok(tex) || MEM32(tex) != 0x58504853u /* "SHPX" */ || MEM32(tex + 0x14) < 0x20u || MEM32(tex + 0x14) > 0x200u)
        return 0;
    hdr = tex + MEM32(tex + 0x14);
    if (!st_ok(hdr) || MEM8(hdr) != 0x7Bu)
        return 0;
    w = MEM16(hdr + 4);
    h = MEM16(hdr + 6);
    if (w != h || (w != 16 && w != 32))
        return 0;
    if (!(MEMF(rec + 0x1C) < 1.5f && MEMF(rec + 0x20) < 1.5f))
        return 0;                                                 /* an atlas of several pictures: the splat would be cut */
    *hdr_out = hdr;
    return w;
}

/* the record of the blood type: its key is remembered per decal manager (the game builds a new manager with its own set
 * of records for every level, and the set differs: the key 9DDC3F55 of the first level is not in all of them) */
static uint32_t s_st_selmgr, s_st_selbeg, s_st_selkey;

static uint32_t st_pick(uint32_t mgr, uint32_t beg, uint32_t end)
{
    uint32_t rec, best = 0;
    int best_score = -1, idx = 0;
    for (rec = beg; rec < end; rec += 0x34u, idx++) {
        uint32_t hdr = 0, n = st_rec_size(rec, &hdr), tex = MEM32(rec + 4);
        int score = -1;
        if (n) {
            score = 1 + (MEM32(rec) == ST_KEY ? 100 : 0) + (MEM32(rec + 0x2C) == 0 ? 10 : 0) + (n == 32 ? 5 : 0);
            if (st_ok(tex) && MEM32(tex + 0x10) == 0x63697262u /* "bric" */)
                score += 50;
        }
        fprintf(stderr, "[STAIN] manager %08X record %d key %08X tex %08X name %.4s hdr %08X %s size %u live %u -> score %d\n",
                (unsigned)mgr, idx, (unsigned)MEM32(rec), (unsigned)tex, st_ok(tex) ? (const char *)XBOX_PTR(tex + 0x10) : "----",
                (unsigned)hdr, n ? "P8" : "not usable", (unsigned)n, (unsigned)MEM32(rec + 0x2C), score);
        if (score > best_score) { best_score = score; best = rec; }
    }
    if (best)
        fprintf(stderr, "[STAIN] manager %08X: record key %08X taken for blood\n", (unsigned)mgr, (unsigned)MEM32(best));
    else
        fprintf(stderr, "[STAIN] manager %08X: no usable record (needs a 16x16 or 32x32 palettised texture): no stains here\n", (unsigned)mgr);
    s_st_selmgr = mgr;
    s_st_selbeg = beg;
    s_st_selkey = best ? MEM32(best) : 0;
    return best;
}

/* finds the record of the blood type and puts the splat into its texture; returns the record or 0 */
static uint32_t st_record(uint32_t mgr)
{
    uint32_t beg = MEM32(mgr + 0x124), end = MEM32(mgr + 0x128), rec = 0, hdr = 0, n, pix, pal;
    int rgb[3];
    if (!st_ok(beg) || !st_ok(end) || end <= beg || end - beg > 0x34u * 64u)
        return 0;
    if (mgr == s_st_selmgr && beg == s_st_selbeg) {
        if (!s_st_selkey)
            return 0;                                             /* this level has no usable record (logged once) */
        for (rec = beg; rec < end; rec += 0x34u)
            if (MEM32(rec) == s_st_selkey)
                break;
        if (rec >= end)
            rec = 0;
    }
    if (!rec)
        rec = st_pick(mgr, beg, end);
    if (!rec)
        return 0;
    n = st_rec_size(rec, &hdr);
    if (!n)
        return 0;
    pix = hdr + 0x10;
    if (g_stain_tex_pix != pix) {
        g_stain_tex_pix = pix;
        g_stain_tex_pal = 0;                                      /* the GPU tells us its palette address at its first decode */
    }
    pal = pix + n * n + 0x40u;                                    /* measured on 32x32: 64 bytes after the pixels */
    if (g_stain_tex_pal >= pix + n * n && g_stain_tex_pal <= pix + n * n + 0x200u && !(g_stain_tex_pal & 3u))
        pal = g_stain_tex_pal;
    game_blood_stain_rgb(rgb);
    s_st_atlas_on = st_atlas_ready(rgb);                          /* before the 32x32 bytes change: that change decodes the slot again */
    if (rgb[0] != s_st_rgb[0] || rgb[1] != s_st_rgb[1] || rgb[2] != s_st_rgb[2] || s_st_n != n) {
        s_st_rgb[0] = rgb[0]; s_st_rgb[1] = rgb[1]; s_st_rgb[2] = rgb[2];
        st_make_tex(rgb, n);
    }
    if (memcmp((const void *)XBOX_PTR(pix), s_st_pix, n * n) || memcmp((const void *)XBOX_PTR(pal), s_st_pal, 1024)) {
        memcpy((void *)XBOX_PTR(pix), s_st_pix, n * n);
        memcpy((void *)XBOX_PTR(pal), s_st_pal, 1024);
        if (st_log_on())
            fprintf(stderr, "[STAIN] blood splat (%ux%u) written into the texture at %08X, palette %08X (colour %d %d %d)\n", (unsigned)n,
                    (unsigned)n, (unsigned)pix, (unsigned)pal, rgb[0], rgb[1], rgb[2]);
    }
    return rec;
}

/* ---- calls into the game (guest stack scratch; every register the hook's caller may hold is put back) ---- */
typedef struct { uint32_t eax, ecx, edx, ebx, esi, edi, ebp, esp; RecompXmm x[8]; double fs[8]; int ft; } StSave;

static void st_save(StSave *s)
{
    s->eax = g_eax; s->ecx = g_ecx; s->edx = g_edx; s->ebx = g_ebx; s->esi = g_esi; s->edi = g_edi; s->ebp = g_ebp; s->esp = esp;
    s->x[0] = g_xmm0; s->x[1] = g_xmm1; s->x[2] = g_xmm2; s->x[3] = g_xmm3; s->x[4] = g_xmm4; s->x[5] = g_xmm5; s->x[6] = g_xmm6; s->x[7] = g_xmm7;
    memcpy(s->fs, g_fp_stack, sizeof s->fs);
    s->ft = g_fp_top;
}

static void st_load(const StSave *s)
{
    g_eax = s->eax; g_ecx = s->ecx; g_edx = s->edx; g_ebx = s->ebx; g_esi = s->esi; g_edi = s->edi; g_ebp = s->ebp; esp = s->esp;
    g_xmm0 = s->x[0]; g_xmm1 = s->x[1]; g_xmm2 = s->x[2]; g_xmm3 = s->x[3]; g_xmm4 = s->x[4]; g_xmm5 = s->x[5]; g_xmm6 = s->x[6]; g_xmm7 = s->x[7];
    memcpy(g_fp_stack, s->fs, sizeof s->fs);
    g_fp_top = s->ft;
}

static float st_len(float x, float y, float z) { return sqrtf(x * x + y * y + z * z); }

/* the world ray from a to b. 1 = hit: point p, unit normal n (n_ok = 0 when the game's result did not hold a sane normal) */
static int st_ray(const float a[3], const float b[3], float p[3], float n[3], int *n_ok)
{
    StSave sv;
    uint32_t world = MEM32(0x3DEA20), base, Q, R, obj, i;
    float frac, w = MEMF(0x3210B0), cand[2][3], best = 9.0f;
    int ci, pick = -1;
    *n_ok = 0;
    if (!st_ok(world))
        return 0;
    st_save(&sv);
    base = (sv.esp - 0x300u) & ~0xFu;
    Q = base;
    R = base + 0x30u;
    for (i = 0; i < 0xC0u; i += 4)
        MEM32(base + i) = 0;
    MEMF(Q + 0x00) = a[0]; MEMF(Q + 0x04) = a[1]; MEMF(Q + 0x08) = a[2]; MEMF(Q + 0x0C) = w;
    MEMF(Q + 0x10) = b[0]; MEMF(Q + 0x14) = b[1]; MEMF(Q + 0x18) = b[2]; MEMF(Q + 0x1C) = w;
    MEM8(Q + 0x20) = 1;
    MEM32(Q + 0x24) = 0x4006u;
    MEMF(R + 0x14) = 1.0f;
    MEM32(R + 0x20) = 0;
    esp = base - 0x40u;
    g_ecx = world;
    PUSH32(esp, R);
    PUSH32(esp, Q);
    PUSH32(esp, 0x000FAA1Bu);
    RECOMP_ABI_CALL(0x002236B0u, sub_002236B0);
    obj = MEM32(R + 0x20);
    frac = MEMF(R + 0x14);
    for (ci = 0; ci < 2; ci++) {
        cand[ci][0] = MEMF(R + (ci ? 0x00u : 0x04u));
        cand[ci][1] = MEMF(R + (ci ? 0x04u : 0x08u));
        cand[ci][2] = MEMF(R + (ci ? 0x08u : 0x0Cu));
        if (cand[ci][0] == cand[ci][0] && cand[ci][1] == cand[ci][1] && cand[ci][2] == cand[ci][2]) {
            float l = st_len(cand[ci][0], cand[ci][1], cand[ci][2]), e = fabsf(l - 1.0f);
            if (e < 0.12f && e < best) { best = e; pick = ci; }
        }
    }
    st_load(&sv);
    if (st_log_on())
        fprintf(stderr, "[STAIN] ray (%.2f %.2f %.2f)->(%.2f %.2f %.2f): obj %08X frac %.3f normal@4 (%.2f %.2f %.2f) normal@0 (%.2f %.2f %.2f)\n",
                (double)a[0], (double)a[1], (double)a[2], (double)b[0], (double)b[1], (double)b[2], (unsigned)obj, (double)frac,
                (double)cand[0][0], (double)cand[0][1], (double)cand[0][2], (double)cand[1][0], (double)cand[1][1], (double)cand[1][2]);
    if (!obj || !(frac > 0.0f) || !(frac < 1.0f))
        return 0;
    for (i = 0; i < 3; i++)
        p[i] = a[i] + (b[i] - a[i]) * frac;
    if (pick >= 0) {
        n[0] = cand[pick][0]; n[1] = cand[pick][1]; n[2] = cand[pick][2];
        *n_ok = 1;
    }
    return 1;
}

/* one stain: position, unit normal (pointing out of the surface), size */
static int st_spawn(const float p[3], const float n[3], float size, const StEnt *ent, int flip, int norot)
{
    StSave sv;
    uint32_t mgr = MEM32(0x3C5A78), rec, node, base, B, out, i;
    float o8, oC, o10, o14, o18;
    if (!st_ok(mgr))
        return 0;
    rec = st_record(mgr);
    if (!rec)
        return 0;
    if (!s_st_atlas_on)
        ent = NULL;                                               /* the small drawn splat: the whole texture */
    o8 = MEMF(rec + 8); oC = MEMF(rec + 0xC); o10 = MEMF(rec + 0x10); o14 = MEMF(rec + 0x14); o18 = MEMF(rec + 0x18);
    MEMF(rec + 8) = size; MEMF(rec + 0xC) = size; MEMF(rec + 0x10) = 1.0f; MEMF(rec + 0x14) = 1.0f;
    if (norot)
        MEMF(rec + 0x18) = 0.0f;                                  /* a wall splat keeps its drips pointing down */
    st_save(&sv);
    base = (sv.esp - 0x200u) & ~0xFu;
    B = base;
    out = base + 0x40u;
    for (i = 0; i < 0x50u; i += 4)
        MEM32(base + i) = 0;
    MEMF(B + 0x00) = n[0]; MEMF(B + 0x04) = n[1]; MEMF(B + 0x08) = n[2];
    MEMF(B + 0x10) = p[0] + n[0] * 0.004f; MEMF(B + 0x14) = p[1] + n[1] * 0.004f; MEMF(B + 0x18) = p[2] + n[2] * 0.004f;
    MEMF(B + 0x1C) = 1.0f;
    esp = base - 0x40u;
    g_ecx = mgr;
    PUSH32(esp, out);
    PUSH32(esp, MEM32(rec));
    PUSH32(esp, 0x000AB5C5u);
    RECOMP_ABI_CALL(0x000AA880u, sub_000AA880);
    node = g_eax;
    if (st_ok(node) || (node >= 0x3C3FE8u && node < 0x3C5A78u)) {
        PUSH32(esp, 0);
        PUSH32(esp, B);
        PUSH32(esp, rec);
        PUSH32(esp, node);
        PUSH32(esp, 0x000AB5DBu);
        RECOMP_ABI_CALL(0x000AAD50u, sub_000AAD50);
        if (ent) {                                                /* the node's picture: a rectangle of the atlas (UV at +0x30..+0x3C) */
            float ua = flip ? ent->u1 : ent->u0, ub = flip ? ent->u0 : ent->u1;
            MEMF(node + 0x30) = ua; MEMF(node + 0x34) = ent->v0;
            MEMF(node + 0x38) = ub; MEMF(node + 0x3C) = ent->v1;
        }
    } else {
        node = 0;
    }
    st_load(&sv);
    MEMF(rec + 8) = o8; MEMF(rec + 0xC) = oC; MEMF(rec + 0x10) = o10; MEMF(rec + 0x14) = o14; MEMF(rec + 0x18) = o18;
    if (st_log_on())
        fprintf(stderr, "[STAIN] spawn at %.2f %.2f %.2f normal %.2f %.2f %.2f size %.2f -> %s (live %u of %u)\n", (double)p[0], (double)p[1],
                (double)p[2], (double)n[0], (double)n[1], (double)n[2], (double)size, node ? "ok" : "no node", (unsigned)MEM32(mgr + 0x120),
                (unsigned)s_dec_cap);
    return node != 0;
}

/* what kind of stain: a hit (a spatter), a death (a pool; satellites round it are hits) */
enum { ST_HIT = 0, ST_POOL = 1 };

/* one stain on a surface with unit normal n. mult scales everything (level, the scale setting). With the hi-res atlas the
 * picture and size follow the surface: a wall takes one of the three big splats (never rotated: the drips hang down), a
 * floor a clean crop of droplets and spatter, a death on a floor the pool. Without the atlas the small drawn splat. */
static void st_put(const float p[3], const float n[3], float mult, int kind)
{
    int wall = fabsf(n[2]) < 0.6f;
    float r = st_rand();
    uint32_t mgr = MEM32(0x3C5A78);
    if (st_ok(mgr))
        (void)st_record(mgr);                                     /* settles s_st_atlas_on (and writes the texture) before the picture is chosen */
    if (!s_st_atlas_on) {
        st_spawn(p, n, (wall ? 0.16f + 0.14f * r : kind == ST_POOL ? 0.30f + 0.14f * r : 0.07f + 0.07f * r) * mult, NULL, 0, 0);
    } else if (kind == ST_POOL && !wall) {
        st_spawn(p, n, (0.34f + 0.14f * r) * mult, &k_st_pool[0], st_rand() < 0.5f, 0);
    } else if (wall) {
        st_spawn(p, n, (0.26f + 0.18f * r) * mult, &k_st_wall[(int)(st_rand() * 2.999f)], st_rand() < 0.5f, 1);
    } else {
        const StEnt *e = &k_st_crop[(int)(st_rand() * 11.999f)];
        st_spawn(p, n, 0.5f * e->scale * (0.8f + 0.7f * r) * mult, e, st_rand() < 0.5f, 0);
    }
}

/* the floor under (x, y, z): straight down; a floor facing up is a good floor */
static void st_floor(float x, float y, float z, float mult, int kind)
{
    float a[3], b[3], p[3], n[3];
    int n_ok;
    a[0] = x; a[1] = y; a[2] = z + 0.05f;
    b[0] = x; b[1] = y; b[2] = z - 3.0f;
    if (!st_ray(a, b, p, n, &n_ok))
        return;
    if (a[2] - p[2] < 0.15f)
        return;                                                   /* the ray stopped on the soldier or a prop right under it, not the floor */
    if (!n_ok || n[2] < 0.5f) { n[0] = 0.0f; n[1] = 0.0f; n[2] = 1.0f; }
    st_put(p, n, mult, kind);
}

/* a wall behind the hit, along the bullet's direction d (unit) */
static void st_wall(float x, float y, float z, const float d[3], float mult)
{
    float a[3], b[3], p[3], n[3], dot;
    int n_ok;
    a[0] = x + d[0] * 0.2f; a[1] = y + d[1] * 0.2f; a[2] = z + d[2] * 0.2f;
    b[0] = x + d[0] * 4.5f; b[1] = y + d[1] * 4.5f; b[2] = z + d[2] * 4.5f;
    if (!st_ray(a, b, p, n, &n_ok) || !n_ok)
        return;                                                   /* no sane normal: no wall stain (the floor one does not need it) */
    dot = n[0] * d[0] + n[1] * d[1] + n[2] * d[2];
    if (dot > 0.0f) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; } /* facing the shooter */
    if (fabsf(dot) < 0.2f)
        return;                                                   /* grazing: it would be a streak */
    st_put(p, n, mult, ST_HIT);
}

/* ---- triggers ---- */
static float s_st_dir[3];
static DWORD s_st_dir_t;

/* sub_000FB7C0 after the hit test succeeded (hook loc_000FB861): obj = the bullet / contact object whose segment ran from
 * +0x1C0 to +0x60 (the vector sub_000FA8C0 tests) */
void game_stain_prep(uint32_t obj)
{
    float dx, dy, dz, l;
    if (game_stain_get() <= 0 || !st_ok(obj))
        return;
    dx = MEMF(obj + 0x60) - MEMF(obj + 0x1C0);
    dy = MEMF(obj + 0x64) - MEMF(obj + 0x1C4);
    dz = MEMF(obj + 0x68) - MEMF(obj + 0x1C8);
    l = st_len(dx, dy, dz);
    if (!(l > 0.0001f) || !(l < 1000.0f))
        return;
    s_st_dir[0] = dx / l; s_st_dir[1] = dy / l; s_st_dir[2] = dz / l;
    s_st_dir_t = GetTickCount();
}

static struct { uint32_t sol; float p[3]; DWORD t; } s_st_hit[16];

static int st_pos_ok(float x, float y, float z)
{
    return x == x && y == y && z == z && fabsf(x) < 100000.0f && fabsf(y) < 100000.0f && fabsf(z) < 100000.0f;
}

/* sub_000FB7C0's blood block (hook loc_000FBC77): a soldier was hit at (hx, hy, hz) */
void game_stain_hit(uint32_t sol, float hx, float hy, float hz)
{
    int lv = game_stain_get(), i, slot = 0;
    float d[3], mult;
    DWORD now = GetTickCount();
    static DWORD last_ms;
    if (lv <= 0 || !st_pos_ok(hx, hy, hz))
        return;
    for (i = 0; i < 16; i++)
        if (s_st_hit[i].sol == sol) { slot = i; break; }
        else if (s_st_hit[i].t < s_st_hit[slot].t) slot = i;
    s_st_hit[slot].sol = sol;
    s_st_hit[slot].p[0] = hx; s_st_hit[slot].p[1] = hy; s_st_hit[slot].p[2] = hz;
    s_st_hit[slot].t = now;
    if (lv < 2 || (int)(now - last_ms) < 60)
        return;                                                   /* Low = only deaths; a burst of hits: one stain every 60 ms */
    last_ms = now;
    mult = (lv >= 3 ? 1.4f : 1.0f) * s_st_scale;
    if ((int)(now - s_st_dir_t) < 100) {
        d[0] = s_st_dir[0]; d[1] = s_st_dir[1]; d[2] = s_st_dir[2];
    } else {                                                      /* no bullet segment: along the line from the camera */
        uint32_t cam = MEM32(0x3E8D08);
        float l;
        if (!st_ok(cam))
            return;
        d[0] = hx - MEMF(cam + 0x90); d[1] = hy - MEMF(cam + 0x94); d[2] = hz - MEMF(cam + 0x98);
        l = st_len(d[0], d[1], d[2]);
        if (!(l > 0.01f))
            return;
        d[0] /= l; d[1] /= l; d[2] /= l;
    }
    if (lv >= 3 || st_rand() < 0.6f)
        st_wall(hx, hy, hz, d, mult);
    if (lv >= 3 || st_rand() < 0.5f) {
        st_floor(hx, hy, hz, mult, ST_HIT);
        if (s_st_atlas_on && (lv >= 3 || st_rand() < 0.5f)) {     /* drops fall round the first one */
            float a = st_rand() * 6.2832f, rr = 0.15f + 0.4f * st_rand();
            st_floor(hx + cosf(a) * rr, hy + sinf(a) * rr, hz, mult, ST_HIT);
        }
    }
}

/* a soldier died (game_lod_dead): a pool on the floor where he was last hit */
void game_stain_death(uint32_t sol)
{
    int lv = game_stain_get(), i;
    DWORD now = GetTickCount();
    if (lv <= 0)
        return;
    for (i = 0; i < 16; i++)
        if (s_st_hit[i].sol == sol && (int)(now - s_st_hit[i].t) < 1500) {
            float mult = (lv >= 3 ? 1.3f : 1.0f) * s_st_scale, x = s_st_hit[i].p[0], y = s_st_hit[i].p[1], z = s_st_hit[i].p[2];
            int k;
            st_floor(x, y, z, mult, ST_POOL);
            if (s_st_atlas_on)
                for (k = 0; k < 2 + (lv >= 3); k++) {              /* drops and a small spatter round the pool */
                    float a = st_rand() * 6.2832f, rr = 0.35f + 0.55f * st_rand();
                    st_floor(x + cosf(a) * rr, y + sinf(a) * rr, z, mult, ST_HIT);
                }
            s_st_hit[i].sol = 0;
            return;
        }
}

/* the player lost health (game_pain_poll): blood on the floor under the player and on the screen */
void game_stain_player_hurt(float drop, float maxhp)
{
    int lv = game_stain_get();
    uint32_t cam = MEM32(0x3E8D08);
    float s = 0.35f + 2.0f * drop / (maxhp > 20.0f ? maxhp : 100.0f);
    static DWORD last_ms;
    DWORD now = GetTickCount();
    extern void recomp_screenfx_trigger(int kind, float strength);
    if ((int)(now - last_ms) < 250)
        return;
    last_ms = now;
    recomp_screenfx_trigger(0, s > 1.0f ? 1.0f : s);             /* the "Screen blood" row decides whether it shows */
    if (lv < 2 || !st_ok(cam))
        return;
    st_floor(MEMF(cam + 0x90), MEMF(cam + 0x94), MEMF(cam + 0x98) - 0.2f, (lv >= 3 ? 1.4f : 1.0f) * s_st_scale, ST_HIT);
}
