/**
 * The game window's mouse pointer: a rifle round (copper-jacketed bullet in
 * a brass case) lying at 45 degrees with its tip up and to the left, which is
 * also the hot spot. Drawn here from a handful of profile numbers with 4x4
 * supersampling, a dark outline and a soft shadow so it reads on the dark
 * menu backgrounds as well as on bright ones.
 *
 * The window class takes it as its cursor (fb_present.c). While the mouse
 * aims in play the input layer swaps in a blank one and puts this one back
 * afterwards (mkb_input.c), so nothing else has to know about it.
 */
#include "war_cursor.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WC_SS      4        /* samples per pixel along each axis */
#define WC_DESIGN  40.0f    /* the picture is drawn for a 40 pixel square */
#define WC_TIP     2.5f     /* where the tip lies, x and y, in design pixels */
#define WC_LEN     43.0f    /* length of the round, tip to base */

enum { WC_NONE, WC_COPPER, WC_BRASS, WC_GROOVE };

/* The round's silhouette: what it is made of, and its half width, at
 * distance s from the tip along its axis. */
static int round_at(float s, float *half)
{
    if (s < 0.0f || s > WC_LEN)
        return WC_NONE;
    if (s < 15.0f) { *half = 4.3f * powf(s / 15.0f, 0.62f); return WC_COPPER; }
    if (s < 16.2f) { *half = 4.3f; return WC_GROOVE; }                     /* crimp */
    if (s < 21.0f) { *half = 4.5f; return WC_BRASS; }                      /* neck */
    if (s < 26.5f) { *half = 4.5f + 1.5f * (s - 21.0f) / 5.5f; return WC_BRASS; }   /* shoulder */
    if (s < 40.0f) { *half = 6.0f + 0.3f * (s - 26.5f) / 13.5f; return WC_BRASS; }  /* case */
    if (s < 41.2f) { *half = 5.2f; return WC_GROOVE; }                     /* extractor groove */
    *half = 6.6f;                                                          /* rim */
    return WC_BRASS;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Colour of a point on the round: a cylinder lit from the upper right.
 * u is -1 on the edge facing the light and +1 on the far edge. */
static uint32_t shade(int m, float u)
{
    static const float base[4][3] = {
        { 0.0f, 0.0f, 0.0f },
        { 206.0f, 118.0f, 64.0f },      /* copper jacket */
        { 228.0f, 186.0f, 86.0f },      /* brass case */
        { 52.0f, 38.0f, 22.0f },        /* groove */
    };
    float nz = sqrtf(clampf(1.0f - u * u, 0.0f, 1.0f));
    float diff = clampf(-0.6f * u + 0.8f * nz, 0.0f, 1.0f);
    float spec = powf(clampf(-0.316f * u + 0.949f * nz, 0.0f, 1.0f), 18.0f);
    float bounce = clampf((u - 0.55f) / 0.45f, 0.0f, 1.0f) * 0.22f;
    float l = 0.30f + 0.70f * diff + bounce;
    float c[3];
    int i;

    for (i = 0; i < 3; i++) {
        c[i] = base[m][i] * l;
        if (m != WC_GROOVE)
            c[i] += 255.0f * spec * 0.55f;
        c[i] = clampf(c[i], 0.0f, 255.0f);
    }
    return ((uint32_t)(c[0] + 0.5f) << 16) | ((uint32_t)(c[1] + 0.5f) << 8) | (uint32_t)(c[2] + 0.5f);
}

void war_cursor_render(uint32_t *argb, int n, int *hot_x, int *hot_y)
{
    const float k = (float)n / WC_DESIGN;
    const int hn = n * WC_SS;
    const float to_design = 1.0f / (WC_SS * k);       /* one sample, in design pixels */
    const float d_unit = 3.0f * WC_SS * k;            /* chamfer distance -> design px */
    const int sh_x = (int)(1.4f * WC_SS * k + 0.5f);  /* the shadow's offset */
    const int sh_y = (int)(1.8f * WC_SS * k + 0.5f);
    uint8_t *mat = (uint8_t *)calloc((size_t)hn * hn, 1);
    uint32_t *col = (uint32_t *)calloc((size_t)hn * hn, sizeof(uint32_t));
    int *dist = (int *)malloc((size_t)hn * hn * sizeof(int));
    int x, y, ox, oy, sx, sy;

    if (!mat || !col || !dist) {
        memset(argb, 0, (size_t)n * n * sizeof(uint32_t));
        free(mat); free(col); free(dist);
        *hot_x = *hot_y = 0;
        return;
    }

    /* the round itself */
    for (y = 0; y < hn; y++) {
        for (x = 0; x < hn; x++) {
            float dx = ((float)x + 0.5f) * to_design - WC_TIP;
            float dy = ((float)y + 0.5f) * to_design - WC_TIP;
            float s = (dx + dy) * 0.70710678f;
            float t = (dy - dx) * 0.70710678f;
            float half = 0.0f;
            int m = round_at(s, &half);
            if (m != WC_NONE && fabsf(t) <= half) {
                mat[y * hn + x] = (uint8_t)m;
                col[y * hn + x] = shade(m, t / half);
            }
        }
    }

    /* distance to the round (3-4 chamfer) for the outline and the shadow */
    for (y = 0; y < hn; y++)
        for (x = 0; x < hn; x++)
            dist[y * hn + x] = mat[y * hn + x] ? 0 : (1 << 28);
    for (y = 0; y < hn; y++) {
        for (x = 0; x < hn; x++) {
            int d = dist[y * hn + x];
            if (x > 0 && dist[y * hn + x - 1] + 3 < d) d = dist[y * hn + x - 1] + 3;
            if (y > 0) {
                if (dist[(y - 1) * hn + x] + 3 < d) d = dist[(y - 1) * hn + x] + 3;
                if (x > 0 && dist[(y - 1) * hn + x - 1] + 4 < d) d = dist[(y - 1) * hn + x - 1] + 4;
                if (x + 1 < hn && dist[(y - 1) * hn + x + 1] + 4 < d) d = dist[(y - 1) * hn + x + 1] + 4;
            }
            dist[y * hn + x] = d;
        }
    }
    for (y = hn - 1; y >= 0; y--) {
        for (x = hn - 1; x >= 0; x--) {
            int d = dist[y * hn + x];
            if (x + 1 < hn && dist[y * hn + x + 1] + 3 < d) d = dist[y * hn + x + 1] + 3;
            if (y + 1 < hn) {
                if (dist[(y + 1) * hn + x] + 3 < d) d = dist[(y + 1) * hn + x] + 3;
                if (x + 1 < hn && dist[(y + 1) * hn + x + 1] + 4 < d) d = dist[(y + 1) * hn + x + 1] + 4;
                if (x > 0 && dist[(y + 1) * hn + x - 1] + 4 < d) d = dist[(y + 1) * hn + x - 1] + 4;
            }
            dist[y * hn + x] = d;
        }
    }

    /* average the samples down to pixels, outline and shadow included */
    for (oy = 0; oy < n; oy++) {
        for (ox = 0; ox < n; ox++) {
            float ar = 0.0f, ag = 0.0f, ab = 0.0f, aa = 0.0f;
            for (sy = 0; sy < WC_SS; sy++) {
                for (sx = 0; sx < WC_SS; sx++) {
                    int px = ox * WC_SS + sx, py = oy * WC_SS + sy;
                    int i = py * hn + px;
                    float r, g, b, a;
                    if (mat[i]) {
                        r = (float)((col[i] >> 16) & 255);
                        g = (float)((col[i] >> 8) & 255);
                        b = (float)(col[i] & 255);
                        a = 1.0f;
                    } else {
                        float d = (float)dist[i] / d_unit;
                        float a_o = clampf(1.0f - (d - 0.9f) / 0.8f, 0.0f, 1.0f);
                        float d_sh = 99.0f, a_sh;
                        int qx = px - sh_x, qy = py - sh_y;
                        if (qx >= 0 && qy >= 0)
                            d_sh = (float)dist[qy * hn + qx] / d_unit;
                        a_sh = 0.40f * clampf(1.0f - d_sh / 2.6f, 0.0f, 1.0f);
                        a = a_o + a_sh * (1.0f - a_o);
                        r = 16.0f; g = 12.0f; b = 8.0f;
                    }
                    ar += r * a; ag += g * a; ab += b * a; aa += a;
                }
            }
            if (aa <= 0.0f) {
                argb[oy * n + ox] = 0;
            } else {
                float alpha = aa / (float)(WC_SS * WC_SS);
                argb[oy * n + ox] = ((uint32_t)(alpha * 255.0f + 0.5f) << 24)
                    | ((uint32_t)(ar / aa + 0.5f) << 16)
                    | ((uint32_t)(ag / aa + 0.5f) << 8)
                    | (uint32_t)(ab / aa + 0.5f);
            }
        }
    }

    *hot_x = (int)(WC_TIP * k);
    *hot_y = (int)(WC_TIP * k);
    free(mat); free(col); free(dist);
}

#if defined(_WIN32)

/* A w x h array of straight-alpha 0xAARRGGBB pixels as a Windows cursor: a
 * 32-bit bitmap with alpha and an all-zero mask (ignored while the bitmap
 * carries alpha). */
static HCURSOR cursor_from_argb(const uint32_t *px, int w, int h, int hx, int hy)
{
    BITMAPV5HEADER bi;
    HDC dc;
    HBITMAP color, mask = NULL;
    void *bits = NULL;
    BYTE *mask_bits;
    ICONINFO ii;
    HCURSOR cur = NULL;
    int mask_pitch;

    memset(&bi, 0, sizeof bi);
    bi.bV5Size = sizeof bi;
    bi.bV5Width = w;
    bi.bV5Height = -h;                  /* top row first */
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask   = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask  = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    dc = GetDC(NULL);
    color = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!color || !bits) {
        if (color) DeleteObject(color);
        return NULL;
    }
    memcpy(bits, px, (size_t)w * h * sizeof(uint32_t));

    mask_pitch = ((w + 15) / 16) * 2;
    mask_bits = (BYTE *)calloc((size_t)mask_pitch * h, 1);
    if (mask_bits)
        mask = CreateBitmap(w, h, 1, 1, mask_bits);
    free(mask_bits);
    if (mask) {
        ii.fIcon = FALSE;
        ii.xHotspot = (DWORD)hx;
        ii.yHotspot = (DWORD)hy;
        ii.hbmMask = mask;
        ii.hbmColor = color;
        cur = (HCURSOR)CreateIconIndirect(&ii);
        DeleteObject(mask);
    }
    DeleteObject(color);
    return cur;
}

/* The drawn round, at a size that follows the system's cursor size. */
static HCURSOR make_drawn_cursor(void)
{
    int n = GetSystemMetrics(SM_CXCURSOR) * 5 / 4;     /* 40 for the usual 32 */
    int hx = 0, hy = 0;
    uint32_t *px;
    HCURSOR cur;

    if (n < 32) n = 32;
    if (n > 128) n = 128;
    px = (uint32_t *)malloc((size_t)n * n * sizeof(uint32_t));
    if (!px)
        return NULL;
    war_cursor_render(px, n, &hx, &hy);
    cur = cursor_from_argb(px, n, n, hx, hy);
    free(px);
    return cur;
}

/* ---- a picture file (png, bmp, jpg, gif) as the pointer ----
 * Read through GDI+, loaded at run time so nothing extra is linked. */
typedef struct { UINT w, h; INT stride; INT fmt; void *scan0; UINT_PTR reserved; } wc_bitmapdata;
typedef struct { INT x, y, w, h; } wc_rect;
typedef struct { UINT32 version; void *callback; BOOL no_thread; BOOL no_codecs; } wc_startup;

/* straight-alpha 0xAARRGGBB, malloc'ed, or NULL */
static uint32_t *load_picture(const char *path, int *out_w, int *out_h)
{
    typedef int (WINAPI *startup_fn)(ULONG_PTR *, const wc_startup *, void *);
    typedef int (WINAPI *fromfile_fn)(const WCHAR *, void **);
    typedef int (WINAPI *dim_fn)(void *, UINT *);
    typedef int (WINAPI *lock_fn)(void *, const wc_rect *, UINT, INT, wc_bitmapdata *);
    typedef int (WINAPI *unlock_fn)(void *, wc_bitmapdata *);
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
    wc_startup su;
    void *bmp = NULL;
    UINT w = 0, h = 0, y;
    wc_rect rc;
    wc_bitmapdata bd;
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
    if (!MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH)
        || !GetFullPathNameW(wide, MAX_PATH, full, NULL))
        return NULL;
    if (fromfile(full, &bmp) != 0 || !bmp)
        return NULL;
    get_w(bmp, &w);
    get_h(bmp, &h);
    if (w > 0 && h > 0 && w <= 4096 && h <= 4096) {
        rc.x = 0; rc.y = 0; rc.w = (INT)w; rc.h = (INT)h;
        memset(&bd, 0, sizeof bd);
        /* ImageLockModeRead, PixelFormat32bppARGB (not premultiplied) */
        if (lock(bmp, &rc, 1u, 0x0026200A, &bd) == 0) {
            px = (uint32_t *)malloc((size_t)w * h * sizeof(uint32_t));
            if (px)
                for (y = 0; y < h; y++)
                    memcpy(px + (size_t)y * w, (const BYTE *)bd.scan0 + (ptrdiff_t)y * bd.stride,
                           (size_t)w * sizeof(uint32_t));
            unlock(bmp, &bd);
        }
    }
    dispose(bmp);
    *out_w = (int)w;
    *out_h = (int)h;
    return px;
}

static void fetch_premul(const uint32_t *src, int sw, int sh, int x, int y, float *o)
{
    uint32_t p;
    float a;
    if (x < 0) x = 0;
    if (x >= sw) x = sw - 1;
    if (y < 0) y = 0;
    if (y >= sh) y = sh - 1;
    p = src[y * sw + x];
    a = (float)(p >> 24) / 255.0f;
    o[0] = a;
    o[1] = (float)((p >> 16) & 255) * a;
    o[2] = (float)((p >> 8) & 255) * a;
    o[3] = (float)(p & 255) * a;
}

/* Resample a picture (4 x 4 bilinear samples per new pixel, alpha-weighted). */
static uint32_t *scale_picture(const uint32_t *src, int sw, int sh, int dw, int dh)
{
    uint32_t *dst = (uint32_t *)malloc((size_t)dw * dh * sizeof(uint32_t));
    int x, y, i, j;

    if (!dst)
        return NULL;
    for (y = 0; y < dh; y++) {
        for (x = 0; x < dw; x++) {
            float acc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            for (j = 0; j < 4; j++) {
                for (i = 0; i < 4; i++) {
                    float fx = ((float)x + ((float)i + 0.5f) / 4.0f) * (float)sw / (float)dw - 0.5f;
                    float fy = ((float)y + ((float)j + 0.5f) / 4.0f) * (float)sh / (float)dh - 0.5f;
                    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
                    float wx = fx - (float)x0, wy = fy - (float)y0;
                    float t[4], c;
                    int k;
                    for (k = 0; k < 4; k++) {
                        fetch_premul(src, sw, sh, x0 + (k & 1), y0 + (k >> 1), t);
                        c = ((k & 1) ? wx : 1.0f - wx) * ((k >> 1) ? wy : 1.0f - wy);
                        acc[0] += t[0] * c; acc[1] += t[1] * c;
                        acc[2] += t[2] * c; acc[3] += t[3] * c;
                    }
                }
            }
            if (acc[0] <= 0.0f) {
                dst[y * dw + x] = 0;
            } else {
                float a = acc[0] / 16.0f;
                float r = acc[1] / acc[0], g = acc[2] / acc[0], b = acc[3] / acc[0];
                dst[y * dw + x] = ((uint32_t)(a * 255.0f + 0.5f) << 24)
                    | ((uint32_t)clampf(r + 0.5f, 0.0f, 255.0f) << 16)
                    | ((uint32_t)clampf(g + 0.5f, 0.0f, 255.0f) << 8)
                    | (uint32_t)clampf(b + 0.5f, 0.0f, 255.0f);
            }
        }
    }
    return dst;
}

static int color_dist(uint32_t a, uint32_t b)
{
    int dr = (int)((a >> 16) & 255) - (int)((b >> 16) & 255);
    int dg = (int)((a >> 8) & 255) - (int)((b >> 8) & 255);
    int db = (int)(a & 255) - (int)(b & 255);
    if (dr < 0) dr = -dr;
    if (dg < 0) dg = -dg;
    if (db < 0) db = -db;
    return dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
}

/* A picture with no transparency of its own and one flat colour in all four
 * corners (a screenshot of a pointer on a grey page): that colour, wherever
 * it connects to the border, becomes transparent, and the pixels just inside
 * it get a partial alpha and the background taken back out of their colour,
 * so no grey halo is left around the edge. RECOMP_CURSOR_KEY=0 skips it. */
static void remove_background(uint32_t *px, int w, int h)
{
    const uint32_t key = px[0] & 0x00FFFFFFu;
    uint8_t *bg;
    int *stack;
    int sp = 0, x, y, i, band;
    const int corners[4] = { 0, w - 1, (h - 1) * w, (h - 1) * w + w - 1 };

    for (i = 0; i < w * h; i++)
        if ((px[i] >> 24) < 250u)
            return;
    for (i = 0; i < 4; i++)
        if (color_dist(px[corners[i]], key) > 24)
            return;

    bg = (uint8_t *)calloc((size_t)w * h, 1);
    stack = (int *)malloc((size_t)w * h * sizeof(int));
    if (!bg || !stack) {
        free(bg); free(stack);
        return;
    }
    for (x = 0; x < w; x++) {
        if (color_dist(px[x], key) <= 16 && !bg[x]) { bg[x] = 1; stack[sp++] = x; }
        i = (h - 1) * w + x;
        if (color_dist(px[i], key) <= 16 && !bg[i]) { bg[i] = 1; stack[sp++] = i; }
    }
    for (y = 0; y < h; y++) {
        i = y * w;
        if (color_dist(px[i], key) <= 16 && !bg[i]) { bg[i] = 1; stack[sp++] = i; }
        i = y * w + w - 1;
        if (color_dist(px[i], key) <= 16 && !bg[i]) { bg[i] = 1; stack[sp++] = i; }
    }
    while (sp > 0) {
        int p = stack[--sp], px_x = p % w, px_y = p / w;
        static const int dx4[4] = { 1, -1, 0, 0 }, dy4[4] = { 0, 0, 1, -1 };
        for (i = 0; i < 4; i++) {
            int nx = px_x + dx4[i], ny = px_y + dy4[i], q;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                continue;
            q = ny * w + nx;
            if (!bg[q] && color_dist(px[q], key) <= 16) { bg[q] = 1; stack[sp++] = q; }
        }
    }
    {   /* pockets of the same colour inside a loop of the pointer (the hole of a
         * triangle): any such area of a few dozen pixels goes too */
        uint8_t *seen = (uint8_t *)calloc((size_t)w * h, 1);
        if (seen) {
            int start;
            for (start = 0; start < w * h; start++) {
                int head = 0, count = 0;
                if (bg[start] || seen[start] || color_dist(px[start], key) > 16)
                    continue;
                stack[count++] = start;
                seen[start] = 1;
                while (head < count) {
                    int p = stack[head++], qx = p % w, qy = p / w, d;
                    static const int dx4[4] = { 1, -1, 0, 0 }, dy4[4] = { 0, 0, 1, -1 };
                    for (d = 0; d < 4; d++) {
                        int nx = qx + dx4[d], ny = qy + dy4[d], q;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                            continue;
                        q = ny * w + nx;
                        if (!bg[q] && !seen[q] && color_dist(px[q], key) <= 16) {
                            seen[q] = 1;
                            stack[count++] = q;
                        }
                    }
                }
                if (count >= 24)
                    for (i = 0; i < count; i++)
                        bg[stack[i]] = 1;
            }
            free(seen);
        }
    }
    /* the pixels within two of the background: partial alpha, key colour out */
    {
        uint8_t *lvl = (uint8_t *)calloc((size_t)w * h, 1);   /* 1, 2: steps in from the background */
        if (lvl) {
            for (band = 1; band <= 2; band++) {
                for (y = 0; y < h; y++) {
                    for (x = 0; x < w; x++) {
                        int adj = 0, dy, dx;
                        if (bg[y * w + x] || lvl[y * w + x])
                            continue;
                        for (dy = -1; dy <= 1 && !adj; dy++)
                            for (dx = -1; dx <= 1; dx++) {
                                int nx = x + dx, ny = y + dy;
                                if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                                    continue;
                                if (band == 1 ? bg[ny * w + nx] : lvl[ny * w + nx] == 1) { adj = 1; break; }
                            }
                        if (adj) lvl[y * w + x] = (uint8_t)(band + 2);   /* +2: not yet a source for this pass */
                    }
                }
                for (i = 0; i < w * h; i++)
                    if (lvl[i] >= 3)
                        lvl[i] -= 2;
            }
            for (i = 0; i < w * h; i++) {
                int d, ch;
                float a;
                uint32_t out = 0;
                if (!lvl[i])
                    continue;
                d = color_dist(px[i], key);
                a = clampf(((float)d - 10.0f) / 60.0f, 0.0f, 1.0f);
                for (ch = 0; ch < 3; ch++) {
                    int sh = 16 - 8 * ch;
                    float c = (float)((px[i] >> sh) & 255), k = (float)((key >> sh) & 255);
                    c = a > 0.02f ? k + (c - k) / a : c;
                    out |= (uint32_t)clampf(c + 0.5f, 0.0f, 255.0f) << sh;
                }
                px[i] = ((uint32_t)(a * 255.0f + 0.5f) << 24) | out;
            }
            free(lvl);
        }
    }
    for (i = 0; i < w * h; i++)
        if (bg[i])
            px[i] = 0;
    free(bg);
    free(stack);
}

/* A cursor from a file: .cur / .ani as they are, anything else as a picture.
 * RECOMP_CURSOR_SIZE = pixels of the picture's longer side (default: as it
 * is, but at most as big as the drawn round, 40), RECOMP_CURSOR_HOT = x,y of the pointing spot in the
 * picture's own pixels (default: the solid pixel nearest the top left, the
 * tip of an arrow). Margins of no alpha are cut off, and a flat-coloured
 * page behind a pointer is made transparent (RECOMP_CURSOR_KEY=0 keeps it). */
static HCURSOR cursor_from_file(const char *path)
{
    const char *dot = strrchr(path, '.');
    int w = 0, h = 0, hx = 0, hy = 0, size = 0, dw, dh;
    uint32_t *px, *sc;
    const char *e;
    HCURSOR cur = NULL;

    if (dot && (!_stricmp(dot, ".cur") || !_stricmp(dot, ".ani")))
        return (HCURSOR)LoadImageA(NULL, path, IMAGE_CURSOR, 0, 0, LR_LOADFROMFILE);

    px = load_picture(path, &w, &h);
    if (!px)
        return NULL;
    if (!((e = getenv("RECOMP_CURSOR_KEY")) != NULL && *e == '0'))
        remove_background(px, w, h);
    {   /* the pointing spot (given, else the solid pixel nearest the top left
         * corner: the tip of an arrow), then the empty margin cut away */
        int have_hot = 0, x, y, best = 1 << 30, x0 = w, y0 = h, x1 = -1, y1 = -1;
        if ((e = getenv("RECOMP_CURSOR_HOT")) != NULL && sscanf(e, "%d , %d", &hx, &hy) == 2)
            have_hot = 1;
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                uint32_t a = px[y * w + x] >> 24;
                if (!have_hot && a >= 128u && x + y < best) { best = x + y; hx = x; hy = y; }
                if (a > 8u) {
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
            }
        }
        if (x1 >= x0 && y1 >= y0 && (x1 - x0 + 1 < w || y1 - y0 + 1 < h)) {
            int nw = x1 - x0 + 1, nh = y1 - y0 + 1;
            uint32_t *cp = (uint32_t *)malloc((size_t)nw * nh * sizeof(uint32_t));
            if (cp) {
                for (y = 0; y < nh; y++)
                    memcpy(cp + (size_t)y * nw, px + (size_t)(y + y0) * w + x0, (size_t)nw * sizeof(uint32_t));
                free(px);
                px = cp;
                hx -= x0; hy -= y0;
                w = nw; h = nh;
            }
        }
    }
    if ((e = getenv("RECOMP_CURSOR_SIZE")) != NULL)
        size = atoi(e);
    if (size < 8 || size > 128) {       /* as big as the drawn round at most */
        int cap = GetSystemMetrics(SM_CXCURSOR) * 5 / 4;
        if (cap < 32) cap = 32;
        if (cap > 128) cap = 128;
        size = (w > h ? w : h) <= cap ? 0 : cap;
    }
    if (hx < 0) hx = 0;
    if (hx >= w) hx = w - 1;
    if (hy < 0) hy = 0;
    if (hy >= h) hy = h - 1;

    dw = w; dh = h;
    if (size && size != (w > h ? w : h)) {
        dw = w >= h ? size : (int)((float)w * size / (float)h + 0.5f);
        dh = h >= w ? size : (int)((float)h * size / (float)w + 0.5f);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        sc = scale_picture(px, w, h, dw, dh);
        if (sc) {
            free(px);
            px = sc;
            hx = hx * dw / w;
            hy = hy * dh / h;
        } else {
            dw = w; dh = h;
        }
    }
    if (dw > 128 || dh > 128) {     /* too large for a cursor */
        free(px);
        return NULL;
    }
    cur = cursor_from_argb(px, dw, dh, hx, hy);
    free(px);
    fprintf(stderr, "  [CURSOR] %s %dx%d -> %dx%d, pointing spot %d,%d\n", path, w, h, dw, dh, hx, hy);
    return cur;
}

HCURSOR recomp_war_cursor(void)
{
    static const char *const names[] = {
        "menu_cursor.cur", "menu_cursor.png", "menu_cursor.bmp", "menu_cursor.jpg", "menu_cursor.gif"
    };
    static HCURSOR cur;
    static int done;
    const char *e;
    size_t i;

    if (done)
        return cur;
    done = 1;

    e = getenv("RECOMP_CURSOR");
    if (e && (!_stricmp(e, "default") || !_stricmp(e, "arrow") || !_stricmp(e, "0")))
        return cur = NULL;
    if (e && *e) {
        cur = cursor_from_file(e);
        if (cur)
            return cur;
        fprintf(stderr, "  [CURSOR] cannot load '%s', using the drawn one\n", e);
    }
    {   /* the working directory first, then the folder of the program itself */
        char exe_dir[MAX_PATH], path[MAX_PATH + 32];
        char *slash;
        int pass;
        exe_dir[0] = 0;
        if (GetModuleFileNameA(NULL, exe_dir, MAX_PATH) && (slash = strrchr(exe_dir, '\\')) != NULL)
            slash[1] = 0;
        else
            exe_dir[0] = 0;
        for (pass = 0; pass < 2 && !cur; pass++) {
            if (pass == 1 && !exe_dir[0])
                break;
            for (i = 0; i < sizeof names / sizeof names[0] && !cur; i++) {
                snprintf(path, sizeof path, "%s%s", pass ? exe_dir : "", names[i]);
                if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
                    cur = cursor_from_file(path);
            }
        }
    }
    if (!cur)
        cur = make_drawn_cursor();
    return cur;
}

#endif
