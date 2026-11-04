/**
 * icon_swap.c -- redraw the game's button prompts for the device in use (see icon_swap.h).
 *
 * The art is drawn here, nothing of the game's is shipped: GDI shapes and text on a 128 x 128
 * mask, composited in colour, then averaged down to the texture's size (so the edges are smooth
 * at 32 x 32 and at every mip level). Xbox discs and bumpers, PlayStation symbols, D-pad crosses,
 * keycaps and a mouse.
 */
#include "icon_swap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#include <windows.h>
#define COBJMACROS
#include <objbase.h>
#include <wincodec.h>

#include "icon_table.h"
#include "../d3d/d3d8_swizzle.h"        /* d3d8_dxt_decode_block: DXT1/DXT3/DXT5 blocks (tm_dds_native) */

#define MS 128                          /* the master art's size */

static CRITICAL_SECTION s_cs;
static volatile LONG s_cs_ready;
static IconView s_view;                 /* the published view */
static volatile uint32_t s_serial;

static void ic_lock(void)
{
    if (!s_cs_ready) {
        static volatile LONG once;
        if (InterlockedCompareExchange(&once, 1, 0) == 0) {
            InitializeCriticalSection(&s_cs);
            s_cs_ready = 1;
        } else {
            while (!s_cs_ready)
                Sleep(0);
        }
    }
    EnterCriticalSection(&s_cs);
}

static void ic_unlock(void)
{
    LeaveCriticalSection(&s_cs);
}

void icon_swap_publish(const IconView *v)
{
    ic_lock();
    if (memcmp(&s_view, v, sizeof s_view)) {
        s_view = *v;
        s_serial++;
        fprintf(stderr, "  [ICONS] view: %s%s, pad %d %d %d %d (A B X Y) key A '%s' B '%s' Start '%s'\n",
                v->device ? "gamepad" : "keyboard", v->device ? (v->style ? " PlayStation" : " Xbox") : "",
                v->pad[0], v->pad[1], v->pad[2], v->pad[3], v->key[0], v->key[1], v->key[10]);
        fflush(stderr);
    }
    ic_unlock();
}

uint32_t icon_swap_serial(void)
{
    return s_serial;
}

/* ---- matching the game's own icons ------------------------------------------------------ */

static uint64_t ic_fnv(const uint8_t *p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    while (n--)
        h = (h ^ *p++) * 0x100000001B3ull;
    return h;
}

int icon_swap_match(uint32_t w, uint32_t h, uint32_t block, const uint8_t *level0, uint32_t bytes,
                    uint64_t *hash_out)
{
    static uint64_t seen[256];
    static int nseen;
    uint64_t hv;
    size_t i;
    int found = 0, k;

    if (block != 16 || !w || !h || w > 64 || h > 64 || bytes != w * h)
        return 0;
    hv = ic_fnv(level0, bytes);
    if (hash_out)
        *hash_out = hv;
    for (i = 0; i < sizeof k_icon_tab / sizeof k_icon_tab[0]; i++)
        if (k_icon_tab[i].hash == hv && k_icon_tab[i].w == w && k_icon_tab[i].h == h) {
            found = k_icon_tab[i].ic + 1;
            break;
        }
    for (k = 0; k < nseen && seen[k] != hv; k++)
        ;
    if (k == nseen && nseen < 256) {                 /* each small DXT texture once, matched or not */
        seen[nseen++] = hv;
        fprintf(stderr, "  [ICONS] small DXT texture %ux%u hash %016llx -> %s\n", w, h,
                (unsigned long long)hv, found ? k_icon_tab[i].name : "no match");
        fflush(stderr);
    }
    return found;
}

/* ---- the art ---------------------------------------------------------------------------------- */

static HDC s_dc;
static HBITMAP s_bmp;
static uint32_t *s_mask;                          /* the DIB's pixels: white = drawn */
static float s_cv[MS * MS][4];                    /* premultiplied r g b a */
static int s_gdi_ok = -1;

static int gdi_init(void)
{
    BITMAPINFO bi;
    HDC dc;

    if (s_gdi_ok >= 0)
        return s_gdi_ok;
    s_gdi_ok = 0;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = MS;
    bi.bmiHeader.biHeight = -MS;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dc = GetDC(NULL);
    s_dc = CreateCompatibleDC(dc);
    ReleaseDC(NULL, dc);
    if (!s_dc)
        return 0;
    s_bmp = CreateDIBSection(s_dc, &bi, DIB_RGB_COLORS, (void **)&s_mask, NULL, 0);
    if (!s_bmp || !s_mask)
        return 0;
    SelectObject(s_dc, s_bmp);
    SetBkMode(s_dc, TRANSPARENT);
    s_gdi_ok = 1;
    return 1;
}

static void m_clear(void)
{
    GdiFlush();
    memset(s_mask, 0, sizeof(uint32_t) * MS * MS);
    SelectClipRgn(s_dc, NULL);
}

static void m_clip(int l, int t, int r, int b)
{
    HRGN rg = CreateRectRgn(l, t, r, b);
    SelectClipRgn(s_dc, rg);
    DeleteObject(rg);
}

/* white fill, no outline */
static void m_fill_begin(HGDIOBJ *ob, HGDIOBJ *op)
{
    *ob = SelectObject(s_dc, GetStockObject(WHITE_BRUSH));
    *op = SelectObject(s_dc, GetStockObject(NULL_PEN));
}

static void m_fill_end(HGDIOBJ ob, HGDIOBJ op)
{
    SelectObject(s_dc, ob);
    SelectObject(s_dc, op);
}

static void m_ellipse(int l, int t, int r, int b)
{
    HGDIOBJ ob, op;
    m_fill_begin(&ob, &op);
    Ellipse(s_dc, l, t, r + 1, b + 1);
    m_fill_end(ob, op);
}

static void m_rrect(int l, int t, int r, int b, int rad)
{
    HGDIOBJ ob, op;
    m_fill_begin(&ob, &op);
    RoundRect(s_dc, l, t, r + 1, b + 1, rad * 2, rad * 2);
    m_fill_end(ob, op);
}

static void m_poly(const POINT *p, int n)
{
    HGDIOBJ ob, op;
    m_fill_begin(&ob, &op);
    Polygon(s_dc, p, n);
    m_fill_end(ob, op);
}

/* outlines: a white pen of width w, nothing inside */
static HGDIOBJ m_pen_begin(int w, HGDIOBJ *ob)
{
    HPEN pen = CreatePen(PS_SOLID, w, RGB(255, 255, 255));
    HGDIOBJ op = SelectObject(s_dc, pen);
    *ob = SelectObject(s_dc, GetStockObject(NULL_BRUSH));
    return op;
}

static void m_pen_end(HGDIOBJ op, HGDIOBJ ob)
{
    HGDIOBJ pen = SelectObject(s_dc, op);
    SelectObject(s_dc, ob);
    DeleteObject(pen);
}

static void m_ring(int l, int t, int r, int b, int w)
{
    HGDIOBJ ob, op = m_pen_begin(w, &ob);
    Ellipse(s_dc, l, t, r + 1, b + 1);
    m_pen_end(op, ob);
}

static void m_box(int l, int t, int r, int b, int w)
{
    HGDIOBJ ob, op = m_pen_begin(w, &ob);
    Rectangle(s_dc, l, t, r + 1, b + 1);
    m_pen_end(op, ob);
}

static void m_line(int x0, int y0, int x1, int y1, int w)
{
    HGDIOBJ ob, op = m_pen_begin(w, &ob);
    MoveToEx(s_dc, x0, y0, NULL);
    LineTo(s_dc, x1, y1);
    m_pen_end(op, ob);
}

static void m_polyline(const POINT *p, int n, int w)
{
    HGDIOBJ ob, op = m_pen_begin(w, &ob);
    Polygon(s_dc, p, n);
    m_pen_end(op, ob);
}

/* centred text, shrunk until it is at most max_w wide */
static void m_text(const wchar_t *s, int cx, int cy, int h, int max_w)
{
    HFONT old, f;
    SIZE sz;
    int len = (int)wcslen(s);

    for (;;) {
        f = CreateFontW(-h, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Arial");
        old = (HFONT)SelectObject(s_dc, f);
        GetTextExtentPoint32W(s_dc, s, len, &sz);
        if (sz.cx <= max_w || h <= 10)
            break;
        SelectObject(s_dc, old);
        DeleteObject(f);
        h -= 4;
    }
    SetTextColor(s_dc, RGB(255, 255, 255));
    SetTextAlign(s_dc, TA_LEFT | TA_TOP);
    TextOutW(s_dc, cx - sz.cx / 2, cy - sz.cy / 2, s, len);
    SelectObject(s_dc, old);
    DeleteObject(f);
}

/* colour 0xRRGGBB through the mask, over the canvas */
static void paint(uint32_t rgb, float alpha)
{
    float r = (float)((rgb >> 16) & 255) / 255.0f, g = (float)((rgb >> 8) & 255) / 255.0f,
          b = (float)(rgb & 255) / 255.0f;
    int i;
    GdiFlush();
    for (i = 0; i < MS * MS; i++) {
        float m = (float)(s_mask[i] & 255) * (1.0f / 255.0f) * alpha, k = 1.0f - m;
        if (m <= 0.0f)
            continue;
        s_cv[i][0] = r * m + s_cv[i][0] * k;
        s_cv[i][1] = g * m + s_cv[i][1] * k;
        s_cv[i][2] = b * m + s_cv[i][2] * k;
        s_cv[i][3] = m + s_cv[i][3] * k;
    }
}

enum { PB_A, PB_B, PB_X, PB_Y, PB_LB, PB_RB, PB_LT, PB_RT, PB_L3, PB_R3, PB_START, PB_BACK,
       PB_DUP, PB_DDOWN, PB_DLEFT, PB_DRIGHT };

static void disc(uint32_t rim, uint32_t body)
{
    m_clear(); m_ellipse(2, 2, 125, 125); paint(rim, 1.0f);
    m_clear(); m_ellipse(9, 9, 118, 118); paint(body, 1.0f);
}

static void gloss(void)
{
    m_clear(); m_ellipse(24, 13, 104, 62); paint(0xFFFFFF, 0.20f);
}

static void label_on(const wchar_t *s, int h, int max_w, uint32_t col)
{
    m_clear(); m_text(s, 64, 66, h, max_w); paint(col, 1.0f);
}

static void art_xbox(int pb)
{
    static const struct { uint32_t col; const wchar_t *ch; } face[4] = {
        { 0x3E9E34, L"A" }, { 0xC2312A, L"B" }, { 0x2B5CC4, L"X" }, { 0xDDAA1A, L"Y" } };
    POINT p[8];

    switch (pb) {
    case PB_A: case PB_B: case PB_X: case PB_Y:
        disc(0x1B1B1B, face[pb].col);
        gloss();
        label_on(face[pb].ch, 84, 80, 0xFFFFFF);
        break;
    case PB_LB: case PB_RB:
        m_clear(); m_rrect(2, 28, 125, 100, 30); paint(0x14171B, 1.0f);
        m_clear(); m_rrect(8, 34, 119, 94, 25); paint(0x565C66, 1.0f);
        label_on(pb == PB_LB ? L"LB" : L"RB", 56, 92, 0xFFFFFF);
        break;
    case PB_LT: case PB_RT:
        m_clear(); m_rrect(20, 4, 107, 123, 34); paint(0x14171B, 1.0f);
        m_clear(); m_rrect(26, 10, 101, 117, 29); paint(0x565C66, 1.0f);
        label_on(pb == PB_LT ? L"LT" : L"RT", 50, 70, 0xFFFFFF);
        break;
    case PB_L3: case PB_R3:
        disc(0x14171B, 0x3A3F47);
        m_clear(); m_ring(24, 24, 103, 103, 6); paint(0x9AA3B0, 1.0f);
        label_on(pb == PB_L3 ? L"L3" : L"R3", 46, 54, 0xFFFFFF);
        break;
    case PB_START:
        disc(0x14171B, 0x3A3F47);
        m_clear(); m_rrect(34, 38, 94, 47, 4); m_rrect(34, 59, 94, 68, 4); m_rrect(34, 80, 94, 89, 4);
        paint(0xFFFFFF, 1.0f);
        break;
    case PB_BACK:
        disc(0x14171B, 0x3A3F47);
        m_clear(); m_box(32, 46, 76, 86, 8); paint(0xFFFFFF, 1.0f);
        m_clear(); m_box(50, 36, 94, 76, 8); paint(0xFFFFFF, 1.0f);
        (void)p;
        break;
    default:
        break;
    }
}

static void art_ps(int pb)
{
    POINT p[3];

    switch (pb) {
    case PB_A: case PB_B: case PB_X: case PB_Y:
        disc(0x0E1013, 0x2A2E36);
        gloss();
        m_clear();
        if (pb == PB_A) {                             /* cross */
            m_line(42, 42, 86, 86, 12); m_line(86, 42, 42, 86, 12);
            paint(0x7FB2F0, 1.0f);
        } else if (pb == PB_B) {                      /* circle */
            m_ring(37, 37, 90, 90, 12);
            paint(0xF0675F, 1.0f);
        } else if (pb == PB_X) {                      /* square */
            m_box(39, 39, 88, 88, 12);
            paint(0xE98AC9, 1.0f);
        } else {                                      /* triangle */
            p[0].x = 64; p[0].y = 34; p[1].x = 96; p[1].y = 88; p[2].x = 32; p[2].y = 88;
            m_polyline(p, 3, 11);
            paint(0x55D6A3, 1.0f);
        }
        break;
    case PB_LB: case PB_RB:
        m_clear(); m_rrect(2, 28, 125, 100, 30); paint(0x0E1013, 1.0f);
        m_clear(); m_rrect(8, 34, 119, 94, 25); paint(0x4A505A, 1.0f);
        label_on(pb == PB_LB ? L"L1" : L"R1", 56, 92, 0xFFFFFF);
        break;
    case PB_LT: case PB_RT:
        m_clear(); m_rrect(20, 4, 107, 123, 34); paint(0x0E1013, 1.0f);
        m_clear(); m_rrect(26, 10, 101, 117, 29); paint(0x4A505A, 1.0f);
        label_on(pb == PB_LT ? L"L2" : L"R2", 50, 70, 0xFFFFFF);
        break;
    case PB_L3: case PB_R3:
        disc(0x0E1013, 0x2A2E36);
        m_clear(); m_ring(24, 24, 103, 103, 6); paint(0x9AA3B0, 1.0f);
        label_on(pb == PB_L3 ? L"L3" : L"R3", 46, 54, 0xFFFFFF);
        break;
    case PB_START:                                    /* Options */
        m_clear(); m_rrect(8, 24, 119, 103, 32); paint(0x0E1013, 1.0f);
        m_clear(); m_rrect(14, 30, 113, 97, 27); paint(0x3A3F47, 1.0f);
        m_clear(); m_rrect(34, 42, 94, 50, 4); m_rrect(34, 60, 94, 68, 4); m_rrect(34, 78, 94, 86, 4);
        paint(0xFFFFFF, 1.0f);
        break;
    case PB_BACK:                                     /* Share / Create */
        m_clear(); m_rrect(8, 24, 119, 103, 32); paint(0x0E1013, 1.0f);
        m_clear(); m_rrect(14, 30, 113, 97, 27); paint(0x3A3F47, 1.0f);
        label_on(L"SHARE", 24, 84, 0xFFFFFF);
        break;
    default:
        break;
    }
}

static void art_dpad(int pb, int style)
{
    uint32_t rim = style ? 0x0E1013 : 0x14171B, body = style ? 0x3A3F47 : 0x4A505A;
    m_clear(); m_rrect(42, 2, 85, 125, 12); m_rrect(2, 42, 125, 85, 12); paint(rim, 1.0f);
    m_clear(); m_rrect(48, 8, 79, 119, 8); m_rrect(8, 48, 119, 79, 8); paint(body, 1.0f);
    m_clear();
    if (pb == PB_DUP)         m_rrect(48, 8, 79, 52, 8);
    else if (pb == PB_DDOWN)  m_rrect(48, 75, 79, 119, 8);
    else if (pb == PB_DLEFT)  m_rrect(8, 48, 52, 79, 8);
    else                      m_rrect(75, 48, 119, 79, 8);
    paint(0xFFFFFF, 1.0f);
}

static void art_pad(int style, int pb)
{
    if (pb >= PB_DUP)
        art_dpad(pb, style);
    else if (style)
        art_ps(pb);
    else
        art_xbox(pb);
}

/* a mouse button: the body, the pressed part in colour */
static void art_mouse(char which)
{
    m_clear(); m_rrect(30, 2, 97, 125, 32); paint(0x15181C, 1.0f);
    m_clear(); m_rrect(35, 7, 92, 120, 28); paint(0xE8E8E8, 1.0f);
    m_clear();
    if (which == 'L')      { m_clip(0, 0, 63, 62); m_rrect(35, 7, 92, 120, 28); }
    else if (which == 'R') { m_clip(65, 0, MS, 62); m_rrect(35, 7, 92, 120, 28); }
    else if (which == 'M') { m_rrect(57, 14, 70, 48, 6); }
    else                   { m_rrect(35, 40, 49, 70, 6); }
    paint(0xE0463C, 1.0f);
    m_clear(); m_line(63, 8, 63, 62, 3); m_line(36, 62, 91, 62, 3); paint(0x6A6A6A, 1.0f);
    if (which == '4' || which == '5') {
        wchar_t s[2] = { (wchar_t)which, 0 };
        m_clear(); m_text(s, 64, 92, 30, 30); paint(0x333333, 1.0f);
    }
}

static void art_key(const char *key)
{
    wchar_t w[IC_KEYLEN];
    size_t i, n = strlen(key);

    if (key[0] == '\x01') {
        art_mouse(key[1]);
        return;
    }
    if (n >= IC_KEYLEN)
        n = IC_KEYLEN - 1;
    for (i = 0; i < n; i++)
        w[i] = (unsigned char)key[i];
    w[n] = 0;
    if (!strcmp(key, "\x18"))      { w[0] = 0x2191; w[1] = 0; }        /* arrows */
    else if (!strcmp(key, "\x19")) { w[0] = 0x2193; w[1] = 0; }
    else if (!strcmp(key, "\x1b")) { w[0] = 0x2190; w[1] = 0; }
    else if (!strcmp(key, "\x1a")) { w[0] = 0x2192; w[1] = 0; }
    m_clear(); m_rrect(2, 16, 125, 124, 22); paint(0x6C6C6C, 1.0f);       /* the key's side */
    m_clear(); m_rrect(2, 4, 125, 108, 22); paint(0x151515, 1.0f);
    m_clear(); m_rrect(7, 9, 120, 103, 18); paint(0xF1F1F1, 1.0f);
    m_clear(); m_text(w, 64, 56, 66, 108); paint(0x1B1B1B, 1.0f);
}

/* ---- the master per control, cached per view ---------------------------------------------------- */

static uint32_t s_master[IC_COUNT][MS * MS];
static uint32_t s_master_ser[IC_COUNT];
static int s_master_keep[IC_COUNT];      /* 1: the game's own art for this view */
static IconView s_cur;                   /* the view the masters are drawn for */
static uint32_t s_cur_ser;

static void master_for(int ic)
{
    int pb = -1, i;

    memset(s_cv, 0, sizeof s_cv);
    s_master_keep[ic] = 0;
    if (s_cur.device) {
        pb = s_cur.pad[ic];
        if (pb == ic && !s_cur.style) {
            s_master_keep[ic] = 1;                    /* the game's Xbox art is already right */
            return;
        }
        if (pb < 0 || pb >= IC_COUNT) {
            s_master_keep[ic] = 1;
            return;
        }
        art_pad(s_cur.style, pb);
    } else {
        if (!s_cur.key[ic][0]) {                      /* no key does this here: the game's own art stays */
            s_master_keep[ic] = 1;
            return;
        }
        art_key(s_cur.key[ic]);
    }
    for (i = 0; i < MS * MS; i++) {
        float a = s_cv[i][3];
        uint32_t A = (uint32_t)(a * 255.0f + 0.5f), R = 0, G = 0, B = 0;
        if (a > 0.0f) {
            R = (uint32_t)(s_cv[i][0] / a * 255.0f + 0.5f);
            G = (uint32_t)(s_cv[i][1] / a * 255.0f + 0.5f);
            B = (uint32_t)(s_cv[i][2] / a * 255.0f + 0.5f);
            if (R > 255) R = 255;
            if (G > 255) G = 255;
            if (B > 255) B = 255;
        }
        s_master[ic][i] = (A << 24) | (R << 16) | (G << 8) | B;
    }
}

static int fill_cell(int ic, uint32_t *dst, uint32_t stride, uint32_t w, uint32_t h)
{
    uint32_t x, y;
    uint32_t ser;

    if (ic < 0 || ic >= IC_COUNT || !gdi_init())
        return 0;
    ic_lock();
    ser = s_serial;
    if (ser != s_cur_ser) {
        s_cur = s_view;
        s_cur_ser = ser;
    }
    if (s_master_ser[ic] != ser) {
        master_for(ic);
        s_master_ser[ic] = ser;
    }
    if (s_master_keep[ic]) {
        ic_unlock();
        return 0;
    }
    for (y = 0; y < h; y++) {
        float fy0 = (float)y * MS / (float)h, fy1 = (float)(y + 1) * MS / (float)h;
        for (x = 0; x < w; x++) {
            float fx0 = (float)x * MS / (float)w, fx1 = (float)(x + 1) * MS / (float)w;
            float sr = 0, sg = 0, sb = 0, sa = 0, wsum = 0;
            int iy, ix;
            for (iy = (int)fy0; iy < (int)(fy1 + 0.999f) && iy < MS; iy++) {
                float wy = (float)(iy + 1 < fy1 ? iy + 1 : fy1) - (float)(iy > fy0 ? iy : fy0);
                for (ix = (int)fx0; ix < (int)(fx1 + 0.999f) && ix < MS; ix++) {
                    float wx = (float)(ix + 1 < fx1 ? ix + 1 : fx1) - (float)(ix > fx0 ? ix : fx0);
                    uint32_t p = s_master[ic][iy * MS + ix];
                    float a = (float)(p >> 24) * (1.0f / 255.0f), wt = wx * wy;
                    if (wt <= 0.0f)
                        continue;
                    sr += (float)((p >> 16) & 255) * a * wt;
                    sg += (float)((p >> 8) & 255) * a * wt;
                    sb += (float)(p & 255) * a * wt;
                    sa += a * wt;
                    wsum += wt;
                }
            }
            if (wsum <= 0.0f || sa <= 0.0f) {
                dst[y * stride + x] = 0;
            } else {
                uint32_t A = (uint32_t)(sa / wsum * 255.0f + 0.5f);
                uint32_t R = (uint32_t)(sr / sa + 0.5f), G = (uint32_t)(sg / sa + 0.5f),
                         B = (uint32_t)(sb / sa + 0.5f);
                if (R > 255) R = 255;
                if (G > 255) G = 255;
                if (B > 255) B = 255;
                if (A > 255) A = 255;
                dst[y * stride + x] = (A << 24) | (R << 16) | (G << 8) | B;
            }
        }
    }
    ic_unlock();
    return 1;
}

int icon_swap_fill(int ic, uint32_t *dst, uint32_t w, uint32_t h)
{
    return fill_cell(ic, dst, w, w, h);
}

/* The in-mission prompt sheet (512 x 64 at the top level, 32 x 32 cells): the cells of the game's
 * controls are drawn again, the others (stick arrows) stay as they are in `dst`. */
static const signed char k_sheet_cell[2][16] = {
    { 3, 2, 0, 1, 6, 7, 5, 4, 12, 14, 13, 15, 8, 9, -1, -1 },      /* row 0: Y X A B L R White Black D-pad up left down right L3 R3 */
    { 11, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 }   /* row 1: Back Start */
};

int icon_swap_fill_sheet(uint32_t *dst, uint32_t w, uint32_t h)
{
    uint32_t row, col, cw = w / 16, ch = h / 2;
    if (!cw || !ch)
        return 0;
    for (row = 0; row < 2; row++)
        for (col = 0; col < 16; col++) {
            int ic = k_sheet_cell[row][col];
            if (ic >= 0)
                fill_cell(ic, dst + row * ch * w + col * cw, w, cw, ch);
        }
    return 1;
}

/* 0 or the sheet id (100) when this is the game's prompt sheet */
int icon_swap_match_sheet(uint32_t w, uint32_t h, uint32_t fmt, uint64_t hash)
{
    return hash == 0xD5E66A646EF4891Bull || (w == 512 && h == 64 && fmt == 0x0B) ? 100 : 0;
}

/* ---- the texture dump and the texture mods ------------------------------------------------------ */

static uint32_t s_crc_tab[256];

static uint32_t png_crc(uint32_t crc, const uint8_t *p, size_t n)
{
    if (!s_crc_tab[1]) {
        uint32_t i, k, c;
        for (i = 0; i < 256; i++) {
            c = i;
            for (k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            s_crc_tab[i] = c;
        }
    }
    crc = ~crc;
    while (n--)
        crc = s_crc_tab[(crc ^ *p++) & 255] ^ (crc >> 8);
    return ~crc;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n)
{
    uint8_t b[4];
    uint32_t crc;
    put_be32(b, n);
    fwrite(b, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (n)
        fwrite(data, 1, n, f);
    crc = png_crc(0, (const uint8_t *)type, 4);
    if (n) {
        /* crc over type+data: continue from the type's crc */
        uint32_t c = ~crc;
        const uint8_t *p = data;
        size_t m = n;
        while (m--)
            c = s_crc_tab[(c ^ *p++) & 255] ^ (c >> 8);
        crc = ~c;
    }
    put_be32(b, crc);
    fwrite(b, 1, 4, f);
}

/* A straight-alpha 0xAARRGGBB image as an uncompressed (stored-block) RGBA PNG: big, but every tool reads it. */
static int png_write(const char *path, const uint32_t *argb, uint32_t w, uint32_t h)
{
    FILE *f = fopen(path, "wb");
    uint8_t ihdr[13];
    size_t rowb = (size_t)w * 4 + 1, raw = rowb * h, pos = 0, y;
    uint8_t *z, *r;
    uint32_t a = 1, b = 0, i;
    size_t zn, nblk = (raw + 65534) / 65535;

    if (!f)
        return 0;
    r = (uint8_t *)malloc(raw);
    zn = 2 + raw + nblk * 5 + 4;
    z = (uint8_t *)malloc(zn);
    if (!r || !z) {
        free(r); free(z); fclose(f);
        return 0;
    }
    for (y = 0; y < h; y++) {
        uint8_t *row = r + y * rowb;
        row[0] = 0;
        for (i = 0; i < w; i++) {
            uint32_t p = argb[y * w + i];
            row[1 + i * 4] = (uint8_t)(p >> 16);
            row[2 + i * 4] = (uint8_t)(p >> 8);
            row[3 + i * 4] = (uint8_t)p;
            row[4 + i * 4] = (uint8_t)(p >> 24);
        }
    }
    z[pos++] = 0x78; z[pos++] = 0x01;
    for (y = 0; y < raw; y += 65535) {
        size_t n = raw - y < 65535 ? raw - y : 65535;
        z[pos++] = (y + n >= raw) ? 1 : 0;
        z[pos++] = (uint8_t)(n & 255); z[pos++] = (uint8_t)(n >> 8);
        z[pos++] = (uint8_t)(~n & 255); z[pos++] = (uint8_t)((~n >> 8) & 255);
        memcpy(z + pos, r + y, n);
        pos += n;
    }
    for (y = 0; y < raw; y++) {
        a = (a + r[y]) % 65521u;
        b = (b + a) % 65521u;
    }
    put_be32(z + pos, (b << 16) | a);
    pos += 4;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    put_be32(ihdr, w); put_be32(ihdr + 4, h);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, (uint32_t)pos);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(r);
    free(z);
    return 1;
}

/* DDS, uncompressed A8R8G8B8, the top row first: the layout every DDS editor opens. */
static void dds_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static int dds_write(const char *path, const uint32_t *argb, uint32_t w, uint32_t h)
{
    uint8_t hd[128];
    uint32_t y;
    FILE *f;

    memset(hd, 0, sizeof hd);
    memcpy(hd, "DDS ", 4);
    dds_u32(hd + 4, 124);
    dds_u32(hd + 8, 0x100Fu);                       /* caps, height, width, pitch, pixel format */
    dds_u32(hd + 12, h);
    dds_u32(hd + 16, w);
    dds_u32(hd + 20, w * 4);
    dds_u32(hd + 76, 32);
    dds_u32(hd + 80, 0x41u);                        /* RGB + alpha */
    dds_u32(hd + 88, 32);
    dds_u32(hd + 92, 0x00FF0000u);
    dds_u32(hd + 96, 0x0000FF00u);
    dds_u32(hd + 100, 0x000000FFu);
    dds_u32(hd + 104, 0xFF000000u);
    dds_u32(hd + 108, 0x1000u);                     /* DDSCAPS_TEXTURE */
    f = fopen(path, "wb");
    if (!f)
        return 0;
    fwrite(hd, 1, sizeof hd, f);
    for (y = 0; y < h; y++)
        fwrite(argb + (size_t)y * w, 4, w, f);
    fclose(f);
    return 1;
}

/* The dump: every decoded texture the game draws (8..1024 px) is written once as <hash>_<W>x<H>_f<fmt>.png
 * (or .dds with RECOMP_TEX_DUMP_FMT=dds) into the dump folder (RECOMP_TEX_DUMP, Recomp Settings > Dump textures). */
void icon_swap_dump(const uint32_t *argb, uint32_t w, uint32_t h, uint32_t fmt, uint64_t hash)
{
    static int state = -1;                           /* -1 not read, 0 off, 1 on */
    static int as_dds;
    static char dir[MAX_PATH];
    static uint64_t seen[16384];
    static int count;
    uint32_t k;
    char path[MAX_PATH + 64];

    if (state < 0) {
        const char *e = getenv("RECOMP_TEX_DUMP");
        const char *fm = getenv("RECOMP_TEX_DUMP_FMT");
        state = e && *e && *e != '0';
        as_dds = fm && !_stricmp(fm, "dds");
        if (state) {
            snprintf(dir, sizeof dir, "%s", strlen(e) > 1 ? e : "tex_dump");
            CreateDirectoryA(dir, NULL);
            fprintf(stderr, "  [TEXMOD] dumping decoded textures to %s (name = hash_WxH_fmt.png)\n", dir);
            fflush(stderr);
        }
    }
    if (state != 1 || !argb || w < 8 || h < 8 || w > 1024 || h > 1024 || count >= 8000)
        return;
    k = (uint32_t)(hash ^ (hash >> 32) ^ (w * 31u + h)) & 16383u;
    while (seen[k]) {
        if (seen[k] == (hash | 1ull))
            return;
        k = (k + 1) & 16383u;
    }
    seen[k] = hash | 1ull;
    count++;
    snprintf(path, sizeof path, "%s\\%016llx_%ux%u_f%02x.%s", dir, (unsigned long long)hash, w, h, fmt,
             as_dds ? "dds" : "png");
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        /* Dumped by an earlier run: same hash = same pixels, so only touch its time (Explorer can sort by date, newest
         * first); writing ~230 KB per texture again would stall the executor thread. */
        HANDLE fh = CreateFileA(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (fh != INVALID_HANDLE_VALUE) {
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            SetFileTime(fh, NULL, NULL, &now);
            CloseHandle(fh);
        }
        return;
    }
    if (as_dds)
        dds_write(path, argb, w, h);
    else
        png_write(path, argb, w, h);
}

/* ---- texture mods: mods\textures\<hash16>*.png replaces the texture with that hash --------------- */

typedef struct { uint64_t hash; char path[MAX_PATH]; } ModTex;
static ModTex *s_mt;
static int s_mt_n, s_mt_cap, s_mt_init;

static void texmod_scan_dir(const char *dir)
{
    WIN32_FIND_DATAA fd;
    char pat[MAX_PATH + 8];
    HANDLE h;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        const char *n = fd.cFileName, *dot = strrchr(n, '.');
        uint64_t hv = 0;
        int i, ok = 1, found = -1;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !dot)
            continue;
        if (_stricmp(dot, ".png") && _stricmp(dot, ".tga") && _stricmp(dot, ".bmp") && _stricmp(dot, ".jpg")
            && _stricmp(dot, ".dds"))
            continue;
        for (i = 0; i < 16 && ok; i++) {
            char c = n[i];
            if (c >= '0' && c <= '9') hv = hv * 16 + (uint64_t)(c - '0');
            else if (c >= 'a' && c <= 'f') hv = hv * 16 + (uint64_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') hv = hv * 16 + (uint64_t)(c - 'A' + 10);
            else ok = 0;
        }
        if (!ok)
            continue;
        for (i = 0; i < s_mt_n; i++)
            if (s_mt[i].hash == hv)
                found = i;
        if (found < 0) {
            if (s_mt_n == s_mt_cap) {
                ModTex *p = (ModTex *)realloc(s_mt, (size_t)(s_mt_cap ? s_mt_cap * 2 : 256) * sizeof *p);
                if (!p)
                    break;
                s_mt = p;
                s_mt_cap = s_mt_cap ? s_mt_cap * 2 : 256;
            }
            found = s_mt_n++;
        }
        s_mt[found].hash = hv;                       /* a later folder replaces an earlier one */
        snprintf(s_mt[found].path, MAX_PATH, "%s\\%s", dir, n);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void texmod_init(void)
{
    const char *e = getenv("RECOMP_MODS"), *d = getenv("RECOMP_MODS_DIR");
    char root[MAX_PATH], sub[MAX_PATH + 16], pat[MAX_PATH + 8];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    s_mt_init = 1;
    if (e && e[0] == '0')
        return;
    snprintf(root, sizeof root, "%s", d && d[0] ? d : "mods");
    snprintf(sub, sizeof sub, "%s\\textures", root);
    texmod_scan_dir(sub);                            /* mods\textures */
    snprintf(pat, sizeof pat, "%s\\*", root);        /* then every pack: mods\<pack>\textures, by name */
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') {
                snprintf(sub, sizeof sub, "%s\\%s\\textures", root, fd.cFileName);
                texmod_scan_dir(sub);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (s_mt_n) {
        fprintf(stderr, "  [TEXMOD] %d replacement texture(s) found under %s\n", s_mt_n, root);
        fflush(stderr);
    }
}

typedef struct { UINT w, h; INT stride; INT fmt; void *scan0; UINT_PTR reserved; } tm_bitmapdata;
typedef struct { INT x, y, w, h; } tm_rect;
typedef struct { UINT32 version; void *callback; BOOL no_thread; BOOL no_codecs; } tm_startup;

/* DDS read from the file itself, first. Windows' own DDS codec (WIC) reads DXT1/DXT3/DXT5 but refuses uncompressed
 * files and BC4-BC7 (checked on this machine with real files), so: uncompressed 24/32-bit RGB(A) with masks, and
 * DXT1/DXT3/DXT5 (BC1-BC3), with the old or the DX10 header, first level only. Returns malloc'ed 0xAARRGGBB, or NULL
 * for any other format (tm_load_picture then tries WIC). */
static uint32_t dds_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* a channel of `width` bits (1..32) taken to 8 bits */
static uint32_t dds_chan8(uint32_t v, uint32_t width)
{
    uint32_t maxv;
    if (width >= 8)
        return v >> (width - 8);
    maxv = (1u << width) - 1u;
    return (v * 255u + maxv / 2u) / maxv;
}

static uint32_t *tm_dds_native(const char *path, uint32_t *out_w, uint32_t *out_h)
{
    FILE *f;
    uint8_t *file = NULL;
    uint32_t *px = NULL;
    long size;
    uint32_t w, h, pf_flags, fourcc, bits = 0, bpp, off, fmt = 0, bb, bw, bh, bx, by, x, y, i, k;
    uint32_t m[4] = { 0, 0, 0, 0 }, shift[4] = { 0, 0, 0, 0 }, width[4] = { 0, 0, 0, 0 }, c[4];

    f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 128 || size > 64L * 1024 * 1024
        || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    file = (uint8_t *)malloc((size_t)size);
    if (!file || fread(file, 1, (size_t)size, f) != (size_t)size) {
        free(file);
        fclose(f);
        return NULL;
    }
    fclose(f);

    if (memcmp(file, "DDS ", 4) != 0 || dds_get32(file + 4) != 124 || dds_get32(file + 76) != 32)
        goto fail;
    h = dds_get32(file + 12);
    w = dds_get32(file + 16);
    if (w < 1 || h < 1 || w > 4096 || h > 4096)
        goto fail;
    pf_flags = dds_get32(file + 80);
    fourcc = dds_get32(file + 84);
    bits = dds_get32(file + 88);
    m[0] = dds_get32(file + 92);
    m[1] = dds_get32(file + 96);
    m[2] = dds_get32(file + 100);
    m[3] = dds_get32(file + 104);
    off = 128;
    if (pf_flags & 0x4u) {                                          /* a FourCC */
        if (fourcc == 0x30315844u) {                                /* 'DX10': the format is in the next 20 bytes */
            uint32_t dxgi;
            if (size < 148)
                goto fail;
            dxgi = dds_get32(file + 128);
            off = 148;
            switch (dxgi) {
            case 71: case 72: fmt = 0x0C; break;                    /* BC1 */
            case 74: case 75: fmt = 0x0E; break;                    /* BC2 */
            case 77: case 78: fmt = 0x0F; break;                    /* BC3 */
            case 87: case 88: case 91: case 93:                     /* B8G8R8A8 / B8G8R8X8 */
                bits = 32;
                m[0] = 0x00FF0000u; m[1] = 0x0000FF00u; m[2] = 0x000000FFu;
                m[3] = (dxgi == 88 || dxgi == 93) ? 0u : 0xFF000000u;
                break;
            case 28: case 29:                                       /* R8G8B8A8 */
                bits = 32;
                m[0] = 0x000000FFu; m[1] = 0x0000FF00u; m[2] = 0x00FF0000u; m[3] = 0xFF000000u;
                break;
            default:
                goto fail;                                          /* BC4-BC7 and the rest */
            }
        } else if (fourcc == 0x31545844u) {                         /* 'DXT1' */
            fmt = 0x0C;
        } else if (fourcc == 0x32545844u || fourcc == 0x33545844u) {    /* 'DXT2', 'DXT3' */
            fmt = 0x0E;
        } else if (fourcc == 0x34545844u || fourcc == 0x35545844u) {    /* 'DXT4', 'DXT5' */
            fmt = 0x0F;
        } else {
            goto fail;                                              /* 'ATI1' / 'ATI2' (BC4 / BC5) and the rest */
        }
    } else if (!(pf_flags & 0x40u)) {
        goto fail;                                                  /* no FourCC and not RGB */
    }

    if (fmt) {
        bb = fmt == 0x0C ? 8u : 16u;
        bw = (w + 3u) / 4u;
        bh = (h + 3u) / 4u;
        if ((uint64_t)off + (uint64_t)bw * bh * bb > (uint64_t)size)
            goto fail;
        px = (uint32_t *)malloc((size_t)w * h * 4u);
        if (!px)
            goto fail;
        for (by = 0; by < bh; by++)
            for (bx = 0; bx < bw; bx++) {
                uint32_t blk[16];
                d3d8_dxt_decode_block(file + off + ((size_t)by * bw + bx) * bb, fmt, blk);
                for (i = 0; i < 16; i++) {
                    x = bx * 4u + (i & 3u);
                    y = by * 4u + (i >> 2);
                    if (x < w && y < h)
                        px[(size_t)y * w + x] = blk[i];
                }
            }
    } else {
        if (bits != 32 && bits != 24)
            goto fail;
        bpp = bits / 8u;
        if ((uint64_t)off + (uint64_t)w * h * bpp > (uint64_t)size)
            goto fail;
        for (k = 0; k < 4; k++) {
            if (!m[k])
                continue;
            while (!((m[k] >> shift[k]) & 1u))
                shift[k]++;
            for (i = m[k]; i; i &= i - 1u)
                width[k]++;
        }
        px = (uint32_t *)malloc((size_t)w * h * 4u);
        if (!px)
            goto fail;
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                const uint8_t *q = file + off + ((size_t)y * w + x) * bpp;
                uint32_t v = 0;
                for (k = 0; k < bpp; k++)
                    v |= (uint32_t)q[k] << (8 * k);
                for (k = 0; k < 4; k++)                             /* masks in R, G, B, A order */
                    c[k] = m[k] ? dds_chan8((v & m[k]) >> shift[k], width[k]) : (k == 3 ? 255u : 0u);
                px[(size_t)y * w + x] = (c[3] << 24) | (c[0] << 16) | (c[1] << 8) | c[2];
            }
    }
    free(file);
    *out_w = w;
    *out_h = h;
    return px;

fail:
    free(file);
    free(px);
    return NULL;
}

/* DDS through WIC: the fallback after tm_dds_native (BC4-BC7 on Windows builds that read them). The first picture,
 * straight-alpha BGRA. The GUIDs are the public ones; a wrong one only makes the picture fail to load. */
static __declspec(thread) int tm_com;

static uint32_t *tm_load_dds(const char *path, uint32_t *out_w, uint32_t *out_h)
{
    typedef HRESULT (WINAPI *init_fn)(LPVOID, DWORD);
    typedef HRESULT (WINAPI *create_fn)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
    static HMODULE ole;
    static init_fn co_init;
    static create_fn co_create;
    static const GUID clsid_wic = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
    static const GUID iid_wic = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
    static const GUID fmt_bgra = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f } };
    WCHAR wide[MAX_PATH], full[MAX_PATH];
    IWICImagingFactory *fac = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    UINT w = 0, h = 0;
    uint32_t *px = NULL;

    if (!ole) {
        ole = LoadLibraryA("ole32.dll");
        if (!ole)
            return NULL;
        co_init = (init_fn)GetProcAddress(ole, "CoInitializeEx");
        co_create = (create_fn)GetProcAddress(ole, "CoCreateInstance");
    }
    if (!co_init || !co_create)
        return NULL;
    if (!tm_com) {
        co_init(NULL, 0);                            /* COINIT_MULTITHREADED */
        tm_com = 1;
    }
    if (!MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH)
        || !GetFullPathNameW(wide, MAX_PATH, full, NULL))
        return NULL;
    if (FAILED(co_create(&clsid_wic, NULL, CLSCTX_INPROC_SERVER, &iid_wic, (void **)&fac)) || !fac)
        return NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(fac, full, NULL, GENERIC_READ,
                                                               WICDecodeMetadataCacheOnDemand, &dec))
        && SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &frame))
        && SUCCEEDED(IWICImagingFactory_CreateFormatConverter(fac, &conv))
        && SUCCEEDED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame, &fmt_bgra,
                                                    WICBitmapDitherTypeNone, NULL, 0.0,
                                                    WICBitmapPaletteTypeCustom))
        && SUCCEEDED(IWICFormatConverter_GetSize(conv, &w, &h))
        && w > 0 && h > 0 && w <= 8192 && h <= 8192) {
        px = (uint32_t *)malloc((size_t)w * h * 4);
        if (px && FAILED(IWICFormatConverter_CopyPixels(conv, NULL, w * 4, w * h * 4, (BYTE *)px))) {
            free(px);
            px = NULL;
        }
    }
    if (conv)
        IUnknown_Release(conv);
    if (frame)
        IUnknown_Release(frame);
    if (dec)
        IUnknown_Release(dec);
    IUnknown_Release(fac);
    if (px) {
        *out_w = w;
        *out_h = h;
    }
    return px;
}

/* straight-alpha 0xAARRGGBB, malloc'ed, or NULL; any format GDI+ reads, DDS through WIC */
static uint32_t *tm_load_picture(const char *path, uint32_t *out_w, uint32_t *out_h)
{
    typedef int (WINAPI *startup_fn)(ULONG_PTR *, const tm_startup *, void *);
    typedef int (WINAPI *fromfile_fn)(const WCHAR *, void **);
    typedef int (WINAPI *dim_fn)(void *, UINT *);
    typedef int (WINAPI *lock_fn)(void *, const tm_rect *, UINT, INT, tm_bitmapdata *);
    typedef int (WINAPI *unlock_fn)(void *, tm_bitmapdata *);
    typedef int (WINAPI *dispose_fn)(void *);
    static ULONG_PTR token;
    static HMODULE gp;
    static startup_fn startup;
    static fromfile_fn fromfile;
    static dim_fn get_w, get_h;
    static lock_fn lock;
    static unlock_fn unlock;
    static dispose_fn dispose;
    WCHAR wide[MAX_PATH], full[MAX_PATH];
    tm_startup su;
    void *bmp = NULL;
    UINT w = 0, h = 0, y;
    tm_rect rc;
    tm_bitmapdata bd;
    uint32_t *px = NULL;
    const char *dot = strrchr(path, '.');

    if (dot && !_stricmp(dot, ".dds")) {
        uint32_t *dx = tm_dds_native(path, out_w, out_h);
        return dx ? dx : tm_load_dds(path, out_w, out_h);
    }
    if (!gp) {
        gp = LoadLibraryA("gdiplus.dll");
        if (!gp)
            return NULL;
        startup  = (startup_fn)GetProcAddress(gp, "GdiplusStartup");
        fromfile = (fromfile_fn)GetProcAddress(gp, "GdipCreateBitmapFromFile");
        get_w    = (dim_fn)GetProcAddress(gp, "GdipGetImageWidth");
        get_h    = (dim_fn)GetProcAddress(gp, "GdipGetImageHeight");
        lock     = (lock_fn)GetProcAddress(gp, "GdipBitmapLockBits");
        unlock   = (unlock_fn)GetProcAddress(gp, "GdipBitmapUnlockBits");
        dispose  = (dispose_fn)GetProcAddress(gp, "GdipDisposeImage");
    }
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
    if (w > 0 && h > 0 && w <= 8192 && h <= 8192) {
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
    *out_w = w;
    *out_h = h;
    return px;
}

uint32_t *texmod_load(uint64_t hash, uint32_t *w, uint32_t *h)
{
    int i;
    uint32_t *px;
    if (!s_mt_init)
        texmod_init();
    for (i = s_mt_n - 1; i >= 0; i--)
        if (s_mt[i].hash == hash) {
            static int logged;
            px = tm_load_picture(s_mt[i].path, w, h);
            if (logged < 80) {
                logged++;
                fprintf(stderr, "  [TEXMOD] %016llx <- %s (%s)\n", (unsigned long long)hash, s_mt[i].path,
                        px ? "ok" : "could not be read");
                fflush(stderr);
            }
            return px;
        }
    return NULL;
}

/* One mip down: 2 x 2 average, alpha-weighted. dst is max(sw/2,1) x max(sh/2,1). */
void texmod_downsample(const uint32_t *src, uint32_t sw, uint32_t sh, uint32_t *dst)
{
    uint32_t dw = sw > 1 ? sw / 2 : 1, dh = sh > 1 ? sh / 2 : 1, x, y;
    for (y = 0; y < dh; y++)
        for (x = 0; x < dw; x++) {
            float a = 0, r = 0, g = 0, b = 0;
            int i, j;
            for (j = 0; j < 2; j++)
                for (i = 0; i < 2; i++) {
                    uint32_t sx = x * 2 + i < sw ? x * 2 + i : sw - 1, sy = y * 2 + j < sh ? y * 2 + j : sh - 1;
                    uint32_t p = src[sy * sw + sx];
                    float pa = (float)(p >> 24);
                    a += pa;
                    r += (float)((p >> 16) & 255) * pa;
                    g += (float)((p >> 8) & 255) * pa;
                    b += (float)(p & 255) * pa;
                }
            if (a <= 0.0f) {
                dst[y * dw + x] = 0;
            } else {
                uint32_t A = (uint32_t)(a / 4.0f + 0.5f), R = (uint32_t)(r / a + 0.5f),
                         G = (uint32_t)(g / a + 0.5f), B = (uint32_t)(b / a + 0.5f);
                dst[y * dw + x] = (A << 24) | (R << 16) | (G << 8) | B;
            }
        }
}

#else /* !_WIN32: nothing to redraw */

void icon_swap_publish(const IconView *v) { (void)v; }
uint32_t icon_swap_serial(void) { return 0; }
int icon_swap_match(uint32_t w, uint32_t h, uint32_t b, const uint8_t *l, uint32_t n, uint64_t *x)
{ (void)w; (void)h; (void)b; (void)l; (void)n; (void)x; return 0; }
int icon_swap_fill(int ic, uint32_t *d, uint32_t w, uint32_t h)
{ (void)ic; (void)d; (void)w; (void)h; return 0; }
void icon_swap_dump(const uint32_t *a, uint32_t w, uint32_t h, uint32_t f, uint64_t x)
{ (void)a; (void)w; (void)h; (void)f; (void)x; }
int icon_swap_fill_sheet(uint32_t *d, uint32_t w, uint32_t h) { (void)d; (void)w; (void)h; return 0; }
int icon_swap_match_sheet(uint32_t w, uint32_t h, uint32_t f, uint64_t x) { (void)w; (void)h; (void)f; (void)x; return 0; }
uint32_t *texmod_load(uint64_t hash, uint32_t *w, uint32_t *h) { (void)hash; (void)w; (void)h; return 0; }
void texmod_downsample(const uint32_t *s, uint32_t sw, uint32_t sh, uint32_t *d) { (void)s; (void)sw; (void)sh; (void)d; }

#endif
