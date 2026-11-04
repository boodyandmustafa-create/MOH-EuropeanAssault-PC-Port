/* Screen effects on the player's view: blood splatters (a soldier killed or hit very close) and dirt / dust (an explosion
 * nearby). Pictures: mods/screenfx/blood_NN.png and dirt_NN.png (RGBA, straight alpha; tools/screenfx/gen_screenfx.py makes
 * the shipped ones, replace them with your own).
 *
 * How it is drawn: a click-through topmost layered window with per-pixel alpha laid over the game window, owned by its own
 * thread (so the present thread never waits for it). A splatter is scaled once when it appears, then composited at ~30 Hz
 * only while something is on screen: it pops in, holds, fades out and (blood) slides down a little. Nothing runs when idle.
 *
 * recomp_screenfx_trigger(kind, strength) may be called from any thread (cheap: it only queues a request). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FX_BLOOD 0
#define FX_DIRT  1
#define FX_RAIN  2                           /* raindrops: not triggered one by one but sustained (recomp_screenfx_rain) */
#define FX_KINDS 3
#define FX_PICS  16
#define FX_MAX   72                          /* splatters on screen at once (all kinds) */
#define FX_RAIN_CAP 48                       /* ... of which at most this many are raindrops, so rain never starves blood / dirt */
#define FX_QUEUE 8

typedef struct { uint32_t *px; int w, h; } FxPic;       /* straight alpha 0xAARRGGBB */
typedef struct {
    int on, kind;
    int w, h;
    uint32_t *pm;                            /* premultiplied, scaled, already flipped */
    float xf, yf;                            /* centre as a fraction of the game window */
    DWORD born, life;
    float peak, drip;                        /* peak opacity 0..1, slide down as a fraction of the window height */
    float sa, sb;                            /* raindrops: the slide runs at constant speed from life fraction sa to sb */
} FxSplat;
typedef struct { int kind; float strength; } FxReq;

static FxPic s_pics[FX_KINDS][FX_PICS];
static int s_npics[FX_KINDS];
static FxSplat s_sp[FX_MAX];
static FxReq s_q[FX_QUEUE];
static int s_qn;
static CRITICAL_SECTION s_cs;
static HANDLE s_ev, s_thread;
static volatile HWND s_game;
static volatile LONG s_started;
static volatile int s_level[FX_KINDS] = { 2, 2, 0 };      /* 0 off, 1 low, 2 normal, 3 high; rain starts off */
static DWORD s_last_trig[FX_KINDS];
static volatile DWORD s_rain_until;                       /* raining while GetTickCount() is before this (refreshed by the game side) */
static volatile LONG s_rain_x1000;                        /* rain strength 0..1 x 1000 */
static int s_rain_force;                                  /* RECOMP_SCREEN_RAIN_FORCE=1: always raining (to judge the look) */
static int (*volatile s_menu_cb)(void);                   /* returns 1 while a menu / the pause screen is up: nothing is drawn then */
static volatile DWORD s_bar_until;                        /* the HUD bar is drawn while GetTickCount() is before this (refreshed by the game side) */
static volatile DWORD s_tm_until;                         /* the countdown number is drawn while GetTickCount() is before this */
static volatile LONG s_tm_v[3];                           /* countdown left, top (window fractions x1000), digit height as a fraction x1000, and the value in tenths of a second */
static volatile LONG s_tm_val;
static volatile LONG s_bar_v[5];

/* Split-screen (RECOMP_SPLIT_PADS): one overlay over the whole window cannot belong to one player, so nothing is drawn */
static int fx_split_screen(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_SPLIT_PADS");
        on = e && *e && *e != '0';
    }
    return on;
}                          /* bar left, top, right, bottom and fill, window fractions x1000 */

void recomp_screenfx_set_menu_cb(int (*cb)(void))
{
    s_menu_cb = cb;
}

void recomp_screenfx_bar(float x0, float y0, float x1, float y1, float fill)
{
    DWORD now = GetTickCount();
    int was_off = (int)(s_bar_until - now) <= 0;

    s_bar_v[0] = (LONG)(x0 * 1000.0f);
    s_bar_v[1] = (LONG)(y0 * 1000.0f);
    s_bar_v[2] = (LONG)(x1 * 1000.0f);
    s_bar_v[3] = (LONG)(y1 * 1000.0f);
    s_bar_v[4] = (LONG)(fill * 1000.0f + 0.5f);
    s_bar_until = now + 200;
    /* an idle overlay thread sleeps with no timeout, so wake it when the bar comes back. Only on that change: this runs every
     * game frame, and a wake per call would composite at game fps. */
    if (was_off && s_ev)
        SetEvent(s_ev);
}

/* The grenade countdown: x, y = top-left as window fractions, h = digit height as a window-height fraction, tenths = time left. */
void recomp_screenfx_timer(float x, float y, float h, int tenths)
{
    DWORD now = GetTickCount();
    int was_off = (int)(s_tm_until - now) <= 0;

    s_tm_v[0] = (LONG)(x * 1000.0f);
    s_tm_v[1] = (LONG)(y * 1000.0f);
    s_tm_v[2] = (LONG)(h * 1000.0f);
    s_tm_val = tenths;
    s_tm_until = now + 200;
    if (was_off && s_ev)
        SetEvent(s_ev);
}

/* ---- picture files through GDI+ (loaded at run time, nothing extra linked) ---- */
typedef struct { UINT w, h; INT stride; INT fmt; void *scan0; UINT_PTR reserved; } fx_bitmapdata;
typedef struct { INT x, y, w, h; } fx_rect;
typedef struct { UINT32 version; void *callback; BOOL no_thread; BOOL no_codecs; } fx_startup;

static uint32_t *fx_load_picture(const char *path, int *out_w, int *out_h)
{
    typedef int (WINAPI *startup_fn)(ULONG_PTR *, const fx_startup *, void *);
    typedef int (WINAPI *fromfile_fn)(const WCHAR *, void **);
    typedef int (WINAPI *dim_fn)(void *, UINT *);
    typedef int (WINAPI *lock_fn)(void *, const fx_rect *, UINT, INT, fx_bitmapdata *);
    typedef int (WINAPI *unlock_fn)(void *, fx_bitmapdata *);
    typedef int (WINAPI *dispose_fn)(void *);
    static ULONG_PTR token;
    HMODULE gp = LoadLibraryA("gdiplus.dll");
    startup_fn startup;
    fromfile_fn fromfile;
    dim_fn get_w, get_h;
    lock_fn lock;
    unlock_fn unlock;
    dispose_fn dispose;
    WCHAR wide[MAX_PATH], full[MAX_PATH];
    fx_startup su;
    void *bmp = NULL;
    UINT w = 0, h = 0, y;
    fx_rect rc;
    fx_bitmapdata bd;
    uint32_t *px = NULL;

    if (!gp)
        return NULL;
    startup  = (startup_fn)GetProcAddress(gp, "GdiplusStartup");
    fromfile = (fromfile_fn)GetProcAddress(gp, "GdipCreateBitmapFromFile");
    get_w    = (dim_fn)GetProcAddress(gp, "GdipGetImageWidth");
    get_h    = (dim_fn)GetProcAddress(gp, "GdipGetImageHeight");
    lock     = (lock_fn)GetProcAddress(gp, "GdipBitmapLockBits");
    unlock   = (unlock_fn)GetProcAddress(gp, "GdipBitmapUnlockBits");
    dispose  = (dispose_fn)GetProcAddress(gp, "GdipDisposeImage");
    if (!startup || !fromfile || !get_w || !get_h || !lock || !unlock || !dispose)
        return NULL;
    if (!token) {
        memset(&su, 0, sizeof su);
        su.version = 1;
        if (startup(&token, &su, NULL) != 0) {
            token = 0;
            return NULL;
        }
    }
    if (!MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH) || !GetFullPathNameW(wide, MAX_PATH, full, NULL))
        return NULL;
    if (fromfile(full, &bmp) != 0 || !bmp)
        return NULL;
    get_w(bmp, &w);
    get_h(bmp, &h);
    if (w > 0 && h > 0 && w <= 4096 && h <= 4096) {
        rc.x = 0; rc.y = 0; rc.w = (INT)w; rc.h = (INT)h;
        memset(&bd, 0, sizeof bd);
        if (lock(bmp, &rc, 1u, 0x0026200A, &bd) == 0) {     /* read, 32bpp ARGB (straight) */
            px = (uint32_t *)malloc((size_t)w * h * sizeof(uint32_t));
            if (px)
                for (y = 0; y < h; y++)
                    memcpy(px + (size_t)y * w, (const BYTE *)bd.scan0 + (ptrdiff_t)y * bd.stride, (size_t)w * sizeof(uint32_t));
            unlock(bmp, &bd);
        }
    }
    dispose(bmp);
    *out_w = (int)w;
    *out_h = (int)h;
    return px;
}

static void fx_load_all(void)
{
    static const char *k_name[FX_KINDS] = { "blood", "dirt", "rain" };
    int k, i;
    char path[MAX_PATH];
    for (k = 0; k < FX_KINDS; k++)
        for (i = 1; i <= FX_PICS; i++) {
            FxPic *p = &s_pics[k][s_npics[k]];
            snprintf(path, sizeof path, "mods\\screenfx\\%s_%02d.png", k_name[k], i);
            p->px = fx_load_picture(path, &p->w, &p->h);
            if (p->px)
                s_npics[k]++;
        }
    fprintf(stderr, "[SCREENFX] pictures: %d blood, %d dirt, %d rain (mods/screenfx)\n", s_npics[0], s_npics[1], s_npics[2]);
}

/* ---- splatters ---- */
static unsigned fx_rng(void)
{
    static unsigned s = 0x9E3779B9u;
    s = s * 1664525u + 1013904223u;
    return s >> 8;
}

static float fx_rand(float lo, float hi)
{
    return lo + (hi - lo) * (float)(fx_rng() & 0xFFFF) / 65535.0f;
}

/* scale a straight-alpha picture to w x h (bilinear), optionally mirrored, into premultiplied 0xAARRGGBB */
static uint32_t *fx_scale(const FxPic *p, int w, int h, int flip)
{
    uint32_t *d = (uint32_t *)malloc((size_t)w * h * sizeof(uint32_t));
    int x, y;
    if (!d)
        return NULL;
    for (y = 0; y < h; y++) {
        float fy = ((float)y + 0.5f) * (float)p->h / (float)h - 0.5f;
        int y0 = (int)floorf(fy), y1;
        float wy = fy - (float)y0;
        if (y0 < 0) { y0 = 0; wy = 0.0f; }
        y1 = y0 + 1 < p->h ? y0 + 1 : p->h - 1;
        if (y0 >= p->h) y0 = y1 = p->h - 1;
        for (x = 0; x < w; x++) {
            float fx = ((float)(flip ? w - 1 - x : x) + 0.5f) * (float)p->w / (float)w - 0.5f;
            int x0 = (int)floorf(fx), x1;
            float wx = fx - (float)x0, a = 0, r = 0, g = 0, b = 0;
            int k;
            if (x0 < 0) { x0 = 0; wx = 0.0f; }
            x1 = x0 + 1 < p->w ? x0 + 1 : p->w - 1;
            if (x0 >= p->w) x0 = x1 = p->w - 1;
            for (k = 0; k < 4; k++) {
                uint32_t c = p->px[(k & 2 ? y1 : y0) * p->w + (k & 1 ? x1 : x0)];
                float cw = (k & 1 ? wx : 1.0f - wx) * (k & 2 ? wy : 1.0f - wy);
                float ca = (float)(c >> 24) * cw;
                a += ca;
                r += (float)((c >> 16) & 255) * ca;
                g += (float)((c >> 8) & 255) * ca;
                b += (float)(c & 255) * ca;
            }
            if (a <= 0.5f) {
                d[y * w + x] = 0;
            } else {
                float inv = 1.0f / a;                /* straight colour = r/a; premultiplied = colour * a / 255 */
                uint32_t pa = (uint32_t)(a + 0.5f);
                d[y * w + x] = (pa << 24)
                    | ((uint32_t)(r * inv * a / 255.0f + 0.5f) << 16)
                    | ((uint32_t)(g * inv * a / 255.0f + 0.5f) << 8)
                    | (uint32_t)(b * inv * a / 255.0f + 0.5f);
            }
        }
    }
    return d;
}

static void fx_spawn(int kind, float strength, int winw, int winh)
{
    int i, slot = -1, n = s_npics[kind];
    float mult = s_level[kind] == 1 ? 0.55f : s_level[kind] == 3 ? 1.6f : 1.0f;
    float size, str = strength < 0.15f ? 0.15f : strength > 1.0f ? 1.0f : strength;
    FxSplat *s;
    DWORD now = GetTickCount();
    if (!n || winw < 64 || winh < 64)
        return;
    if (kind == FX_RAIN) {                  /* raindrops stay under their own cap */
        int cap = s_level[FX_RAIN] == 1 ? 16 : s_level[FX_RAIN] == 3 ? FX_RAIN_CAP : 30, have = 0;
        for (i = 0; i < FX_MAX; i++)
            if (s_sp[i].on && s_sp[i].kind == FX_RAIN)
                have++;
        if (have >= cap)
            return;
    }
    for (i = 0; i < FX_MAX; i++)
        if (!s_sp[i].on) {
            slot = i;
            break;
        }
    if (slot < 0) {                         /* replace the oldest (never a raindrop for blood / dirt unless nothing else is on) */
        DWORD oldest = 0;
        if (kind == FX_RAIN)
            return;
        for (i = 0; i < FX_MAX; i++)
            if (s_sp[i].kind != FX_RAIN && now - s_sp[i].born >= oldest) {
                oldest = now - s_sp[i].born;
                slot = i;
            }
        if (slot < 0)
            slot = 0;
        free(s_sp[slot].pm);
        s_sp[slot].pm = NULL;
        s_sp[slot].on = 0;
    }
    s = &s_sp[slot];
    if (kind == FX_BLOOD)
        size = fx_rand(0.28f, 0.50f) * (0.65f + 0.5f * str) * (0.75f + 0.25f * mult);
    else if (kind == FX_RAIN)
        size = (fx_rng() % 100 < 12) ? fx_rand(0.07f, 0.12f) : fx_rand(0.018f, 0.06f);
    else
        size = fx_rand(0.70f, 1.10f) * (0.7f + 0.4f * str);
    s->w = (int)((float)winh * size);
    s->h = s->w;
    if (kind == FX_RAIN && s->w < 10)
        s->w = s->h = 10;
    if (s->w > winw * 2) s->w = s->h = winw * 2;
    s->pm = fx_scale(&s_pics[kind][fx_rng() % (unsigned)n], s->w, s->h, (int)(fx_rng() & 1));
    if (!s->pm)
        return;
    s->kind = kind;
    if (kind == FX_RAIN) {
        /* A raindrop can land anywhere on the glass, a little more often towards the edges (rejection sampling), and it
         * keeps the middle usable: the density there is a quarter of the edge's. */
        for (i = 0; i < 12; i++) {
            float u, v, e, k;
            s->xf = fx_rand(0.01f, 0.99f);
            s->yf = fx_rand(0.01f, 0.99f);
            u = (s->xf - 0.5f) * 2.0f;
            v = (s->yf - 0.5f) * 2.0f;
            e = sqrtf(u * u + v * v);
            k = e <= 0.30f ? 0.0f : e >= 0.95f ? 1.0f : (e - 0.30f) / 0.65f;
            k = k * k * (3.0f - 2.0f * k);
            if (fx_rand(0.0f, 1.0f) < 0.25f + 0.75f * k)
                break;
        }
        s->born = now;
        s->life = (DWORD)(fx_rand(4500.0f, 9500.0f) * (mult < 1.0f ? 0.8f : 1.0f));
        s->peak = fx_rand(0.55f, 0.95f);
        /* most beads sit still; about a quarter run down the glass a short way (bigger ones further) at constant speed:
         * 2.5-7% of the window height per second, starting after the drop has sat a while */
        s->drip = (fx_rng() % 100 < 25) ? fx_rand(0.04f, 0.20f) * (0.6f + (float)s->w / (float)winh * 6.0f) : 0.0f;
        if (s->drip > 0.35f) s->drip = 0.35f;
        s->sa = fx_rand(0.08f, 0.30f);
        s->sb = s->sa + s->drip / fx_rand(0.025f, 0.07f) * 1000.0f / (float)s->life;
        if (s->sb > 0.95f) {                /* not enough life left for the whole run: shorten it, same speed */
            s->drip *= (0.95f - s->sa) / (s->sb - s->sa);
            s->sb = 0.95f;
        }
        s->on = 1;
        return;
    }
    {   /* whatever the picture is, it never ends in a hard edge: its outer 10% fades to nothing on every side */
        int fx, fy, band = s->w / 10 > 1 ? s->w / 10 : 1;
        for (fy = 0; fy < s->h; fy++)
            for (fx = 0; fx < s->w; fx++) {
                int m = fx < s->w - 1 - fx ? fx : s->w - 1 - fx, m2 = fy < s->h - 1 - fy ? fy : s->h - 1 - fy;
                uint32_t p = s->pm[fy * s->w + fx], a;
                float k;
                if (m2 < m) m = m2;
                if (m >= band || !(p >> 24))
                    continue;
                k = (float)m / (float)band;
                k = k * k * (3.0f - 2.0f * k);
                a = (uint32_t)(k * 256.0f);
                s->pm[fy * s->w + fx] = ((((p >> 24) * a) >> 8) << 24) | (((((p >> 16) & 255) * a) >> 8) << 16)
                                      | (((((p >> 8) & 255) * a) >> 8) << 8) | ((((p & 255) * a) >> 8));
            }
    }
    /* The middle of the view stays clear: the centre of a splatter sits in a band along the left / right edge (or the top /
     * bottom edge), and whatever still reaches inward is faded out by an elliptical mask over the middle of the screen. */
    if (fx_rng() % 10 < 7) {
        s->xf = (fx_rng() & 1) ? fx_rand(0.03f, 0.18f) : fx_rand(0.82f, 0.97f);
        s->yf = fx_rand(0.12f, 0.88f);
    } else {
        s->xf = fx_rand(0.08f, 0.92f);
        s->yf = (fx_rng() & 1) ? fx_rand(0.05f, 0.18f) : fx_rand(0.82f, 0.95f);
    }
    {
        const float e0 = kind == FX_BLOOD ? 0.50f : 0.42f, e1 = kind == FX_BLOOD ? 0.86f : 0.95f;
        int mx, my, ox = (int)(s->xf * (float)winw) - s->w / 2, oy = (int)(s->yf * (float)winh) - s->h / 2;
        for (my = 0; my < s->h; my++) {
            float v = ((float)(oy + my) / (float)winh - 0.5f) * 2.0f;
            for (mx = 0; mx < s->w; mx++) {
                float u = ((float)(ox + mx) / (float)winw - 0.5f) * 2.0f;
                float e = sqrtf(u * u + v * v), k;
                uint32_t p = s->pm[my * s->w + mx], a;
                if (e >= e1 || !(p >> 24))
                    continue;
                k = e <= e0 ? 0.0f : (e - e0) / (e1 - e0);
                k = k * k * (3.0f - 2.0f * k);
                a = (uint32_t)(k * 256.0f);
                s->pm[my * s->w + mx] = ((((p >> 24) * a) >> 8) << 24) | (((((p >> 16) & 255) * a) >> 8) << 16)
                                      | (((((p >> 8) & 255) * a) >> 8) << 8) | ((((p & 255) * a) >> 8));
            }
        }
    }
    s->born = now;
    if (kind == FX_BLOOD) {
        s->life = (DWORD)(4200.0f * (0.8f + 0.25f * mult));
        s->peak = (0.78f + 0.2f * str) * (mult < 1.0f ? 0.8f : 1.0f);
        s->drip = fx_rand(0.05f, 0.10f);
    } else {
        s->life = (DWORD)(4600.0f * (0.85f + 0.2f * mult));
        s->peak = (0.55f + 0.3f * str) * (mult < 1.0f ? 0.8f : 1.0f);
        s->drip = fx_rand(0.015f, 0.04f);
    }
    if (s->peak > 1.0f) s->peak = 1.0f;
    s->on = 1;
}

static float fx_factor(const FxSplat *s, DWORD now)
{
    float t = (float)(now - s->born) / (float)s->life;
    float hold = s->kind == FX_RAIN ? 0.62f : 0.40f;
    if (t >= 1.0f)
        return 0.0f;
    if (t < 0.04f)
        return t / 0.04f;
    if (t < hold)
        return 1.0f;
    t = (t - hold) / (1.0f - hold);
    return powf(1.0f - t, 1.6f);
}

static void fx_over(uint32_t *d, uint32_t s)
{
    uint32_t ia = 255u - (s >> 24), out = 0, sh;
    for (sh = 0; sh < 32; sh += 8) {
        uint32_t v = ((s >> sh) & 255u) + ((((*d >> sh) & 255u) * ia + 127u) / 255u);
        out |= (v > 255u ? 255u : v) << sh;
    }
    *d = out;
}

/* Premultiplied 0xAARRGGBB: a dark frame, an empty see-through channel and the yellow fill. */
static void fx_bar(uint32_t *bits, int W, int H, RECT *dirty)
{
    int l = (int)((float)s_bar_v[0] * (float)W * 0.001f), t = (int)((float)s_bar_v[1] * (float)H * 0.001f);
    int r = (int)((float)s_bar_v[2] * (float)W * 0.001f), b = (int)((float)s_bar_v[3] * (float)H * 0.001f);
    int bd = H >= 900 ? 3 : 2, fill_r, x, y;

    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > W) r = W;
    if (b > H) b = H;
    if (r - l <= 2 * bd || b - t <= 2 * bd)
        return;
    fill_r = l + bd + (int)((float)(r - l - 2 * bd) * (float)s_bar_v[4] * 0.001f + 0.5f);
    for (y = t; y < b; y++)
        for (x = l; x < r; x++) {
            uint32_t c;
            if (x < l + bd || x >= r - bd || y < t + bd || y >= b - bd)
                c = 0xE6242424u;
            else if (x < fill_r)
                c = 0xFFFFC800u;
            else
                c = 0x96000000u;
            fx_over(&bits[(size_t)y * W + x], c);
        }
    if (l < dirty->left) dirty->left = l;
    if (t < dirty->top) dirty->top = t;
    if (r > dirty->right) dirty->right = r;
    if (b > dirty->bottom) dirty->bottom = b;
}

/* seven-segment digits: bits a(top) b(top right) c(bottom right) d(bottom) e(bottom left) f(top left) g(middle) */
static const unsigned char k_seg[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };

static void fx_rect_fill(uint32_t *bits, int W, int H, int x0, int y0, int x1, int y1, uint32_t c, RECT *dirty)
{
    int x, y;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            fx_over(&bits[(size_t)y * W + x], c);
    if (x0 < dirty->left) dirty->left = x0;
    if (y0 < dirty->top) dirty->top = y0;
    if (x1 > dirty->right) dirty->right = x1;
    if (y1 > dirty->bottom) dirty->bottom = y1;
}

static void fx_digit(uint32_t *bits, int W, int H, int x, int y, int dh, int d, uint32_t c, RECT *dirty)
{
    int t = dh / 7 > 2 ? dh / 7 : 2, dw = dh / 2, m = dh / 2;
    unsigned sg = k_seg[d];
    if (sg & 1)   fx_rect_fill(bits, W, H, x, y, x + dw, y + t, c, dirty);
    if (sg & 2)   fx_rect_fill(bits, W, H, x + dw - t, y, x + dw, y + m, c, dirty);
    if (sg & 4)   fx_rect_fill(bits, W, H, x + dw - t, y + m, x + dw, y + dh, c, dirty);
    if (sg & 8)   fx_rect_fill(bits, W, H, x, y + dh - t, x + dw, y + dh, c, dirty);
    if (sg & 16)  fx_rect_fill(bits, W, H, x, y + m, x + t, y + dh, c, dirty);
    if (sg & 32)  fx_rect_fill(bits, W, H, x, y, x + t, y + m, c, dirty);
    if (sg & 64)  fx_rect_fill(bits, W, H, x, y + m - t / 2, x + dw, y + m - t / 2 + t, c, dirty);
}

/* The grenade countdown: a small disc at the side of the screen with the whole seconds left inside. White, yellow from 3 s,
 * red from 1.5 s. Position / size come from recomp_screenfx_timer (x, y = centre, h = diameter as window fractions). */
static void fx_timer(uint32_t *bits, int W, int H, RECT *dirty)
{
    float cx = (float)s_tm_v[0] * (float)W * 0.001f, cy = (float)s_tm_v[1] * (float)H * 0.001f;
    float rad = (float)s_tm_v[2] * (float)H * 0.001f * 0.5f;
    int tenths = (int)s_tm_val, x, y, x0, y0, x1, y1, dh, secs;
    uint32_t col, ink = 0xFF1A1A1Au;

    if (rad < 8.0f || tenths < 0)
        return;
    col = tenths > 30 ? 0xFFF5F5F5u : tenths > 15 ? 0xFFFFD21Eu : 0xFFE62828u;
    x0 = (int)(cx - rad - 3); y0 = (int)(cy - rad - 3); x1 = (int)(cx + rad + 3); y1 = (int)(cy + rad + 3);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy, d = sqrtf(dx * dx + dy * dy);
            float cov = rad - d + 0.5f, a;
            uint32_t c;
            if (cov <= 0.0f)
                continue;
            if (cov > 1.0f)
                cov = 1.0f;
            c = d > rad - 3.0f ? 0xFF202020u : col;             /* thin dark rim */
            a = cov;
            fx_over(&bits[(size_t)y * W + x], ((uint32_t)(a * 235.0f) << 24)
                | ((((c >> 16) & 255u) * (uint32_t)(a * 235.0f) / 255u) << 16)
                | ((((c >> 8) & 255u) * (uint32_t)(a * 235.0f) / 255u) << 8)
                | (((c & 255u) * (uint32_t)(a * 235.0f) / 255u)));
        }
    if (x0 < dirty->left) dirty->left = x0;
    if (y0 < dirty->top) dirty->top = y0;
    if (x1 > dirty->right) dirty->right = x1;
    if (y1 > dirty->bottom) dirty->bottom = y1;
    secs = (tenths + 9) / 10;
    if (secs > 9)
        secs = 9;
    dh = (int)(rad * 1.0f);
    fx_digit(bits, W, H, (int)(cx - (float)dh * 0.25f), (int)(cy - (float)dh * 0.5f), dh, secs, ink, dirty);
}

/* ---- the overlay window and its thread ---- */
static LRESULT CALLBACK fx_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCHITTEST)
        return HTTRANSPARENT;
    if (m == WM_ERASEBKGND)
        return 1;
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI fx_thread(LPVOID unused)
{
    HWND wnd = NULL;
    HDC mem = NULL;
    HBITMAP dib = NULL, old = NULL;
    uint32_t *bits = NULL;
    int bw = 0, bh = 0, shown = 0;
    RECT prev = { 0, 0, 0, 0 };
    DWORD last_top = 0, last_tick = 0, rain_tick_ms = 100;
    float rain_acc = 0.0f;
    int rain_hz = 10;
    (void)unused;
    {   /* raindrops are composited this many times a second while it is only rain on screen (blood / dirt use ~30) */
        const char *e = getenv("RECOMP_SCREEN_RAIN_HZ");
        if (e && atoi(e) >= 2 && atoi(e) <= 30)
            rain_hz = atoi(e);
        rain_tick_ms = (DWORD)(1000 / rain_hz);
    }

    fx_load_all();
    {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = fx_wndproc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = "RecompScreenFx";
        RegisterClassA(&wc);
    }
    for (;;) {
        int active = 0, i, other = 0, raining, moving = 0, paused, bar;
        HWND game = s_game;
        MSG msg;
        DWORD now;
        RECT cr;
        POINT org = { 0, 0 };
        int W, H;

        now = GetTickCount();
        paused = (s_menu_cb && s_menu_cb()) || fx_split_screen();
        raining = !paused && s_level[FX_RAIN] > 0 && (s_rain_force || (int)(s_rain_until - now) > 0);
        bar = !paused && ((int)(s_bar_until - now) > 0 || (int)(s_tm_until - now) > 0);
        for (i = 0; i < FX_MAX; i++)
            if (s_sp[i].on) {
                active = 1;
                if (s_sp[i].kind != FX_RAIN)
                    other = 1;
                else if (s_sp[i].drip > 0.0f) {          /* a raindrop that is running down right now needs the faster tick */
                    float t = (float)(now - s_sp[i].born) / (float)s_sp[i].life;
                    if (t > s_sp[i].sa && t < s_sp[i].sb)
                        moving = 1;
                }
            }
        /* idle: sleep until a request wakes us; with a menu up, poll, so the bar comes back as soon as the menu closes */
        MsgWaitForMultipleObjects(1, &s_ev, FALSE, (active || raining || bar) ? ((other || moving || bar) ? 33 : rain_tick_ms) : (paused ? 100 : INFINITE), QS_ALLINPUT);
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!game || !IsWindow(game))
            continue;
        GetClientRect(game, &cr);
        ClientToScreen(game, &org);
        W = cr.right;
        H = cr.bottom;
        now = GetTickCount();

        /* queued requests become splatters */
        EnterCriticalSection(&s_cs);
        {
            FxReq q[FX_QUEUE];
            int n = s_qn;
            memcpy(q, s_q, sizeof q);
            s_qn = 0;
            LeaveCriticalSection(&s_cs);
            for (i = 0; i < n; i++)
                fx_spawn(q[i].kind, q[i].strength, W, H);
        }
        /* sustained rain: new drops at a steady rate (per second: low 1.5, normal 3, high 6, scaled by the rain strength) */
        paused = (s_menu_cb && s_menu_cb()) || fx_split_screen();
        raining = !paused && s_level[FX_RAIN] > 0 && (s_rain_force || (int)(s_rain_until - now) > 0);
        bar = !paused && ((int)(s_bar_until - now) > 0 || (int)(s_tm_until - now) > 0);   /* read again: the game may have set it while this thread slept */
        if (paused) {                       /* a menu / the pause screen is up: no raindrops, and the overlay is hidden */
            for (i = 0; i < FX_MAX; i++)
                if (s_sp[i].on && s_sp[i].kind == FX_RAIN) {
                    free(s_sp[i].pm);
                    s_sp[i].pm = NULL;
                    s_sp[i].on = 0;
                }
            if (shown && wnd) {
                ShowWindow(wnd, SW_HIDE);
                shown = 0;
            }
            last_tick = now;
            continue;
        }
        if (raining) {
            float dt = last_tick ? (float)(now - last_tick) * 0.001f : 0.0f;
            float rate = (s_level[FX_RAIN] == 1 ? 1.5f : s_level[FX_RAIN] == 3 ? 6.0f : 3.0f)
                       * (s_rain_force ? 1.0f : (float)s_rain_x1000 * 0.001f);
            if (dt > 0.25f)
                dt = 0.25f;
            rain_acc += rate * dt;
            while (rain_acc >= 1.0f) {
                fx_spawn(FX_RAIN, 1.0f, W, H);
                rain_acc -= 1.0f;
            }
        } else {
            rain_acc = 0.0f;
        }
        last_tick = now;
        for (i = 0; i < FX_MAX; i++)
            if (s_sp[i].on && now - s_sp[i].born >= s_sp[i].life) {
                free(s_sp[i].pm);
                s_sp[i].pm = NULL;
                s_sp[i].on = 0;
            }
        active = bar;
        for (i = 0; i < FX_MAX; i++)
            if (s_sp[i].on)
                active = 1;
        if (!active || IsIconic(game) || GetForegroundWindow() != game || W < 64 || H < 64) {
            if (shown && wnd) {
                ShowWindow(wnd, SW_HIDE);
                shown = 0;
            }
            if (!active)
                continue;
            if (IsIconic(game) || GetForegroundWindow() != game)
                continue;
        }
        if (!wnd) {
            wnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                                  "RecompScreenFx", "", WS_POPUP, org.x, org.y, W, H, NULL, NULL, GetModuleHandleA(NULL), NULL);
            if (!wnd)
                continue;
        }
        if (!dib || bw != W || bh != H) {
            BITMAPINFO bi;
            if (dib) {
                SelectObject(mem, old);
                DeleteObject(dib);
            }
            if (!mem)
                mem = CreateCompatibleDC(NULL);
            memset(&bi, 0, sizeof bi);
            bi.bmiHeader.biSize = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth = W;
            bi.bmiHeader.biHeight = -H;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
            if (!dib) {
                bw = bh = 0;
                continue;
            }
            old = (HBITMAP)SelectObject(mem, dib);
            bw = W;
            bh = H;
            prev.left = 0; prev.top = 0; prev.right = W; prev.bottom = H;   /* clear all of it once */
        }

        /* clear what was drawn last time, then composite every splatter */
        {
            RECT dirty = { W, H, 0, 0 };
            int y;
            for (y = prev.top; y < prev.bottom && y < H; y++)
                if (prev.right > prev.left)
                    memset(bits + (size_t)y * W + prev.left, 0, (size_t)(prev.right - prev.left) * 4);
            for (i = 0; i < FX_MAX; i++) {
                FxSplat *s = &s_sp[i];
                float f, t;
                int k, x0, y0, x, sx0, sy0, sx1, sy1, ox, oy, rh;
                uint32_t fw = 0;
                float oyf;
                if (!s->on || !s->pm)
                    continue;
                f = fx_factor(s, now) * s->peak;
                if (f <= 0.002f)
                    continue;
                t = (float)(now - s->born) / (float)s->life;
                k = (int)(f * 256.0f);
                ox = (int)(s->xf * (float)W) - s->w / 2;
                if (s->kind == FX_RAIN) {           /* constant speed from sa to sb; a vertical sub-pixel lerp keeps it smooth */
                    float run = t <= s->sa ? 0.0f : t >= s->sb ? 1.0f : (t - s->sa) / (s->sb - s->sa);
                    oyf = s->yf * (float)H - (float)(s->h / 2) + s->drip * (float)H * run;
                } else {
                    oyf = s->yf * (float)H - (float)(s->h / 2) + s->drip * (float)H * (1.0f - (1.0f - t) * (1.0f - t));
                }
                oy = (int)floorf(oyf);
                if (s->kind == FX_RAIN)
                    fw = (uint32_t)((oyf - (float)oy) * 256.0f) & 255u;
                rh = s->h + (fw ? 1 : 0);
                sx0 = ox < 0 ? -ox : 0;
                sy0 = oy < 0 ? -oy : 0;
                sx1 = ox + s->w > W ? W - ox : s->w;
                sy1 = oy + rh > H ? H - oy : rh;
                if (sx1 <= sx0 || sy1 <= sy0)
                    continue;
                for (y0 = sy0; y0 < sy1; y0++) {
                    const uint32_t *src = s->pm + (size_t)(y0 < s->h ? y0 : s->h - 1) * s->w;
                    const uint32_t *srcb = s->pm + (size_t)(y0 > 0 ? y0 - 1 : 0) * s->w;
                    uint32_t *dst = bits + (size_t)(oy + y0) * W + ox;
                    for (x0 = sx0; x0 < sx1; x0++) {
                        uint32_t p = src[x0], d;
                        uint32_t a, r, g, b, ia;
                        if (fw) {                   /* dest row y0 lies between source rows y0-1 (weight fw) and y0 */
                            uint32_t pa = y0 < s->h ? p : 0, pb = y0 > 0 ? srcb[x0] : 0;
                            p = ((((pa >> 24) * (256 - fw) + (pb >> 24) * fw) >> 8) << 24)
                              | (((((pa >> 16) & 255) * (256 - fw) + ((pb >> 16) & 255) * fw) >> 8) << 16)
                              | (((((pa >> 8) & 255) * (256 - fw) + ((pb >> 8) & 255) * fw) >> 8) << 8)
                              | ((((pa & 255) * (256 - fw) + (pb & 255) * fw) >> 8));
                        }
                        if (!(p >> 24))
                            continue;
                        a = ((p >> 24) * (uint32_t)k) >> 8;
                        r = (((p >> 16) & 255) * (uint32_t)k) >> 8;
                        g = (((p >> 8) & 255) * (uint32_t)k) >> 8;
                        b = ((p & 255) * (uint32_t)k) >> 8;
                        d = dst[x0];
                        ia = 256 - a;
                        dst[x0] = ((a + (((d >> 24) * ia) >> 8)) << 24)
                                | ((r + ((((d >> 16) & 255) * ia) >> 8)) << 16)
                                | ((g + ((((d >> 8) & 255) * ia) >> 8)) << 8)
                                | (b + (((d & 255) * ia) >> 8));
                    }
                }
                (void)x;
                (void)rh;
                if (ox + sx0 < dirty.left) dirty.left = ox + sx0;
                if (oy + sy0 < dirty.top) dirty.top = oy + sy0;
                if (ox + sx1 > dirty.right) dirty.right = ox + sx1;
                if (oy + sy1 > dirty.bottom) dirty.bottom = oy + sy1;
            }
            if (bar && (int)(s_bar_until - now) > 0)
                fx_bar(bits, W, H, &dirty);
            if (bar && (int)(s_tm_until - now) > 0)
                fx_timer(bits, W, H, &dirty);
            {
                RECT upd = dirty;
                POINT dst = org, src = { 0, 0 };
                SIZE sz = { W, H };
                BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
                UPDATELAYEREDWINDOWINFO ui;
                if (prev.right > prev.left) {              /* the area to refresh is last frame's plus this frame's */
                    if (prev.left < upd.left) upd.left = prev.left;
                    if (prev.top < upd.top) upd.top = prev.top;
                    if (prev.right > upd.right) upd.right = prev.right;
                    if (prev.bottom > upd.bottom) upd.bottom = prev.bottom;
                }
                if (upd.right <= upd.left || upd.bottom <= upd.top) {
                    upd.left = 0; upd.top = 0; upd.right = 1; upd.bottom = 1;
                }
                if (upd.left < 0) upd.left = 0;
                if (upd.top < 0) upd.top = 0;
                if (upd.right > W) upd.right = W;
                if (upd.bottom > H) upd.bottom = H;
                memset(&ui, 0, sizeof ui);
                ui.cbSize = sizeof ui;
                ui.hdcDst = NULL;
                ui.pptDst = &dst;
                ui.psize = &sz;
                ui.hdcSrc = mem;
                ui.pptSrc = &src;
                ui.pblend = &bf;
                ui.dwFlags = ULW_ALPHA;
                ui.prcDirty = &upd;
                if (!shown || now - last_top > 1500) {
                    SetWindowPos(wnd, HWND_TOPMOST, org.x, org.y, W, H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
                    last_top = now;
                    shown = 1;
                    ui.prcDirty = NULL;                    /* full update after (re)showing */
                }
                UpdateLayeredWindowIndirect(wnd, &ui);
                prev = dirty.right > dirty.left ? dirty : upd;
            }
        }
    }
    return 0;
}

static void fx_start(void)
{
    if (InterlockedCompareExchange(&s_started, 1, 0))
        return;
    InitializeCriticalSection(&s_cs);
    s_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    {
        const char *b = getenv("RECOMP_SCREEN_BLOOD"), *d = getenv("RECOMP_SCREEN_DIRT"), *r = getenv("RECOMP_SCREEN_RAIN");
        const char *rf = getenv("RECOMP_SCREEN_RAIN_FORCE");
        if (b && *b) s_level[FX_BLOOD] = atoi(b) < 0 ? 0 : atoi(b) > 3 ? 3 : atoi(b);
        if (d && *d) s_level[FX_DIRT] = atoi(d) < 0 ? 0 : atoi(d) > 3 ? 3 : atoi(d);
        if (r && *r) s_level[FX_RAIN] = atoi(r) < 0 ? 0 : atoi(r) > 3 ? 3 : atoi(r);
        s_rain_force = rf && *rf == '1';
    }
    s_thread = CreateThread(NULL, 0, fx_thread, NULL, 0, NULL);
}

/* called by the framebuffer window thread once the game window exists */
void recomp_screenfx_set_game_window(void *hwnd)
{
    s_game = (HWND)hwnd;
    fx_start();
}

int recomp_screenfx_get(int kind)
{
    fx_start();
    return s_level[kind < 0 ? 0 : kind >= FX_KINDS ? FX_KINDS - 1 : kind];
}

void recomp_screenfx_set(int kind, int level)
{
    fx_start();
    s_level[kind < 0 ? 0 : kind >= FX_KINDS ? FX_KINDS - 1 : kind] = level < 0 ? 0 : level > 3 ? 3 : level;
}

/* The game side calls this every frame (or every few) while the player is out in the rain: strength 0..1 keeps the
 * raindrops coming for the next 1.5 s; 0 lets the rain stop (existing drops finish their life). */
void recomp_screenfx_rain(float strength)
{
    fx_start();
    if (strength <= 0.0f || !s_game) {
        s_rain_until = 0;
        return;
    }
    s_rain_x1000 = (LONG)((strength > 1.0f ? 1.0f : strength) * 1000.0f);
    s_rain_until = GetTickCount() + 1500;
    SetEvent(s_ev);
}

/* 1 while drops are being made (for the log line on the game side) */
int recomp_screenfx_rain_on(void)
{
    return s_level[FX_RAIN] > 0 && (s_rain_force || (int)(s_rain_until - GetTickCount()) > 0);
}

/* strength 0..1. Ignored while that effect is off, and at most one new splatter per 140 ms per kind. */
void recomp_screenfx_trigger(int kind, float strength)
{
    DWORD now = GetTickCount();
    kind = kind == 1 ? 1 : 0;
    fx_start();
    if (s_level[kind] <= 0 || !s_game)
        return;
    if (now - s_last_trig[kind] < 140)
        return;
    s_last_trig[kind] = now;
    EnterCriticalSection(&s_cs);
    if (s_qn < FX_QUEUE) {
        s_q[s_qn].kind = kind;
        s_q[s_qn].strength = strength;
        s_qn++;
    }
    LeaveCriticalSection(&s_cs);
    SetEvent(s_ev);
}
