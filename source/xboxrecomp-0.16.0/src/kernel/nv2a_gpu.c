/*
 * NV2A pixel work on the host GPU (Direct3D 11). See nv2a_gpu.h.
 */
#include "nv2a_gpu.h"

#if defined(_WIN32)
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../d3d/d3d8_swizzle.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* ---- device ---------------------------------------------------------- */

static int s_on = -1;                  /* -1: not decided yet */
/* RECOMP_GPU_SCALE (1-4): every surface is drawn at this many times the
 * guest's resolution; RECOMP_GPU_RES=WxH instead sizes the 640x480 frame
 * to WxH, each axis on its own (2560x1440 for the title's widescreen
 * mode, which draws 16:9 squeezed into 640x480). The guest copy stays at
 * its own size: what goes up (surf_upload) is enlarged and what comes back
 * (surf_readback) is averaged down, and the viewport, scissor, clears and
 * occlusion counts are scaled to match. The shaders work in the guest's
 * pixel units throughout (vsc is 1/the guest size), so nothing else
 * changes. */
static float s_sx = 1.0f, s_sy = 1.0f;
static int s_scaled;
#define SC_W(v) ((UINT)((float)(v) * s_sx + 0.5f))
#define SC_H(v) ((UINT)((float)(v) * s_sy + 0.5f))
#define SC_X(v) ((LONG)((float)(v) * s_sx + 0.5f))
#define SC_Y(v) ((LONG)((float)(v) * s_sy + 0.5f))
static int scale_blit(ID3D11ShaderResourceView *src, ID3D11RenderTargetView *dst,
                      UINT w, UINT h, int linear);
static void scale_init(void);
static ID3D11Device *s_dev;
static ID3D11DeviceContext *s_ctx;
static ID3D11DeviceContext1 *s_ctx1;   /* ClearView, when available */
static ID3D11VertexShader *s_vs;
static ID3D11PixelShader *s_ps;
static ID3D11InputLayout *s_il;
static ID3D11Buffer *s_vb, *s_cb_vs, *s_cb_ps;
static ID3D11RasterizerState *s_rs;
static UINT s_vb_bytes, s_vb_pos;
static ID3D11Buffer *s_ib;              /* indices, a ring like s_vb */
static UINT s_ib_pos;
#define GPU_IB_BYTES (4u << 20)
static uint32_t s_frame = 1;

/* RECOMP_GPU_STATS: every 60 frames, where the executor's GPU time went. */
static struct {
    uint32_t draws, readbacks, uploads, textures, rtt, rtt_copies;
    uint32_t up_new, up_stale, up_hash;
    double map_ms, gpu_ms;
    uint32_t gpu_frames;
    double readback_ms, upload_ms;
} s_st;
static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

#define GPU_VB_BYTES (8u << 20)

/* ---- WEAPON LIGHTING (nv2a_gpu.h) ---------------------------------------------------------------------------------------------
 * The first-person weapon's opaque draws run their normal combiner shader and then wpn_post() below, which adds
 *   - a view-space normal from the screen derivatives of the position rebuilt from SV_Position (w = 1/view depth) and a bump
 *     from the screen derivatives of a height made of the picture's own luminance (the game has no normal maps), plus the
 *     height of the rain drops;
 *   - environment light: a 256x144 mip copy of the last presented frame (t4): a colour cast from its average, and a reflection
 *     looked up by the reflected view ray as a screen position (rays pointing behind the camera fall back to the average);
 *   - dust, blood and rain drops, made in the shader (value noise and a drop grid in the texture's own coordinates), their
 *     amounts coming from the game side (explosions / hits near the player, rain) in the WPN constants (b5).
 * Cached shader variants carry WPN_VERSION in their key: bump it whenever k_wpn_hlsl changes. */
#define WPN_VERSION 10u
static const char k_wpn_hlsl[] =
"cbuffer WPN : register(b5) {\n"
"  float4 wp0;     /* x strength, y tan(half fov) across, z tan(half fov) down, w seconds */\n"
"  float4 wp1;     /* x dust, y unused, z wet (rain), w environment copy valid */\n"
"  float4 wp2;     /* rgb blood colour, w pattern seed */\n"
"  float4 wp3;     /* x render target width, y height, zw where the gun sits on the screen now (view units: the patterns follow it) */\n"
"  float4 bl[8];   /* blood events: x, y centre, z size, w age in seconds (negative: unused) */\n"
"  float4 bs[8];   /* blood events: x seed, y strength */\n"
"  float4 wl0;     /* xyz direction to the light (view space, x right, y up, z forward), w how directional it is, 0..1 */\n"
"  float4 wl1;     /* rgb colour of the light */\n"
"};\n"
"Texture2D txe : register(t4);\n"
"SamplerState sme : register(s4);\n"
"float wh21(float2 p) { float3 q = frac(float3(p.x, p.y, p.x) * 0.1031); q += dot(q, q.yzx + 33.33); return frac((q.x + q.y) * q.z); }\n"
"float wvn(float2 p) { float2 i = floor(p), f = frac(p); f = f * f * (3.0 - 2.0 * f);\n"
"  return lerp(lerp(wh21(i), wh21(i + float2(1, 0)), f.x), lerp(wh21(i + float2(0, 1)), wh21(i + float2(1, 1)), f.x), f.y); }\n"
"float wfbm(float2 p) { return 0.5 * wvn(p) + 0.25 * wvn(p * 2.03 + 7.1) + 0.125 * wvn(p * 4.11 + 3.7); }\n"
"float wseg(float2 p, float2 a, float2 b) { float2 pa = p - a, ba = b - a; float h = saturate(dot(pa, ba) / max(dot(ba, ba), 1e-6)); return length(pa - ba * h); }\n"
"/* water on the weapon: coverage, dark rim, and the surface tilt of the drops (view space x right, y up) */\n"
"struct WAcc { float cov; float ring; float2 tilt; };\n"
"void wdrop(float2 p, float2 c, float r, inout WAcc a) {\n"
"  float2 d = p - c; float k = length(d) / max(r, 1e-5);\n"
"  if (k < 1.0) {\n"
"    float m = smoothstep(1.0, 0.8, k);\n"
"    a.cov = max(a.cov, m); a.ring = max(a.ring, m * smoothstep(0.45, 1.0, k)); a.tilt += d / max(r, 1e-5) * 1.1 * m;\n"
"  }\n"
"}\n"
"/* beads: one per 16 mm cell, each with its own cycle (they swell, hold, creep down a little, shrink); wetter = more cells have one */\n"
"void wbeads(float2 p, float t, float wet, inout WAcc a) {\n"
"  float cs = 0.016; float2 id = floor(p / cs);\n"
"  [unroll] for (int j = -1; j <= 1; j++) {\n"
"    [unroll] for (int i = -1; i <= 1; i++) {\n"
"      float2 cell = id + float2(i, j);\n"
"      float3 n = float3(wh21(cell), wh21(cell + 7.7), wh21(cell + 19.3));\n"
"      if (n.x > wet * 0.8) continue;\n"
"      float ph = frac(t / (18.0 + 22.0 * n.y) + n.z);\n"
"      float life = smoothstep(0.0, 0.15, ph) * (1.0 - smoothstep(0.8, 1.0, ph));\n"
"      float rad = (0.0013 + 0.0022 * n.y * n.y) * life;\n"
"      float2 c = (cell + 0.5 + (float2(wh21(cell + 3.1), wh21(cell + 11.9)) - 0.5) * 0.6) * cs;\n"
"      c.y -= smoothstep(0.55, 0.8, ph) * n.y * 0.5 * cs * step(0.5, n.z);\n"
"      wdrop(p, c, rad, a);\n"
"    }\n"
"  }\n"
"}\n"
"/* runners: in 20 mm columns, three slots each: a drop that swells, then slides down in jerks (stick and slip), leaving a thin wet trail */\n"
"void wrunners(float2 p, float t, float wet, inout WAcc a) {\n"
"  float cw = 0.020; float cx = floor(p.x / cw);\n"
"  [unroll] for (int i = -1; i <= 1; i++) {\n"
"    float col = cx + i;\n"
"    [unroll] for (int j = 0; j < 3; j++) {\n"
"      float jj = j * 3.7;\n"
"      float4 n = float4(wh21(float2(col, jj)), wh21(float2(col + 5.1, jj + 1.3)), wh21(float2(col + 9.7, jj + 2.9)), wh21(float2(col + 13.3, jj + 4.1)));\n"
"      if (n.x > wet * 0.55) continue;\n"
"      float u = frac(t / (7.0 + 9.0 * n.y) + n.z);\n"
"      float y0 = 0.05 - 0.35 * n.w;\n"
"      float L = 0.06 + 0.14 * n.y;\n"
"      float sp = u * 4.0;\n"
"      float s = L * (floor(sp) + smoothstep(0.55, 1.0, frac(sp))) * 0.25;\n"
"      float yc = y0 - s;\n"
"      float xc = (col + 0.5 + (n.w - 0.5) * 0.5 + 0.1 * sin(s * 70.0 + n.z * 6.28)) * cw;\n"
"      float fade = 1.0 - smoothstep(0.9, 1.0, u);\n"
"      float r = (0.0026 + 0.0014 * n.z) * smoothstep(0.0, 0.08, u) * fade;\n"
"      wdrop(p, float2(xc, yc), r, a);\n"
"      if (p.y > yc && p.y < y0 && s > 0.004) {\n"
"        float along = (p.y - yc) / s;\n"
"        float w = 0.0009 * (1.0 - 0.6 * along);\n"
"        float m = smoothstep(w, w * 0.3, abs(p.x - xc)) * (1.0 - along * 0.7) * 0.55 * fade;\n"
"        a.cov = max(a.cov, m * 0.5);\n"
"        a.tilt.x += (p.x - xc) / max(w, 1e-5) * 0.35 * m;\n"
"      }\n"
"    }\n"
"  }\n"
"}\n"
"/* one blood event (a hit): an irregular blob and a spray of spots round it, thinning outward; the bigger spots creep down as it ages\n"
"   and the whole thing goes from bright red and wet to dark and dull, then fades. bw / bf: weights for the average freshness */\n"
"void wblood(float2 p, float2 c, float4 e, float2 sd, float2 gdir, inout float bcov, inout float bw, inout float bf, inout float2 bt) {\n"
"  if (e.w < 0.0) return;\n"
"  float2 r = (p - c) / max(e.z, 1e-4);\n"
"  float rr = length(r);\n"
"  if (rr > 1.3) return;\n"
"  float age = e.w;\n"
"  float fresh = 1.0 - smoothstep(4.0, 70.0, age);\n"
"  float life = 1.0 - smoothstep(110.0, 190.0, age);\n"
"  float nz = wfbm(r * 3.5 + sd.x * 13.0);\n"
"  float R = 0.08 + 0.10 * sd.y;\n"
"  float cov = smoothstep(R, R * 0.8, rr * (0.75 + nz * 0.7));\n"
"  float2 tl = 0;\n"
"  float2 g = r * 7.0 + sd.x * 37.0;\n"
"  float2 id = floor(g);\n"
"  [unroll] for (int j = -1; j <= 1; j++) {\n"
"    [unroll] for (int i = -1; i <= 1; i++) {\n"
"      float2 cell = id + float2(i, j);\n"
"      float3 h = float3(wh21(cell), wh21(cell + 4.3), wh21(cell + 8.9));\n"
"      float rc = length((cell + 0.5 - sd.x * 37.0) / 7.0);\n"
"      if (h.x > pow(saturate(1.0 - rc / 1.15), 1.5) * (0.2 + 0.4 * sd.y)) continue;\n"
"      float rad = (0.10 + 0.28 * h.z) * (1.1 - 0.6 * saturate(rc / 1.15));\n"
"      float2 c0 = cell + 0.2 + 0.6 * float2(h.y, h.z);\n"
"      float slide = rad > 0.2 && h.y > 0.35 ? min(age * 0.02 * h.y, 1.6) : 0.0;\n"
"      float2 c1 = c0 + gdir * slide;\n"
"      float dd = wseg(g, c0, c1) / rad;\n"
"      float m = smoothstep(1.0, 0.65, dd);\n"
"      cov = max(cov, m);\n"
"      tl += (g - c1) / rad * 0.9 * m;\n"
"    }\n"
"  }\n"
"  cov *= life * saturate(sd.y * 1.5 + 0.2);\n"
"  bcov = max(bcov, cov);\n"
"  bw += cov;\n"
"  bf += fresh * cov;\n"
"  bt += tl * fresh * life;\n"
"}\n"
"float4 wpn_post(float4 col, float4 sp, float2 uv) {\n"
"  float str = wp0.x, dust = wp1.x, wet = wp1.z, seed = wp2.w;\n"
"  float3 P = float3((sp.x / wp3.x * 2.0 - 1.0) * wp0.y, (1.0 - sp.y / wp3.y * 2.0) * wp0.z, 1.0) / max(sp.w, 1e-6);\n"
"  float3 dx = ddx(P), dy = ddy(P);\n"
"  float3 N = cross(dx, dy);\n"
"  float nl = length(N);\n"
"  N = nl > 1e-12 ? N / nl : float3(0, 0, -1);\n"
"  if (dot(N, P) > 0.0) N = -N;\n"
"  float lum = dot(col.rgb, float3(0.299, 0.587, 0.114));\n"
"  float mxc = max(col.r, max(col.g, col.b)), mnc = min(col.r, min(col.g, col.b));\n"
"  float sat = (mxc - mnc) / (mxc + 0.02);\n"
"  /* the patterns live on the SURFACE: in the texture's own coordinates, so they move with the gun and the hands whatever they do.\n"
"     Gravity is found from the screen: going down the screen changes uv by ddy(uv). */\n"
"  float2 gx = ddx(uv), gd = ddy(uv);\n"
"  float gl = length(gd);\n"
"  float2 gdir = gl > 1e-9 ? gd / gl : float2(0, 1);\n"
"  bool alongU = abs(gd.x) > abs(gd.y);\n"
"  float gsg = (alongU ? gd.x : gd.y) >= 0.0 ? 1.0 : -1.0;\n"
"  float2 pb = uv * 0.4;\n"
"  float2 pr = (alongU ? float2(uv.y, -gsg * uv.x) : float2(uv.x, -gsg * uv.y)) * 0.4;\n"
"  WAcc a; a.cov = 0; a.ring = 0; a.tilt = float2(0, 0);\n"
"  WAcc ar; ar.cov = 0; ar.ring = 0; ar.tilt = float2(0, 0);\n"
"  if (wet > 0.01) { wbeads(pb, wp0.w, wet, a); wrunners(pr, wp0.w, wet, ar); }\n"
"  float2 tr = alongU ? float2(-gsg * ar.tilt.y, ar.tilt.x) : float2(ar.tilt.x, -gsg * ar.tilt.y);   /* the runners' tilt back in uv axes */\n"
"  a.cov = max(a.cov, ar.cov); a.ring = max(a.ring, ar.ring); a.tilt += tr;\n"
"  float bcov = 0, bw = 0, bf = 0; float2 bt = float2(0, 0);\n"
"  [loop] for (int k = 0; k < 8; k++) {\n"
"    float2 c2 = frac(bl[k].xy * float2(1.7, 2.3) + bs[k].x * 0.31 + 0.2) * 0.7 + 0.15;   /* a second place on the surface, for the parts that are hidden */\n"
"    wblood(uv, bl[k].xy, bl[k], bs[k].xy, gdir, bcov, bw, bf, bt);\n"
"    wblood(uv, c2, bl[k], bs[k].xy * float2(1.0, 0.8), gdir, bcov, bw, bf, bt);\n"
"  }\n"
"  float bfresh = bw > 0.0 ? bf / bw : 0.0;\n"
"  float cw = saturate(a.cov);\n"
"  float2 tuv = a.tilt + bt * 0.8;                           /* the tilt of drops and blood, in uv axes -> view x right, y up through the screen's own derivatives */\n"
"  float jdet = gx.x * gd.y - gx.y * gd.x;\n"
"  float jinv = abs(jdet) > 1e-14 ? 1.0 / jdet : 0.0;\n"
"  float2 tpx = float2(gd.y * tuv.x - gd.x * tuv.y, -gx.y * tuv.x + gx.x * tuv.y) * jinv;\n"
"  float2 tvw = float2(tpx.x, -tpx.y);\n"
"  float tvl = length(tvw);\n"
"  tvw = tvl > 1e-9 ? tvw / tvl * length(tuv) : float2(0, 0);\n"
"  float dmask = saturate(saturate((wfbm(uv * 7.0 + seed) - 0.42) * 3.5) * dust);\n"
"  /* bump from the picture's own brightness (no tangents needed) */\n"
"  float H = lum * 0.8;\n"
"  float3 r1 = cross(dy, N), r2 = cross(N, dx);\n"
"  float det = dot(dx, r1);\n"
"  float3 sg = sign(det) * (ddx(H) * r1 + ddy(H) * r2);\n"
"  float plen = length(dx) + length(dy) + 1e-9;\n"
"  N = normalize(abs(det) * N - (2.0 * plen) * sg);\n"
"  /* the triangle normal shows as hard facets on the low-poly hands, so it is only a small part of any normal used here. Ns (water film,\n"
"     drops) leans almost fully to the view. Nd (the light) is a pseudo normal from where the pixel is on the weapon: an ellipse round\n"
"     the gun and hands in view angles, so the right part faces right, the top part up (smooth, no facets), plus a little real normal. */\n"
"  float3 Vv = normalize(-P);\n"
"  float3 Ns = normalize(lerp(N, Vv, 0.9));\n"
"  /* where the gun and hands are on a 16:9 view without aiming (measured on screenshots): centre (0.2, -0.6), half size (0.3, 0.45) in view\n"
"     units scaled to that picture; aiming (wp1.y) moves them to the middle. */\n"
"  float2 vxy = float2((sp.x / wp3.x * 2.0 - 1.0) * wp0.y / 0.93, (1.0 - sp.y / wp3.y * 2.0) * wp0.z / 0.525);\n"
"  float2 gcen = lerp(float2(0.2, -0.6), float2(0.02, -0.62), wp1.y), gext = lerp(float2(0.3, 0.45), float2(0.14, 0.38), wp1.y);\n"
"  float3 Np = normalize(float3((vxy - gcen) / gext, -1.0));\n"
"  float3 Nr = normalize(lerp(N, Vv, 0.5));\n"
"  float3 Nd = normalize(Np + 0.08 * Nr);\n"
"  N = normalize(Ns + float3(tvw * 0.9, 0.0));\n"
"  float3 V = Vv;\n"
"  float3 R = reflect(-V, N);\n"
"  float ndv = saturate(dot(N, V));\n"
"  float fres = 0.04 + 0.96 * pow(1.0 - ndv, 5.0);\n"
"  float3 envA = wp1.w > 0.5 ? txe.SampleLevel(sme, float2(0.5, 0.5), 7.0).rgb : float3(0.4, 0.4, 0.4);\n"
"  float2 su = float2(R.x / max(R.z, 0.25) / wp0.y, -R.y / max(R.z, 0.25) / wp0.z) * 0.5 + 0.5;\n"
"  float3 envS = wp1.w > 0.5 ? txe.SampleLevel(sme, saturate(su), 2.5).rgb : envA;\n"
"  float3 env = lerp(envA, envS, smoothstep(0.05, 0.5, R.z));\n"
"  float el = dot(envA, float3(0.299, 0.587, 0.114)) + 0.03;\n"
"  float metal = saturate(1.3 - sat * 2.2);\n"
"  /* the light: where the picture is brightest (found on the CPU). The side of the gun / hand that faces it is brighter, the other side\n"
"     darker, metal gets a highlight and the edges facing the light a rim. Nothing is added to a surface that is not wet or bloody\n"
"     unless a light faces it. */\n"
"  float lint = wl0.w * str;\n"
"  float3 Lw = wl0.xyz, lcol = wl1.rgb;\n"
"  float ndl = dot(Nd, Lw);\n"
"  float wrapl = saturate(ndl * 0.6 + 0.4);\n"
"  float3 Hh = normalize(Lw + V);\n"
"  /* a broad highlight from the pseudo normal (fixed on the screen, so wide), a tight one on metal only from the real normal (that follows\n"
"     the gun's own shape and movement; the facets were on the skin) */\n"
"  float lspec = pow(saturate(dot(Nd, Hh)), 6.0) * (0.06 + 0.2 * metal) + pow(saturate(dot(Nr, Hh)), 40.0) * smoothstep(0.6, 0.92, metal) * 0.9;   /* only real metal (gun-grey, not skin) */\n"
"  float rim = pow(1.0 - saturate(dot(Nd, V)), 1.5) * saturate(ndl + 0.25);\n"
"  float facing = saturate(ndl * 0.7 + 0.3);                /* how much this part of the weapon faces the light: water and blood shine there */\n"
"  float gloss = saturate(0.2 * wet * facing + 0.8 * cw * (0.4 + 0.6 * facing) + 0.9 * bcov * bfresh * (0.5 + 0.5 * facing)) * (1.0 - 0.75 * dmask);\n"
"  float specAmt = str * (0.15 + 0.85 * fres) * gloss * 1.9;\n"
"  float3 rgb = col.rgb * (1.0 + 0.5 * lint * (2.0 * wrapl - 1.0));\n"
"  rgb *= lerp(float3(1, 1, 1), envA / el, 0.08 * str);\n"
"  rgb += lcol * (lspec * 0.9 + rim * 0.5) * lint * (0.5 + 0.5 * saturate(el * 3.0));\n"
"  rgb *= 1.0 - 0.12 * wet;\n"
"  rgb *= 1.0 - 0.30 * a.ring;\n"
"  rgb = lerp(rgb, float3(0.62, 0.54, 0.42) * (0.25 + el * 0.9), dmask * (0.5 + 0.25 * saturate(dust - 1.0)));\n"
"  float3 bcol = lerp(wp2.rgb * 0.28 + float3(0.012, 0.006, 0.004), wp2.rgb * 1.1, bfresh) * (0.5 + el);\n"
"  rgb = lerp(rgb, bcol, bcov * 0.93);\n"
"  rgb += env * specAmt;\n"
"  rgb += env * cw * (0.3 + 1.5 * fres) * (0.4 + 0.6 * facing);\n"
"  rgb += env * bcov * bfresh * (0.2 + 1.0 * fres) * (0.5 + 0.5 * facing);\n"
"  return float4(rgb, col.a);\n"
"}\n";

/* The vertex shader turns surface pixels into clip space with the vertex's
 * own w, so the hardware interpolates the texture coordinates perspective-
 * correctly exactly as the software path does (s/w, t/w, 1/w, divide).
 * Colours and fog are interpolated linearly in screen space there, hence
 * noperspective.
 *
 * The pixel shader is nv2a_rc_run (nv2a_combiner.c) over the decoded program
 * in PSC: same register file, same input mapping (clamp, scale, bias), same
 * output scale/bias and clamps, same final combiner. Keep the two in step. */
static const char k_hlsl[] =
"cbuffer VSC : register(b0) { float4 vsc; };\n"
"struct VI { float4 pos:POSITION; float4 d0:COLOR0; float4 d1:COLOR1;\n"
"  float4 fog:TEXCOORD4; float4 t0:TEXCOORD0; float4 t1:TEXCOORD1;\n"
"  float4 t2:TEXCOORD2; float4 t3:TEXCOORD3; };\n"
"struct VO { float4 pos:SV_Position; noperspective float4 d0:COLOR0;\n"
"  noperspective float4 d1:COLOR1; noperspective float4 fog:TEXCOORD4;\n"
"  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2;\n"
"  float4 t3:TEXCOORD3; };\n"
"VO vsmain(VI i) {\n"
"  VO o; float w = i.pos.w;\n"
"  o.pos = float4((i.pos.x * vsc.x * 2.0 - 1.0) * w,\n"
"                 (1.0 - i.pos.y * vsc.y * 2.0) * w, i.pos.z * vsc.z * w, w);\n"
"  o.d0 = i.d0; o.d1 = i.d1; o.fog = i.fog;\n"
"  o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3; return o;\n"
"}\n"
"cbuffer PSC : register(b1) {\n"
"  int4 misc;   /* stages, mux_msb, stage0_on, has_final */\n"
"  int4 misc2;  /* fflags, use_rc, alpha_test, alpha_func */\n"
"  int4 misc3;  /* alpha_ref, clip_plane_mode, stage_program, textured */\n"
"  float4 fogc; float4 fc0; float4 fc1;\n"
"  float4 c0[8]; float4 c1[8];\n"
"  int4 fl[8];      /* fl, afl, dst ab|cd<<4|sum<<8, alpha dst */\n"
"  float4 outm[8];  /* pre, mul, apre, amul */\n"
"  int4 inreg[72];  /* reg, rep */\n"
"  float4 inmap[72];/* lo, hi, s, b */\n"
"  float4 tscale[4];/* coordinate scale x y, z: texture valid, w: alpha 1 */\n"
"  float4 wpn;      /* x: weapon lighting strength (0 = a normal draw) */\n"
"};\n"
"Texture2D tx0 : register(t0); Texture2D tx1 : register(t1);\n"
"Texture2D tx2 : register(t2); Texture2D tx3 : register(t3);\n"
"SamplerState sm0 : register(s0); SamplerState sm1 : register(s1);\n"
"SamplerState sm2 : register(s2); SamplerState sm3 : register(s3);\n"
"static float4 R[16];\n"
"float rcmap(int i, float x) { float4 m = inmap[i];\n"
"  float p = x < m.x ? m.x : x; p = p > m.y ? m.y : p; return m.z * p + m.w; }\n"
"float3 rcrgb(int i) { int4 d = inreg[i]; float4 r = R[d.x];\n"
"  if (d.y != 0) { float a = rcmap(i, r.w); return float3(a, a, a); }\n"
"  return float3(rcmap(i, r.x), rcmap(i, r.y), rcmap(i, r.z)); }\n"
"float rcalpha(int i) { int4 d = inreg[i]; float4 r = R[d.x];\n"
"  return rcmap(i, d.y == 1 ? r.w : r.z); }\n"
"float3 rcres3(float3 x, float pre, float mul) { return clamp((x + pre) * mul, -1.0, 1.0); }\n"
"float rcres(float x, float pre, float mul) { return clamp((x + pre) * mul, -1.0, 1.0); }\n"
"float4 samp(int st, float2 uv) {\n"
"  if (st == 0) return tx0.Sample(sm0, uv);\n"
"  if (st == 1) return tx1.Sample(sm1, uv);\n"
"  if (st == 2) return tx2.Sample(sm2, uv);\n"
"  return tx3.Sample(sm3, uv); }\n"
"float4 psmain(VO i) : SV_Target {\n"
"  float4 tc[4] = { i.t0, i.t1, i.t2, i.t3 };\n"
"  float4 t[4]; bool kill = false; int st, s, j;\n"
"  [unroll] for (st = 0; st < 4; st++) {\n"
"    int mode = (misc3.z >> (st * 5)) & 31; float4 c = tc[st];\n"
"    float4 o = float4(0, 0, 0, 0);\n"
"    if (misc2.y == 0 && st == 0) mode = 1;\n"
"    if (mode == 0) o = float4(0, 0, 0, 1);\n"
"    else if (mode == 1 || mode == 2 || mode == 3) {\n"
"      float q = (mode == 3 || c.w == 0.0 || misc2.y == 0) ? 1.0 : c.w;\n"
"      float2 uv = c.xy / q * tscale[st].xy;\n"
"      o = tscale[st].z > 0.0 ? samp(st, uv) : float4(1, 1, 1, 1);\n"
"      if (tscale[st].w > 0.0) o.w = 1.0;\n"
"    } else if (mode == 4) o = saturate(c);\n"
"    else if (mode == 5) {\n"
"      [unroll] for (j = 0; j < 4; j++) {\n"
"        int ge = (misc3.y >> (st * 4 + j)) & 1;\n"
"        if (ge != 0 ? c[j] >= 0.0 : c[j] < 0.0) kill = true; }\n"
"    }\n"
"    t[st] = o;\n"
"  }\n"
"  if (kill) discard;\n"
"  float4 col;\n"
"  if (misc2.y == 0) {\n"
"    col = i.d0; if (misc3.w != 0) col *= t[0];\n"
"  } else {\n"
"    [unroll] for (j = 0; j < 16; j++) R[j] = float4(0, 0, 0, 0);\n"
"    R[3] = float4(fogc.xyz, i.fog.x); R[4] = i.d0; R[5] = i.d1;\n"
"    R[8] = t[0]; R[9] = t[1]; R[10] = t[2]; R[11] = t[3];\n"
"    R[12].w = misc.z != 0 ? t[0].w : 1.0;\n"
"    [loop] for (s = 0; s < misc.x; s++) {\n"
"      R[1] = c0[s]; R[2] = c1[s];\n"
"      bool mux = misc.y != 0 ? R[12].w >= 0.5 : (((int)(R[12].w * 255.0)) & 1) != 0;\n"
"      int b = s * 8;\n"
"      float3 A = rcrgb(b), B = rcrgb(b + 1), C = rcrgb(b + 2), D = rcrgb(b + 3);\n"
"      float aA = rcalpha(b + 4), aB = rcalpha(b + 5);\n"
"      float aC = rcalpha(b + 6), aD = rcalpha(b + 7);\n"
"      int f = fl[s].x, af = fl[s].y; float4 om = outm[s];\n"
"      float dab = dot(A, B), dcd = dot(C, D);\n"
"      float3 pab = (f & 2) != 0 ? float3(dab, dab, dab) : A * B;\n"
"      float3 pcd = (f & 1) != 0 ? float3(dcd, dcd, dcd) : C * D;\n"
"      float3 sum = (f & 4) != 0 ? (mux ? pcd : pab) : pab + pcd;\n"
"      float3 ab = rcres3(pab, om.x, om.y), cd = rcres3(pcd, om.x, om.y);\n"
"      float3 ms = rcres3(sum, om.x, om.y);\n"
"      float apab = aA * aB, apcd = aC * aD;\n"
"      float asum = (af & 4) != 0 ? (mux ? apcd : apab) : apab + apcd;\n"
"      float aab = rcres(apab, om.z, om.w), acd = rcres(apcd, om.z, om.w);\n"
"      float ams = rcres(asum, om.z, om.w);\n"
"      int dst = fl[s].z, adst = fl[s].w; int d;\n"
"      d = dst & 15; if (d != 0) { R[d].xyz = ab; if ((f & 0x80) != 0) R[d].w = ab.z; }\n"
"      d = (dst >> 4) & 15; if (d != 0) { R[d].xyz = cd; if ((f & 0x40) != 0) R[d].w = cd.z; }\n"
"      d = (dst >> 8) & 15; if (d != 0) R[d].xyz = ms;\n"
"      d = adst & 15; if (d != 0) R[d].w = aab;\n"
"      d = (adst >> 4) & 15; if (d != 0) R[d].w = acd;\n"
"      d = (adst >> 8) & 15; if (d != 0) R[d].w = ams;\n"
"      R[0] = float4(0, 0, 0, 0);\n"
"    }\n"
"    if (misc.w != 0) {\n"
"      int ff = misc2.x;\n"
"      R[1] = fc0; R[2] = fc1;\n"
"      float3 va = (ff & 0x40) != 0 ? 1.0 - R[5].xyz : R[5].xyz;\n"
"      float3 vb = (ff & 0x20) != 0 ? 1.0 - R[12].xyz : R[12].xyz;\n"
"      R[14] = float4((ff & 0x80) != 0 ? saturate(va + vb) : va + vb, 0);\n"
"      float3 E = rcrgb(64), F = rcrgb(65); R[15] = float4(E * F, 0);\n"
"      float3 A = rcrgb(66), B = rcrgb(67), C = rcrgb(68), D = rcrgb(69);\n"
"      col = float4(D + C * (1.0 - A) + B * A, rcalpha(70));\n"
"    } else {\n"
"      col = R[12];\n"
"    }\n"
"  }\n"
"  col = saturate(col);\n"
"  if (misc2.z != 0) {\n"
"    int v = (int)(col.w * 255.0 + 0.5), r = misc3.x & 255, fn = misc2.w;\n"
"    bool ok = fn == 0x200 ? false : fn == 0x201 ? v < r : fn == 0x202 ? v == r :\n"
"      fn == 0x203 ? v <= r : fn == 0x204 ? v > r : fn == 0x205 ? v != r :\n"
"      fn == 0x206 ? v >= r : true;\n"
"    if (!ok) discard;\n"
"  }\n"
"  return col;\n"
"}\n";

/* Must match PSC above, field for field. */
typedef struct {
    int32_t misc[4], misc2[4], misc3[4];
    float fogc[4], fc0[4], fc1[4];
    float c0[8][4], c1[8][4];
    int32_t fl[8][4];
    float outm[8][4];
    int32_t inreg[72][4];
    float inmap[72][4];
    float tscale[4][4];
    float wpn[4];                     /* x: weapon lighting strength; 0 for every other draw */
} PSConsts;

/* ---- pixel shaders specialised per combiner setup ---------------------------
 *
 * psmain interprets the register combiners for every pixel: a loop over the
 * stages, each input read from a register file indexed at run time, which
 * on the GPU lives in local memory. At 640x480 that was 40% of an RTX 4060,
 * and at three times the resolution the GPU was saturated at 30 fps; with
 * the combiners bypassed the same frames ran at 60 fps on 39%. So each
 * distinct setup -- stage count, which register feeds which input, the
 * output routing, the texture stage modes -- gets its own shader, written
 * out straight-line from the same rules psmain follows; the numbers that
 * change from draw to draw (constant colours, input mappings, texture
 * scales, the alpha reference) stay in the PSC buffer. A new setup draws
 * with psmain while its shader compiles on a worker thread, and compiled
 * shaders are kept in shader_cache\ for the next run.
 * RECOMP_GPU_PS_SPECIAL=0 keeps psmain for everything. */

#define PSS_VERSION 1u                /* bump when the generator changes */
#define PSS_SLOTS 4096
#define PSS_KEY_MAX 256

typedef struct {
    uint64_t hash;
    uint32_t nkey;
    uint32_t *key;
    ID3D11PixelShader *volatile ps;
    volatile LONG state;              /* 0 free, 1 queued, 2 ready, 3 failed */
} PsSpec;
static PsSpec s_pss[PSS_SLOTS];
static int s_pss_on = -1;

typedef struct { int slot; char *src; } PsJob;
#define PSS_JOBS 512
static PsJob s_pss_q[PSS_JOBS];
static volatile LONG s_pss_head, s_pss_tail;
static HANDLE s_pss_event;
static CRITICAL_SECTION s_pss_lock;

/* The structural part of a PSC block. */
static uint32_t pss_key(const PSConsts *pc, uint32_t *k)
{
    uint32_t n = 0, s, i, st;
    int use_rc = pc->misc2[1] != 0;
    k[n++] = PSS_VERSION;
    k[n++] = (uint32_t)use_rc;
    k[n++] = (uint32_t)pc->misc3[2];            /* stage modes */
    k[n++] = (uint32_t)pc->misc3[1];            /* texkill comparisons */
    k[n++] = (uint32_t)pc->misc3[3];            /* textured */
    k[n++] = (uint32_t)pc->misc2[2];            /* alpha test on */
    if (pc->wpn[0] > 0.0f)
        k[n++] = 0x57504C00u | WPN_VERSION;     /* a weapon variant: every other draw keeps its key (and cached shader) */
    for (st = 0; st < 4; st++)
        k[n++] = (pc->tscale[st][2] > 0.0f ? 1u : 0u) | (pc->tscale[st][3] > 0.0f ? 2u : 0u);
    if (use_rc) {
        uint32_t ns = (uint32_t)pc->misc[0] > 8 ? 8 : (uint32_t)pc->misc[0];
        k[n++] = ns;
        k[n++] = (uint32_t)pc->misc[1];         /* mux msb */
        k[n++] = (uint32_t)pc->misc[2];         /* stage 0 alpha */
        k[n++] = (uint32_t)pc->misc[3];         /* final combiner */
        k[n++] = (uint32_t)pc->misc2[0];        /* final flags */
        for (s = 0; s < ns; s++) {
            for (i = 0; i < 4; i++)
                k[n++] = (uint32_t)pc->fl[s][i];
            for (i = 0; i < 8; i++)
                k[n++] = (uint32_t)pc->inreg[s * 8 + i][0] | ((uint32_t)pc->inreg[s * 8 + i][1] << 8);
        }
        if (pc->misc[3])
            for (i = 64; i < 71; i++)
                k[n++] = (uint32_t)pc->inreg[i][0] | ((uint32_t)pc->inreg[i][1] << 8);
    }
    return n;
}

static uint64_t pss_hash(const uint32_t *k, uint32_t n)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t i;
    for (i = 0; i < n; i++) {
        h ^= k[i];
        h *= 1099511628211ull;
    }
    return h;
}

typedef struct { char *p; size_t n, cap; } Sb;
static void sb_add(Sb *b, const char *fmt, ...)
{
    va_list ap;
    int m;
    for (;;) {
        va_start(ap, fmt);
        m = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
        va_end(ap);
        if (m >= 0 && (size_t)m < b->cap - b->n) {
            b->n += (size_t)m;
            return;
        }
        b->cap = b->cap * 2 + 4096;
        b->p = (char *)realloc(b->p, b->cap);
    }
}

static const char k_pss_head[] =
"struct VO { float4 pos:SV_Position; noperspective float4 d0:COLOR0;\n"
"  noperspective float4 d1:COLOR1; noperspective float4 fog:TEXCOORD4;\n"
"  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2;\n"
"  float4 t3:TEXCOORD3; };\n"
"cbuffer PSC : register(b1) {\n"
"  int4 misc; int4 misc2; int4 misc3;\n"
"  float4 fogc; float4 fc0; float4 fc1;\n"
"  float4 c0[8]; float4 c1[8];\n"
"  int4 fl[8]; float4 outm[8];\n"
"  int4 inreg[72]; float4 inmap[72];\n"
"  float4 tscale[4];\n"
"  float4 wpn;\n"
"};\n"
"Texture2D tx0 : register(t0); Texture2D tx1 : register(t1);\n"
"Texture2D tx2 : register(t2); Texture2D tx3 : register(t3);\n"
"SamplerState sm0 : register(s0); SamplerState sm1 : register(s1);\n"
"SamplerState sm2 : register(s2); SamplerState sm3 : register(s3);\n"
"float rcmap(int i, float x) { float4 m = inmap[i];\n"
"  float p = x < m.x ? m.x : x; p = p > m.y ? m.y : p; return m.z * p + m.w; }\n"
"float3 rcres3(float3 x, float pre, float mul) { return clamp((x + pre) * mul, -1.0, 1.0); }\n"
"float rcres(float x, float pre, float mul) { return clamp((x + pre) * mul, -1.0, 1.0); }\n";

/* An RGB input i (rcrgb), an alpha one (rcalpha). */
static void pss_rgb(Sb *b, const PSConsts *pc, int i, const char *name)
{
    int reg = pc->inreg[i][0] & 15;
    if (pc->inreg[i][1] != 0)
        sb_add(b, "  float3 %s = rcmap(%d, r%d.w).xxx;\n", name, i, reg);
    else
        sb_add(b, "  float3 %s = float3(rcmap(%d, r%d.x), rcmap(%d, r%d.y), rcmap(%d, r%d.z));\n",
               name, i, reg, i, reg, i, reg);
}
static void pss_alpha(Sb *b, const PSConsts *pc, int i, const char *name)
{
    int reg = pc->inreg[i][0] & 15;
    sb_add(b, "  float %s = rcmap(%d, r%d.%c);\n", name, i, reg,
           pc->inreg[i][1] == 1 ? 'w' : 'z');
}

static char *pss_source(const PSConsts *pc)
{
    Sb b = { NULL, 0, 0 };
    int use_rc = pc->misc2[1] != 0, st, s, j;
    int ns = pc->misc[0] > 8 ? 8 : pc->misc[0];
    static const char comp[4] = { 'x', 'y', 'z', 'w' };

    sb_add(&b, "%s", k_pss_head);
    if (pc->wpn[0] > 0.0f)
        sb_add(&b, "%s", k_wpn_hlsl);
    sb_add(&b, "float4 psmain(VO i) : SV_Target {\n");
    sb_add(&b, "  float4 t0 = 0, t1 = 0, t2 = 0, t3 = 0; bool kill = false;\n");
    for (st = 0; st < 4; st++) {
        int mode = (pc->misc3[2] >> (st * 5)) & 31;
        if (!use_rc && st == 0)
            mode = 1;
        if (mode == 0)
            sb_add(&b, "  t%d = float4(0, 0, 0, 1);\n", st);
        else if (mode >= 1 && mode <= 3) {
            if (mode == 3 || !use_rc)
                sb_add(&b, "  { float q = 1.0;\n");
            else
                sb_add(&b, "  { float q = i.t%d.w == 0.0 ? 1.0 : i.t%d.w;\n", st, st);
            if (pc->tscale[st][2] > 0.0f)
                sb_add(&b, "    t%d = tx%d.Sample(sm%d, i.t%d.xy / q * tscale[%d].xy); }\n",
                       st, st, st, st, st);
            else
                sb_add(&b, "    t%d = float4(1, 1, 1, 1); }\n", st);
            if (pc->tscale[st][3] > 0.0f)
                sb_add(&b, "  t%d.w = 1.0;\n", st);
        } else if (mode == 4)
            sb_add(&b, "  t%d = saturate(i.t%d);\n", st, st);
        else if (mode == 5) {
            for (j = 0; j < 4; j++) {
                int ge = (pc->misc3[1] >> (st * 4 + j)) & 1;
                sb_add(&b, "  if (i.t%d.%c %s 0.0) kill = true;\n", st, comp[j], ge ? ">=" : "<");
            }
        }
    }
    sb_add(&b, "  if (kill) discard;\n  float4 col;\n");
    if (!use_rc) {
        sb_add(&b, "  col = i.d0;\n");
        if (pc->misc3[3] != 0)
            sb_add(&b, "  col *= t0;\n");
    } else {
        sb_add(&b, "  float4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0;\n"
                   "  float4 r8 = 0, r9 = 0, r10 = 0, r11 = 0, r12 = 0, r13 = 0, r14 = 0, r15 = 0;\n"
                   "  r3 = float4(fogc.xyz, i.fog.x); r4 = i.d0; r5 = i.d1;\n"
                   "  r8 = t0; r9 = t1; r10 = t2; r11 = t3;\n");
        sb_add(&b, "  r12.w = %s;\n", pc->misc[2] != 0 ? "t0.w" : "1.0");
        for (s = 0; s < ns; s++) {
            int f = pc->fl[s][0], af = pc->fl[s][1], dst = pc->fl[s][2], adst = pc->fl[s][3];
            int base = s * 8, d;
            sb_add(&b, "  { r1 = c0[%d]; r2 = c1[%d];\n", s, s);
            sb_add(&b, "  bool mux = %s;\n", pc->misc[1] != 0 ? "r12.w >= 0.5"
                                                         : "(((int)(r12.w * 255.0)) & 1) != 0");
            pss_rgb(&b, pc, base + 0, "A");
            pss_rgb(&b, pc, base + 1, "B");
            pss_rgb(&b, pc, base + 2, "C");
            pss_rgb(&b, pc, base + 3, "D");
            pss_alpha(&b, pc, base + 4, "aA");
            pss_alpha(&b, pc, base + 5, "aB");
            pss_alpha(&b, pc, base + 6, "aC");
            pss_alpha(&b, pc, base + 7, "aD");
            sb_add(&b, "  float4 om = outm[%d];\n", s);
            sb_add(&b, "  float3 pab = %s;\n", (f & 2) ? "dot(A, B).xxx" : "A * B");
            sb_add(&b, "  float3 pcd = %s;\n", (f & 1) ? "dot(C, D).xxx" : "C * D");
            sb_add(&b, "  float3 sum = %s;\n", (f & 4) ? "(mux ? pcd : pab)" : "pab + pcd");
            sb_add(&b, "  float3 ab = rcres3(pab, om.x, om.y), cd = rcres3(pcd, om.x, om.y);\n"
                       "  float3 ms = rcres3(sum, om.x, om.y);\n"
                       "  float apab = aA * aB, apcd = aC * aD;\n");
            sb_add(&b, "  float asum = %s;\n", (af & 4) ? "(mux ? apcd : apab)" : "apab + apcd");
            sb_add(&b, "  float aab = rcres(apab, om.z, om.w), acd = rcres(apcd, om.z, om.w);\n"
                       "  float ams = rcres(asum, om.z, om.w);\n");
            d = dst & 15;
            if (d) {
                sb_add(&b, "  r%d.xyz = ab;\n", d);
                if (f & 0x80) sb_add(&b, "  r%d.w = ab.z;\n", d);
            }
            d = (dst >> 4) & 15;
            if (d) {
                sb_add(&b, "  r%d.xyz = cd;\n", d);
                if (f & 0x40) sb_add(&b, "  r%d.w = cd.z;\n", d);
            }
            d = (dst >> 8) & 15;
            if (d) sb_add(&b, "  r%d.xyz = ms;\n", d);
            d = adst & 15;
            if (d) sb_add(&b, "  r%d.w = aab;\n", d);
            d = (adst >> 4) & 15;
            if (d) sb_add(&b, "  r%d.w = acd;\n", d);
            d = (adst >> 8) & 15;
            if (d) sb_add(&b, "  r%d.w = ams;\n", d);
            sb_add(&b, "  r0 = float4(0, 0, 0, 0); }\n");
        }
        if (pc->misc[3] != 0) {
            int ff = pc->misc2[0];
            sb_add(&b, "  { r1 = fc0; r2 = fc1;\n");
            sb_add(&b, "  float3 va = %s, vb = %s;\n",
                   (ff & 0x40) ? "1.0 - r5.xyz" : "r5.xyz",
                   (ff & 0x20) ? "1.0 - r12.xyz" : "r12.xyz");
            sb_add(&b, "  r14 = float4(%s, 0);\n", (ff & 0x80) ? "saturate(va + vb)" : "va + vb");
            pss_rgb(&b, pc, 64, "E");
            pss_rgb(&b, pc, 65, "F");
            sb_add(&b, "  r15 = float4(E * F, 0);\n");
            pss_rgb(&b, pc, 66, "A");
            pss_rgb(&b, pc, 67, "B");
            pss_rgb(&b, pc, 68, "C");
            pss_rgb(&b, pc, 69, "D");
            pss_alpha(&b, pc, 70, "aG");
            sb_add(&b, "  col = float4(D + C * (1.0 - A) + B * A, aG); }\n");
        } else
            sb_add(&b, "  col = r12;\n");
    }
    if (pc->wpn[0] > 0.0f)
        sb_add(&b, "  col = wpn_post(col, i.pos, i.t0.xy * tscale[0].xy);\n");
    sb_add(&b, "  col = saturate(col);\n");
    if (pc->misc2[2] != 0)
        sb_add(&b, "  { int v = (int)(col.w * 255.0 + 0.5), r = misc3.x & 255, fn = misc2.w;\n"
                   "  bool ok = fn == 0x200 ? false : fn == 0x201 ? v < r : fn == 0x202 ? v == r :\n"
                   "    fn == 0x203 ? v <= r : fn == 0x204 ? v > r : fn == 0x205 ? v != r :\n"
                   "    fn == 0x206 ? v >= r : true;\n"
                   "  if (!ok) discard; }\n");
    sb_add(&b, "  return col;\n}\n");
    return b.p;
}

static void pss_cache_path(char *out, size_t n, uint64_t h)
{
    snprintf(out, n, "shader_cache\\ps_%016llx.cso", (unsigned long long)h);
}

static DWORD WINAPI pss_worker(LPVOID unused)
{
    (void)unused;
    for (;;) {
        PsJob job;
        int have = 0;
        WaitForSingleObject(s_pss_event, INFINITE);
        for (;;) {
            EnterCriticalSection(&s_pss_lock);
            have = s_pss_tail != s_pss_head;
            if (have) {
                job = s_pss_q[s_pss_tail % PSS_JOBS];
                s_pss_tail++;
            }
            LeaveCriticalSection(&s_pss_lock);
            if (!have)
                break;
            {
                PsSpec *e = &s_pss[job.slot];
                ID3D11PixelShader *ps = NULL;
                char path[64];
                FILE *f;
                DWORD t_start = GetTickCount();
                int from_cache = 0;
                pss_cache_path(path, sizeof path, e->hash);
                if ((f = fopen(path, "rb")) != NULL) {
                    long sz;
                    void *buf;
                    fseek(f, 0, SEEK_END);
                    sz = ftell(f);
                    fseek(f, 0, SEEK_SET);
                    buf = sz > 0 ? malloc((size_t)sz) : NULL;
                    if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz)
                        ID3D11Device_CreatePixelShader(s_dev, buf, (SIZE_T)sz, NULL, &ps);
                    free(buf);
                    fclose(f);
                }
                if (!ps) {
                    ID3DBlob *code = NULL, *err = NULL;
                    HRESULT hr = D3DCompile(job.src, strlen(job.src), "nv2a_ps", NULL, NULL,
                                            "psmain", "ps_5_0",
                                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
                    if (SUCCEEDED(hr)
                        && SUCCEEDED(ID3D11Device_CreatePixelShader(
                               s_dev, ID3D10Blob_GetBufferPointer(code),
                               ID3D10Blob_GetBufferSize(code), NULL, &ps))) {
                        CreateDirectoryA("shader_cache", NULL);
                        if ((f = fopen(path, "wb")) != NULL) {
                            fwrite(ID3D10Blob_GetBufferPointer(code), 1,
                                   ID3D10Blob_GetBufferSize(code), f);
                            fclose(f);
                        }
                    } else {
                        static int shown;
                        if (shown++ < 4)
                            fprintf(stderr, "[NV2A-GPU] combiner shader failed (0x%08lX)%s%s\n%s\n",
                                    (unsigned long)hr, err ? ":\n" : "",
                                    err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "",
                                    job.src);
                    }
                    if (code) ID3D10Blob_Release(code);
                    if (err) ID3D10Blob_Release(err);
                }
                free(job.src);
                e->ps = ps;
                {   /* [PSCOMP]: when a pixel shader setup was first seen and how long its own shader took to come */
                    static volatile LONG n_shown;
                    from_cache = ps != NULL && GetTickCount() - t_start < 15;
                    if (InterlockedIncrement(&n_shown) <= 400)
                        fprintf(stderr, "[PSCOMP] t=%lu %016llx %s in %lu ms (queue %ld)\n", (unsigned long)GetTickCount(),
                                (unsigned long long)e->hash, ps ? "ready" : "FAILED",
                                (unsigned long)(GetTickCount() - t_start), (long)(s_pss_head - s_pss_tail));
                }
                (void)from_cache;
                InterlockedExchange(&e->state, ps ? 2 : 3);
            }
        }
    }
    return 0;
}

/* The shader for this PSC block: its own once compiled, psmain until then. */
static ID3D11PixelShader *pss_lookup(const PSConsts *pc)
{
    uint32_t k[PSS_KEY_MAX], n, i;
    uint64_t h;
    PsSpec *e = NULL;

    if (s_pss_on < 0) {
        const char *v = getenv("RECOMP_GPU_PS_SPECIAL");
        s_pss_on = !(v && v[0] == '0');
        if (s_pss_on) {
            InitializeCriticalSection(&s_pss_lock);
            s_pss_event = CreateEventA(NULL, FALSE, FALSE, NULL);
            CloseHandle(CreateThread(NULL, 0, pss_worker, NULL, 0, NULL));
        }
    }
    if (!s_pss_on)
        return s_ps;
    n = pss_key(pc, k);
    h = pss_hash(k, n);
    for (i = 0; i < PSS_SLOTS; i++) {
        PsSpec *c = &s_pss[(h + i) & (PSS_SLOTS - 1)];
        if (!c->key) {
            e = c;
            break;
        }
        if (c->hash == h && c->nkey == n && !memcmp(c->key, k, n * 4)) {
            if (c->state == 2)
                return c->ps;
            return s_ps;                  /* queued, or failed */
        }
    }
    if (!e)
        return s_ps;                      /* table full */
    e->hash = h;
    e->nkey = n;
    e->key = (uint32_t *)malloc(n * 4);
    if (!e->key)
        return s_ps;
    memcpy(e->key, k, n * 4);
    e->state = 1;
    EnterCriticalSection(&s_pss_lock);
    if (s_pss_head - s_pss_tail < PSS_JOBS) {
        s_pss_q[s_pss_head % PSS_JOBS].slot = (int)(e - s_pss);
        s_pss_q[s_pss_head % PSS_JOBS].src = pss_source(pc);
        s_pss_head++;
    } else
        e->state = 3;
    LeaveCriticalSection(&s_pss_lock);
    SetEvent(s_pss_event);
    return s_ps;
}

typedef struct { float v[4]; } VSConsts;

/* Vertex programs on the GPU (nv2a_gpu_draw_program): the title's NV2A
 * program interpreted per vertex, instruction for instruction as
 * nv2a_vsh_run does it, from the raw 128-bit words. Its inputs are the
 * vertex's fetched attributes in a structured buffer, indexed by
 * SV_VertexID; the near-plane clipping the executor did on the CPU is the
 * two clip distances. */
static const char k_hlsl_prog[] =
"cbuffer VSC : register(b0) { float4 vsc; };\n"
"cbuffer VPROG : register(b2) { uint4 prog[136]; };\n"
"cbuffer VCONST : register(b3) { float4 cst[192]; };\n"
"cbuffer VDRAW : register(b4) {\n"
"  uint4 di;      // x instructions, y inputs per vertex, z first input, w near plane on\n"
"  float4 df;     // x, y anti-aliasing scale, z near-plane offset\n"
"  float4 fogp;   // x, y fog params, z fog on, w fog mode (bits)\n"
"  uint4 slot[4]; // input register -> its place among a vertex's inputs\n"
"  float4 hud;    // x: ultrawide HUD batch shift (fraction of the picture width)\n"
"};\n"
"StructuredBuffer<float4> vin : register(t0);\n"
"struct VOP { float4 pos:SV_Position; noperspective float4 d0:COLOR0;\n"
"  noperspective float4 d1:COLOR1; noperspective float4 fog:TEXCOORD4;\n"
"  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2;\n"
"  float4 t3:TEXCOORD3; float2 cd:SV_ClipDistance0; };\n"
"uint F(uint w, uint lo, uint n) { return (w >> lo) & ((1u << n) - 1u); }\n"
"float4 swz(float4 v, uint s) {\n"
"  return float4(v[(s >> 6) & 3], v[(s >> 4) & 3], v[(s >> 2) & 3], v[s & 3]);\n"
"}\n"
"void wm(inout float4 d, float4 s, uint m) {\n"
"  if (m & 8) d.x = s.x; if (m & 4) d.y = s.y; if (m & 2) d.z = s.z; if (m & 1) d.w = s.w;\n"
"}\n"
"bool finite1(float x) { return (asuint(x) & 0x7f800000u) != 0x7f800000u; }\n"
"VOP vsprog(uint vid : SV_VertexID) {\n"
"  float4 r[13], o[13];\n"
"  int a0 = 0;\n"
"  uint vb = di.z + vid * di.y, k, s;\n"
"  [unroll] for (k = 0; k < 13; k++) { r[k] = 0; o[k] = 0; }\n"
"  o[3].w = 1; o[4].w = 1; o[9].w = 1; o[10].w = 1; o[11].w = 1; o[12].w = 1;\n"
"  [loop] for (s = 0; s < di.x; s++) {\n"
"    uint4 ins = prog[s];\n"
"    float4 src[3];\n"
"    [unroll] for (k = 0; k < 3; k++) {\n"
"      uint neg, sw, mux, reg;\n"
"      if (k == 0) { neg = F(ins.y, 8, 1); sw = F(ins.y, 0, 8); mux = F(ins.z, 26, 2); reg = F(ins.z, 28, 4); }\n"
"      else if (k == 1) { neg = F(ins.z, 25, 1); sw = F(ins.z, 17, 8); mux = F(ins.z, 11, 2); reg = F(ins.z, 13, 4); }\n"
"      else { neg = F(ins.z, 10, 1); sw = F(ins.z, 2, 8); mux = F(ins.w, 28, 2); reg = (F(ins.z, 0, 2) << 2) | F(ins.w, 30, 2); }\n"
"      float4 v;\n"
"      if (mux == 1) v = reg <= 12 ? r[reg] : r[0];\n"
"      else if (mux == 2) { uint ir = F(ins.y, 9, 4); v = vin[vb + slot[ir >> 2][ir & 3]]; }\n"
"      else {\n"
"        int ci = (int)F(ins.y, 13, 8) + (F(ins.w, 1, 1) ? a0 : 0);\n"
"        v = (ci < 0 || ci >= 192) ? float4(0, 0, 0, 0) : cst[ci];\n"
"      }\n"
"      v = swz(v, sw);\n"
"      src[k] = neg ? -v : v;\n"
"    }\n"
"    float4 A = src[0], B = src[1], C = src[2];\n"
"    uint mac = F(ins.y, 21, 4), ilu = F(ins.y, 25, 3);\n"
"    float4 m = 0, il = 0;\n"
"    if (mac != 0) {\n"
"      m = A;\n"
"      if (mac == 2) m = A * B;\n"
"      else if (mac == 3) m = A + C;\n"
"      else if (mac == 4) m = A * B + C;\n"
"      else if (mac == 5) m = (A.x * B.x + A.y * B.y + A.z * B.z).xxxx;\n"
"      else if (mac == 6) m = (A.x * B.x + A.y * B.y + A.z * B.z + B.w).xxxx;\n"
"      else if (mac == 7) m = (A.x * B.x + A.y * B.y + A.z * B.z + A.w * B.w).xxxx;\n"
"      else if (mac == 8) m = float4(1, A.y * B.y, A.z, B.w);\n"
"      else if (mac == 9) m = A < B ? A : B;\n"
"      else if (mac == 10) m = A >= B ? A : B;\n"
"      else if (mac == 11) m = A < B ? 1.0 : 0.0;\n"
"      else if (mac == 12) m = A >= B ? 1.0 : 0.0;\n"
"      else if (mac == 13) a0 = (int)floor(A.x + 0.001);\n"
"    }\n"
"    if (ilu != 0) {\n"
"      float x = C.x;\n"
"      il = C;\n"
"      if (ilu == 2) il = (x == 0 ? asfloat(0x7f800000u) : 1.0 / x).xxxx;\n"
"      else if (ilu == 3) {\n"
"        float f = 1.0 / x;\n"
"        if (abs(f) < 5.42101e-20) f = f < 0 ? -5.42101e-20 : 5.42101e-20;\n"
"        if (abs(f) > 1.884467e19) f = f < 0 ? -1.884467e19 : 1.884467e19;\n"
"        il = f.xxxx;\n"
"      }\n"
"      else if (ilu == 4) il = (1.0 / sqrt(abs(x))).xxxx;\n"
"      else if (ilu == 5) { float fl = floor(x); il = float4(exp2(fl), x - fl, exp2(x), 1); }\n"
"      else if (ilu == 6) {\n"
"        float ax = abs(x);\n"
"        if (ax == 0) il = float4(asfloat(0xff800000u), 1, asfloat(0xff800000u), 1);\n"
"        else { float e = floor(log2(ax)); il = float4(e, ax / exp2(e), log2(ax), 1); }\n"
"      }\n"
"      else if (ilu == 7) {\n"
"        float nl = C.x > 0 ? C.x : 0, nh = C.y > 0 ? C.y : 0;\n"
"        float p = clamp(C.w, -127.9961, 127.9961);\n"
"        il = float4(1, nl, C.x > 0 ? pow(nh, p) : 0, 1);\n"
"      }\n"
"    }\n"
"    uint mm = F(ins.w, 24, 4), im = F(ins.w, 16, 4), td = F(ins.w, 20, 4);\n"
"    if (mac != 0 && mac != 13 && mm != 0 && td <= 12) wm(r[td], m, mm);\n"
"    if (ilu != 0 && im != 0) { uint it = mac != 0 ? 1 : td; if (it <= 12) wm(r[it], il, im); }\n"
"    uint om = F(ins.w, 12, 4);\n"
"    if (om != 0 && F(ins.w, 11, 1) != 0) {\n"
"      float4 so = F(ins.w, 2, 1) ? il : m;\n"
"      uint oa = F(ins.w, 3, 8);\n"
"      if (oa == 0) wm(r[12], so, om); else if (oa < 13) wm(o[oa], so, om);\n"
"    }\n"
"    if (F(ins.w, 0, 1)) break;\n"
"  }\n"
"  float4 P = r[12];\n"
"  if (P.w == 0 && asuint(P.w) == 0 && abs(P.x) < 1e15 && abs(P.y) < 1e15) P.w = 1;\n"
"  else if (abs(P.w) < 5.421011e-20) P.w = (asuint(P.w) >> 31) ? -5.421011e-20 : 5.421011e-20;\n"
"  // ultrawide HUD: df.w = k > 0 squeezes the batch's x by k around the centre, hud.x moves the whole batch (the CPU chose its\n"
"  // edge from where the batch is). Vertices on the very edge (a full-screen quad) stay, so fades and tints still cover it all\n"
"  if (df.w > 0 && P.x > 0.5 && P.x < 639.5) P.x = (0.5 + (P.x / 640.0 - 0.5) * df.w + hud.x) * 640.0;\n"
"  P.x *= df.x; P.y *= df.y;\n"
"  VOP v;\n"
"  float w = P.w;\n"
"  v.cd = (di.w & 2) ? float2(1, 1) : float2(w - 1e-3, (di.w & 1) ? P.z * w - df.z * w : 1.0);\n"
"  v.pos = float4((P.x * vsc.x * 2.0 - 1.0) * w, (1.0 - P.y * vsc.y * 2.0) * w, P.z * vsc.z * w, w);\n"
"  if (!finite1(P.x) || !finite1(P.y) || !finite1(P.z) || !finite1(P.w)\n"
"      || !finite1(v.cd.x) || !finite1(v.cd.y))\n"
"    v.pos = asfloat(0x7fc00000u).xxxx;   // dropped, as the software clipper drops it\n"
"  v.d0 = saturate(o[3]); v.d1 = saturate(o[4]);\n"
"  {\n"
"    float d = o[5].x, f, px = fogp.x, py = fogp.y;\n"
"    uint mode = asuint(fogp.w);\n"
"    if (fogp.z == 0) f = 1;\n"
"    else {\n"
"      if (mode == 0x800u || mode == 0x802u) f = px + exp2(d * py * 16.0) - 1.5;\n"
"      else if (mode == 0x801u || mode == 0x803u) f = px + exp2(-d * d * py * py * 32.0) - 1.5;\n"
"      else f = px + d * py - 1.0;\n"
"      if (mode == 0x802u || mode == 0x803u || mode == 0x804u) f = abs(f);\n"
"      if (!finite1(f) && (asuint(f) & 0x007fffffu) != 0) f = 1;\n"
"      f = clamp(f, 0, 1);\n"
"    }\n"
"    v.fog = float4(f, 0, 0, 0);\n"
"  }\n"
"  v.t0 = o[9]; v.t1 = o[10]; v.t2 = o[11]; v.t3 = o[12];\n"
"  return v;\n"
"}\n";

static ID3D11VertexShader *s_vs_prog;
/* RECOMP_GPU_VS_CHECK: the program's outputs streamed out (a geometry shader
 * made from the vertex shader itself) so the executor can compare them with
 * nv2a_vsh_run's. 34 floats a vertex: pos, d0, d1, fog, t0..t3, clip. */
static ID3D11GeometryShader *s_so_gs;
static ID3D11Buffer *s_so_buf, *s_so_stage;
static float *s_cap_out;
#define SO_FLOATS 34
#define SO_MAX_VERTS 65536
static ID3D11Buffer *s_cb_prog, *s_cb_const, *s_cb_draw, *s_vin;
static ID3D11ShaderResourceView *s_vin_srv;
static UINT s_vin_pos;                  /* bytes, a ring like s_vb */
#define GPU_VIN_BYTES (16u << 20)
static uint32_t s_prog_loaded_ver = 0xFFFFFFFFu, s_prog_loaded_n;
static const void *s_prog_loaded_ptr;
static uint32_t s_const_loaded_ver = 0xFFFFFFFFu;
typedef struct {
    uint32_t di[4];
    float df[4];
    float fogp[4];
    uint32_t slot[16];
    float hud[4];                       /* x: ultrawide HUD batch shift (fraction of the picture width) */
} VDrawConsts;

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr;
    /* the generic shader (and the vertex shader) are compiled at start-up, on the game's thread: no weapon lighting in it (that code
     * makes this compile several seconds longer); the weapon's own variants have it (pss_source) */
    hr = D3DCompile(k_hlsl, sizeof k_hlsl - 1, "nv2a_gpu", NULL, NULL,
                    entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[NV2A-GPU] %s: shader compile failed (0x%08lX)\n%s\n",
                entry, (unsigned long)hr,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

static int gpu_init(void)
{
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got;
    UINT flags = getenv("RECOMP_GPU_DEBUG") ? D3D11_CREATE_DEVICE_DEBUG : 0;
    ID3DBlob *vs, *ps;
    HRESULT hr;

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, levels,
                           2, D3D11_SDK_VERSION, &s_dev, &got, &s_ctx);
    if (FAILED(hr))
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags,
                               levels + 1, 1, D3D11_SDK_VERSION, &s_dev, &got,
                               &s_ctx);
    if (FAILED(hr)) {
        fprintf(stderr, "[NV2A-GPU] no D3D11 device (0x%08lX): software"
                        " rasteriser\n", (unsigned long)hr);
        return 0;
    }
    if (FAILED(ID3D11DeviceContext_QueryInterface(s_ctx, &IID_ID3D11DeviceContext1,
                                                  (void **)&s_ctx1)))
        s_ctx1 = NULL;
    scale_init();

    vs = compile("vsmain", "vs_5_0");
    ps = compile("psmain", "ps_5_0");
    if (!vs || !ps)
        return 0;
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vs),
                                    ID3D10Blob_GetBufferSize(vs), NULL, &s_vs);
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(ps),
                                   ID3D10Blob_GetBufferSize(ps), NULL, &s_ps);
    {
        static const D3D11_INPUT_ELEMENT_DESC el[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,   D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 64,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 80,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 96,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 112, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        ID3D11Device_CreateInputLayout(s_dev, el, 8,
                                       ID3D10Blob_GetBufferPointer(vs),
                                       ID3D10Blob_GetBufferSize(vs), &s_il);
    }
    ID3D10Blob_Release(vs);
    ID3D10Blob_Release(ps);
    {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.ByteWidth = GPU_VB_BYTES;
        ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vb);
        s_vb_bytes = GPU_VB_BYTES;
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        bd.ByteWidth = GPU_IB_BYTES;
        ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_ib);
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.ByteWidth = sizeof(VSConsts);
        ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb_vs);
        bd.ByteWidth = (sizeof(PSConsts) + 15) & ~15u;
        ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb_ps);
    }
    {
        /* No culling (the software path draws both windings), no depth
         * clipping (the executor clips at the near plane itself and the
         * software path never clipped far), the clip rectangle as scissor. */
        D3D11_RASTERIZER_DESC rd;
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = FALSE;
        rd.ScissorEnable = TRUE;
        ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs);
    }
    if (!s_vs || !s_ps || !s_il || !s_vb || !s_cb_vs || !s_cb_ps || !s_rs)
        return 0;
    fprintf(stderr, "[NV2A-GPU] D3D11 device ready (feature level %X)%s\n",
            (unsigned)got, s_ctx1 ? "" : ", no ClearView");
    return 1;
}

static ID3DBlob *compile_src(const char *src, size_t n, const char *entry,
                            const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, n, "nv2a_prog", NULL, NULL, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[NV2A-GPU] %s: shader compile failed (0x%08lX)%s%s" "\n",
                entry, (unsigned long)hr, err ? "\n" : "",
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

/* The program path's shader and buffers, made on first use. 0 when the
 * device cannot (no NO_OVERWRITE maps on a buffer a shader reads, which
 * the input ring needs), and the executor keeps running programs itself. */
static int prog_init(void)
{
    static int state = -1;
    D3D11_FEATURE_DATA_D3D11_OPTIONS op;
    D3D11_BUFFER_DESC bd;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv;
    ID3DBlob *b;

    if (state >= 0)
        return state;
    state = 0;
    memset(&op, 0, sizeof op);
    if (FAILED(ID3D11Device_CheckFeatureSupport(s_dev, D3D11_FEATURE_D3D11_OPTIONS,
                                                &op, sizeof op))
        || !op.MapNoOverwriteOnDynamicBufferSRV) {
        fprintf(stderr, "[NV2A-GPU] vertex programs stay on the CPU (no"
                        " NO_OVERWRITE on shader-read buffers)" "\n");
        return 0;
    }
    if (!(b = compile_src(k_hlsl_prog, sizeof k_hlsl_prog - 1, "vsprog", "vs_5_0")))
        return 0;
    if (FAILED(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(b),
                                               ID3D10Blob_GetBufferSize(b), NULL,
                                               &s_vs_prog))) {
        ID3D10Blob_Release(b);
        return 0;
    }
    if (getenv("RECOMP_GPU_VS_CHECK")) {
        static const D3D11_SO_DECLARATION_ENTRY so[] = {
            { 0, "SV_POSITION", 0, 0, 4, 0 }, { 0, "COLOR", 0, 0, 4, 0 },
            { 0, "COLOR", 1, 0, 4, 0 }, { 0, "TEXCOORD", 4, 0, 4, 0 },
            { 0, "TEXCOORD", 0, 0, 4, 0 }, { 0, "TEXCOORD", 1, 0, 4, 0 },
            { 0, "TEXCOORD", 2, 0, 4, 0 }, { 0, "TEXCOORD", 3, 0, 4, 0 },
            { 0, "SV_ClipDistance", 0, 0, 2, 0 } };
        UINT stride = SO_FLOATS * 4;
        D3D11_BUFFER_DESC sb;
        HRESULT hr = ID3D11Device_CreateGeometryShaderWithStreamOutput(
            s_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b),
            so, 9, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, NULL, &s_so_gs);
        memset(&sb, 0, sizeof sb);
        sb.ByteWidth = SO_MAX_VERTS * SO_FLOATS * 4;
        sb.Usage = D3D11_USAGE_DEFAULT;
        sb.BindFlags = D3D11_BIND_STREAM_OUTPUT;
        if (SUCCEEDED(hr))
            hr = ID3D11Device_CreateBuffer(s_dev, &sb, NULL, &s_so_buf);
        sb.Usage = D3D11_USAGE_STAGING;
        sb.BindFlags = 0;
        sb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(hr))
            hr = ID3D11Device_CreateBuffer(s_dev, &sb, NULL, &s_so_stage);
        fprintf(stderr, "[NV2A-GPU] vertex program check %s (0x%08lX)\n",
                SUCCEEDED(hr) ? "on" : "unavailable", (unsigned long)hr);
        if (FAILED(hr))
            s_so_gs = NULL;
    }
    ID3D10Blob_Release(b);
    memset(&bd, 0, sizeof bd);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.ByteWidth = 136 * 16;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb_prog)))
        return 0;
    bd.ByteWidth = 192 * 16;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb_const)))
        return 0;
    bd.ByteWidth = sizeof(VDrawConsts);
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb_draw)))
        return 0;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = 16;
    bd.ByteWidth = GPU_VIN_BYTES;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vin)))
        return 0;
    memset(&sv, 0, sizeof sv);
    sv.Format = DXGI_FORMAT_UNKNOWN;
    sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sv.Buffer.FirstElement = 0;
    sv.Buffer.NumElements = GPU_VIN_BYTES / 16;
    if (FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_vin,
                                                     &sv, &s_vin_srv)))
        return 0;
    fprintf(stderr, "[NV2A-GPU] vertex programs run on the GPU"
                    " (RECOMP_GPU_VS=0 keeps them on the CPU)" "\n");
    state = 1;
    return 1;
}

int nv2a_gpu_program_ok(void)
{
    return s_on > 0 && prog_init();
}

/* ---- Ultrawide (RECOMP_ASPECT) -------------------------------------------
 * The title's widescreen mode draws 16:9 squeezed into the 640x480 frame. For a wider screen the game side (recomp_manual.c)
 * widens the world's horizontal view and tells the presenters the shape of the gameplay picture it is drawing
 * (recomp_wide_frame); menus, the pause screen and anything else stay 16:9. RECOMP_ASPECT = "21:9", "2.37", or "auto"
 * (the window's shape, from what the presenters report; before the first frame the monitor's). Shapes at or under 16:9 mean
 * off (a taller window never narrows the view); the widest is 32:9. */
static float s_wide_fixed = -1.0f;                  /* -1 not read, 0 none, > 0 fixed */
static int s_wide_auto;
static volatile float s_client_asp;                 /* the window's client shape, as the presenters last saw it */
static volatile float s_frame_asp;
static volatile DWORD s_frame_tick;
static int (*volatile s_wide_gate)(void);           /* 0 = no wide picture now (a menu is up) */

#define WIDE_MIN_ASPECT 1.80f
#define WIDE_MAX_ASPECT 3.60f

static void wide_read(void)
{
    const char *e = getenv("RECOMP_ASPECT");
    float a = 0.0f, w, h;
    s_wide_fixed = 0.0f;
    if (!e || !*e)
        return;
    if (!_stricmp(e, "auto"))
        s_wide_auto = 1;
    else if (sscanf(e, "%f:%f", &w, &h) == 2 && h > 0.0f)
        a = w / h;
    else
        a = (float)atof(e);
    s_wide_fixed = a < WIDE_MIN_ASPECT ? 0.0f : a > WIDE_MAX_ASPECT ? WIDE_MAX_ASPECT : a;
}

/* The configured gameplay shape: 0 = plain 16:9. */
float recomp_wide_target(void)
{
    float a;
    if (s_wide_fixed < 0.0f)
        wide_read();
    if (!s_wide_auto)
        return s_wide_fixed;
    a = s_client_asp;
    if (a <= 0.0f) {
        int mw = GetSystemMetrics(SM_CXSCREEN), mh = GetSystemMetrics(SM_CYSCREEN);
        a = mh > 0 ? (float)mw / (float)mh : 0.0f;
    }
    return a < WIDE_MIN_ASPECT ? 0.0f : a > WIDE_MAX_ASPECT ? WIDE_MAX_ASPECT : a;
}

void recomp_wide_set_gate(int (*gate)(void)) { s_wide_gate = gate; }

/* The game side: the gameplay picture being drawn is this shape (called every view update; 0 = normal). */
void recomp_wide_frame(float asp)
{
    s_frame_asp = asp;
    s_frame_tick = GetTickCount();
}

/* Presenters: the shape to show the frame at (dflt = the shape it has without ultrawide). */
float recomp_present_aspect(float dflt)
{
    float a = s_frame_asp;
    int (*g)(void) = s_wide_gate;
    if (a > 0.0f && GetTickCount() - s_frame_tick < 250u && (!g || g()))
        return a;
    return dflt;
}

/* The factor the 2D HUD is squeezed in x by (the shader also moves each element to its own edge: left ones to the left edge, right
 * ones to the right edge, the middle ones stay in the middle): the game's own 16:9 HUD shape over the picture's shape. 0 = leave the
 * HUD alone (no gameplay picture now, or the picture is not wider than 16:9). */
float recomp_wide_hud_k(void)
{
    float a = recomp_present_aspect(0.0f);
    return a > 16.0f / 9.0f + 0.01f ? (16.0f / 9.0f) / a : 0.0f;
}

/* Presenters report the window's client size (the shape "auto" follows). */
void recomp_wide_client(unsigned cw, unsigned ch)
{
    if (cw && ch)
        s_client_asp = (float)cw / (float)ch;
}

static void scale_init(void)
{
    const char *r = getenv("RECOMP_GPU_RES");
    const char *e = getenv("RECOMP_GPU_SCALE");
    unsigned rw = 0, rh = 0;
    if (r && sscanf(r, "%ux%u", &rw, &rh) == 2 && rw >= 640 && rh >= 480
        && rw <= 640 * 8 && rh <= 480 * 8) {
        float wt = recomp_wide_target();
        if (wt > 0.0f && fabsf((float)rw / (float)rh - 16.0f / 9.0f) < 0.05f) {
            /* a 16:9 resolution with ultrawide on: same height, the width of the wider picture (the surface is the
             * gameplay picture; menus show it squeezed into 16:9) */
            unsigned nw = (unsigned)((float)rh * wt + 0.5f);
            rw = nw > 640 * 8 ? 640 * 8 : nw;
        }
        s_sx = (float)rw / 640.0f;
        s_sy = (float)rh / 480.0f;
    } else {
        int v = e ? atoi(e) : 1;
        s_sx = s_sy = (float)(v < 1 ? 1 : v > 4 ? 4 : v);
    }
    s_scaled = s_sx != 1.0f || s_sy != 1.0f;
    if (s_scaled)
        fprintf(stderr, "[NV2A-GPU] drawing at %.3gx%.3g the guest resolution"
                        " (%ux%u for 640x480)\n", s_sx, s_sy, SC_W(640), SC_H(480));
}

int nv2a_gpu_enabled(void)
{
    if (s_on < 0) {
        const char *e = getenv("RECOMP_GPU");
        s_on = (e && *e && *e != '0') ? gpu_init() : 0;
    }
    return s_on;
}

/* GPU time per frame, from timestamp queries read a few frames late (never
 * waited on). RECOMP_GPU_STATS only. */
#define TQ_RING 4
static struct { ID3D11Query *dis, *t0, *t1; int busy; } s_tq[TQ_RING];
static int s_tq_cur = -1;

static void gpu_time_frame(void)
{
    int i;
    /* Close the frame being timed. */
    if (s_tq_cur >= 0) {
        ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)s_tq[s_tq_cur].t1);
        ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)s_tq[s_tq_cur].dis);
        s_tq[s_tq_cur].busy = 1;
    }
    /* Collect whatever finished. */
    for (i = 0; i < TQ_RING; i++) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 a, b;
        if (!s_tq[i].busy)
            continue;
        if (ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)s_tq[i].dis, &dj,
                                        sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        if (ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)s_tq[i].t0, &a,
                                        sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK
            && ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)s_tq[i].t1, &b,
                                           sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK
            && !dj.Disjoint && dj.Frequency) {
            s_st.gpu_ms += (double)(b - a) * 1000.0 / (double)dj.Frequency;
            s_st.gpu_frames++;
        }
        s_tq[i].busy = 0;
    }
    /* Open the next one in a free slot. */
    s_tq_cur = -1;
    for (i = 0; i < TQ_RING; i++)
        if (!s_tq[i].busy) {
            if (!s_tq[i].dis) {
                D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
                D3D11_QUERY_DESC qt = { D3D11_QUERY_TIMESTAMP, 0 };
                ID3D11Device_CreateQuery(s_dev, &qd, &s_tq[i].dis);
                ID3D11Device_CreateQuery(s_dev, &qt, &s_tq[i].t0);
                ID3D11Device_CreateQuery(s_dev, &qt, &s_tq[i].t1);
            }
            if (!s_tq[i].dis || !s_tq[i].t0 || !s_tq[i].t1)
                return;
            ID3D11DeviceContext_Begin(s_ctx, (ID3D11Asynchronous *)s_tq[i].dis);
            ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)s_tq[i].t0);
            s_tq_cur = i;
            return;
        }
}

extern uint64_t g_pb_walk_ns;          /* nv2a_pb_scan.c */
static void prof_report(void);         /* RECOMP_GPU_PROF, below */

static void wpn_tick(void);                    /* the weapon lighting block, after the blit code */
void nv2a_gpu_frame(void)
{
    static int stats = -1;
    static double t0;
    if (stats < 0)
        stats = getenv("RECOMP_GPU_STATS") != NULL;
    if (stats && s_on > 0)
        gpu_time_frame();
    if (stats && s_frame % 60 == 0) {
        double t = now_ms();
        fprintf(stderr, "[NV2A-GPU] 60 frames in %.0f ms: %u draws,"
                " %u readbacks %.1f ms, %u uploads %.1f ms (new %u stale %u"
                " cpu %u), %u textures, %u surface samples (%u copied);"
                " map wait %.1f ms, GPU %.2f ms/frame, walk %.2f ms/frame\n",
                t0 ? t - t0 : 0.0, s_st.draws, s_st.readbacks,
                s_st.readback_ms, s_st.uploads, s_st.upload_ms, s_st.up_new,
                s_st.up_stale, s_st.up_hash, s_st.textures, s_st.rtt,
                s_st.rtt_copies, s_st.map_ms,
                s_st.gpu_frames ? s_st.gpu_ms / s_st.gpu_frames : 0.0,
                g_pb_walk_ns / 60.0 / 1e6);
        {
            extern void nv2a_pb_exec_prof_report(double, double);
            extern void nv2a_pb_flush_prof_report(void);
            static double prof_ms = -1.0;
            if (prof_ms < 0.0) {
                const char *e = getenv("RECOMP_EXEC_PROF_MS");
                prof_ms = e ? atof(e) : 12.0;
            }
            nv2a_pb_exec_prof_report(g_pb_walk_ns / 60.0 / 1e6, prof_ms);
            extern void nv2a_pb_gather_prof_report(void);
            nv2a_pb_gather_prof_report();
            nv2a_pb_flush_prof_report();
        }
        g_pb_walk_ns = 0;
        prof_report();
        memset(&s_st, 0, sizeof s_st);
        t0 = t;
    }
    wpn_tick();
    if (++s_frame == 0)
        s_frame = 1;
}

/* ---- guest memory helpers ----------------------------------------------- */

static uint8_t *guest(uint32_t va)
{
    return (uint8_t *)xbox_GetMemoryOffset() + va;
}

static uint64_t hash_bytes(const uint8_t *p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    size_t i;
    for (i = 0; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static uint32_t from565(uint16_t v)
{
    uint32_t r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
    return 0xFF000000u | (((r << 3) | (r >> 2)) << 16)
         | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
}

/* ---- surfaces ----------------------------------------------------------- */

typedef struct {
    int used;
    Nv2aGpuSurface d;
    ID3D11Texture2D *tex, *staging;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;   /* made on first use as a texture */
    int dirty;              /* the GPU has writes the guest copy lacks */
    int stale;              /* the guest copy has writes the GPU lacks */
    uint64_t hash;          /* of the guest bytes when last in step */
    uint32_t checked;       /* s_frame of the last hash check */
} GpuSurf;

#define GPU_SURFS 32
static GpuSurf s_surf[GPU_SURFS];

static uint32_t surf_lo(const Nv2aGpuSurface *d) { return d->va & 0x0FFFFFFFu; }
static uint32_t surf_bytes(const Nv2aGpuSurface *d)
{
    return d->swizzled ? d->width * d->height * d->bpp : d->pitch * d->height;
}
static int overlaps(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    return a < b + bn && b < a + an;
}

/* Guest-size textures, one per size: a scaled surface's upload lands here
 * and is enlarged into the surface, its read-back is averaged down here. */
static struct {
    UINT w, h;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    ID3D11RenderTargetView *rtv;
} s_one[8];

static int one_get(UINT w, UINT h)
{
    int i, k = -1;
    for (i = 0; i < 8; i++) {
        if (s_one[i].tex && s_one[i].w == w && s_one[i].h == h)
            return i;
        if (k < 0 && !s_one[i].tex)
            k = i;
    }
    if (k < 0) {
        k = 0;
        ID3D11ShaderResourceView_Release(s_one[0].srv);
        ID3D11RenderTargetView_Release(s_one[0].rtv);
        ID3D11Texture2D_Release(s_one[0].tex);
        memset(&s_one[0], 0, sizeof s_one[0]);
    }
    {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_one[k].tex)))
            return -1;
        if (FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_one[k].tex,
                                                         NULL, &s_one[k].srv))
            || FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s_one[k].tex,
                                                          NULL, &s_one[k].rtv))) {
            if (s_one[k].srv) ID3D11ShaderResourceView_Release(s_one[k].srv);
            ID3D11Texture2D_Release(s_one[k].tex);
            memset(&s_one[k], 0, sizeof s_one[k]);
            return -1;
        }
        s_one[k].w = w;
        s_one[k].h = h;
    }
    return k;
}

static void surf_upload(GpuSurf *s)
{
    const Nv2aGpuSurface *d = &s->d;
    const uint8_t *src = guest(d->va);
    uint32_t w = d->width, h = d->height, x, y;
    double t0 = now_ms();

    ID3D11Resource *dst = (ID3D11Resource *)s->tex;
    int one = -1;

    s_st.uploads++;
    if (s_scaled) {
        one = one_get(w, h);
        if (one < 0)
            return;
        dst = (ID3D11Resource *)s_one[one].tex;
    }
    if (!d->swizzled && d->bpp == 4) {
        ID3D11DeviceContext_UpdateSubresource(s_ctx, dst, 0, NULL, src, d->pitch, 0);
    } else {
        uint32_t *tmp = (uint32_t *)malloc((size_t)w * h * 4);
        if (!tmp)
            return;
        if (d->swizzled) {
            uint32_t mx, my, oy = 0;
            xbox_swizzle_masks(w, h, &mx, &my);
            for (y = 0; y < h; y++) {
                uint32_t ox = 0;
                for (x = 0; x < w; x++) {
                    uint32_t i = oy | ox;
                    tmp[(size_t)y * w + x] = d->bpp == 4
                        ? ((const uint32_t *)src)[i]
                        : from565(((const uint16_t *)src)[i]);
                    ox = (ox - mx) & mx;
                }
                oy = (oy - my) & my;
            }
        } else {
            for (y = 0; y < h; y++)
                for (x = 0; x < w; x++)
                    tmp[(size_t)y * w + x] =
                        from565(((const uint16_t *)(src + (size_t)y * d->pitch))[x]);
        }
        ID3D11DeviceContext_UpdateSubresource(s_ctx, dst, 0, NULL, tmp, w * 4, 0);
        free(tmp);
    }
    if (one >= 0)                       /* enlarged, each pixel a block */
        scale_blit(s_one[one].srv, s->rtv, SC_W(w), SC_H(h), 0);
    s->hash = hash_bytes(src, surf_bytes(d));
    s->stale = 0;
    s_st.upload_ms += now_ms() - t0;
}

static void surf_readback(GpuSurf *s)
{
    const Nv2aGpuSurface *d = &s->d;
    D3D11_MAPPED_SUBRESOURCE m;
    uint8_t *dst = guest(d->va);
    uint32_t w = d->width, h = d->height, x, y;
    double t0;

    if (!s->dirty)
        return;
    t0 = now_ms();
    s_st.readbacks++;
    if (!s->staging) {
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D_GetDesc(s->tex, &td);
        td.Width = w;                   /* guest size, whatever the scale */
        td.Height = h;
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s->staging)))
            return;
    }
    if (s_scaled) {
        int one = one_get(w, h);
        if (one < 0)
            return;
        if (!s->srv && FAILED(ID3D11Device_CreateShaderResourceView(
                                   s_dev, (ID3D11Resource *)s->tex, NULL, &s->srv)))
            return;
        scale_blit(s->srv, s_one[one].rtv, w, h, 1);
        ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s->staging,
                                         (ID3D11Resource *)s_one[one].tex);
    } else
        ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s->staging,
                                         (ID3D11Resource *)s->tex);
    {
        double tm = now_ms();
        HRESULT hr = ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s->staging, 0,
                                             D3D11_MAP_READ, 0, &m);
        s_st.map_ms += now_ms() - tm;
        if (FAILED(hr))
            return;
    }
    if (d->swizzled) {
        uint32_t mx, my, oy = 0;
        xbox_swizzle_masks(w, h, &mx, &my);
        for (y = 0; y < h; y++) {
            const uint32_t *row = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
            uint32_t ox = 0;
            for (x = 0; x < w; x++) {
                uint32_t i = oy | ox, c = row[x];
                if (d->bpp == 4)
                    ((uint32_t *)dst)[i] = c;
                else
                    ((uint16_t *)dst)[i] = (uint16_t)(((c >> 8) & 0xF800)
                        | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
                ox = (ox - mx) & mx;
            }
            oy = (oy - my) & my;
        }
    } else {
        for (y = 0; y < h; y++) {
            const uint32_t *row = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
            uint8_t *out = dst + (size_t)y * d->pitch;
            if (d->bpp == 4) {
                memcpy(out, row, (size_t)w * 4);
            } else {
                for (x = 0; x < w; x++) {
                    uint32_t c = row[x];
                    ((uint16_t *)out)[x] = (uint16_t)(((c >> 8) & 0xF800)
                        | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
                }
            }
        }
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s->staging, 0);
    s->dirty = 0;
    s->hash = hash_bytes(dst, surf_bytes(d));
    s->checked = s_frame;
    s_st.readback_ms += now_ms() - t0;
}

static void surf_free(GpuSurf *s)
{
    if (s->srv) ID3D11ShaderResourceView_Release(s->srv);
    if (s->rtv) ID3D11RenderTargetView_Release(s->rtv);
    if (s->tex) ID3D11Texture2D_Release(s->tex);
    if (s->staging) ID3D11Texture2D_Release(s->staging);
    memset(s, 0, sizeof *s);
}

/* The GPU copy of a surface, current with the guest copy. */
static GpuSurf *surf_get(const Nv2aGpuSurface *d)
{
    GpuSurf *s = NULL, *freeslot = NULL;
    int i;

    for (i = 0; i < GPU_SURFS; i++) {
        GpuSurf *c = &s_surf[i];
        if (!c->used) {
            if (!freeslot) freeslot = c;
            continue;
        }
        /* A linear surface's height is the clip's bottom, which a title
         * can lower for one pass: the taller copy serves. And one image can
         * be drawn swizzled in one pass and linear in the next (MoH: European
         * Assault's 128x128 targets): the pixels are the same, only their
         * order in guest memory differs, so it is one surface, written back
         * in the layout last drawn in. Two copies thrashed -- read back,
         * freed and re-uploaded thirty times a frame. */
        if (c->d.va == d->va && c->d.pitch == d->pitch && c->d.bpp == d->bpp
            && c->d.width == d->width && c->d.height >= d->height) {
            s = c;
            if (c->d.swizzled != d->swizzled) {
                c->d.swizzled = d->swizzled;
                /* The GPU copy is the image; the guest copy is in the old
                 * order now, so the next sync must write it out again. */
                c->dirty = 1;
            }
            break;
        }
    }
    if (s) {
        if (s->stale) {
            s_st.up_stale++;
            surf_upload(s);
        } else if (!s->dirty && s->checked != s_frame) {
            /* Once a frame: has the CPU written it since it was in step? */
            if (hash_bytes(guest(d->va), surf_bytes(d)) != s->hash) {
                s_st.up_hash++;
                surf_upload(s);
            }
            s->checked = s_frame;
        }
        return s;
    }
    /* A new shape: whatever else covered these bytes goes, written back
     * first if it held the only copy of something. */
    for (i = 0; i < GPU_SURFS; i++) {
        GpuSurf *c = &s_surf[i];
        if (c->used && overlaps(surf_lo(&c->d), surf_bytes(&c->d),
                                surf_lo(d), surf_bytes(d))) {
            static int shown;
            if (shown < 40 && getenv("RECOMP_GPU_STATS")) {
                shown++;
                fprintf(stderr, "[NV2A-GPU] surface %08X %ux%u bpp %u pitch %u%s"
                        " replaces %08X %ux%u bpp %u pitch %u%s (dirty %d)\n",
                        d->va, d->width, d->height, d->bpp, d->pitch,
                        d->swizzled ? " swz" : "", c->d.va, c->d.width,
                        c->d.height, c->d.bpp, c->d.pitch,
                        c->d.swizzled ? " swz" : "", c->dirty);
            }
            surf_readback(c);
            surf_free(c);
            if (!freeslot) freeslot = c;
        }
    }
    if (!freeslot) {
        /* Full: drop the first one not holding unsynced pixels. */
        for (i = 0; i < GPU_SURFS && !freeslot; i++) {
            surf_readback(&s_surf[i]);
            surf_free(&s_surf[i]);
            freeslot = &s_surf[i];
        }
    }
    s = freeslot;
    {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = SC_W(d->width);
        td.Height = SC_H(d->height);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s->tex)))
            return NULL;
        if (FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s->tex,
                                                       NULL, &s->rtv))) {
            ID3D11Texture2D_Release(s->tex);
            s->tex = NULL;
            return NULL;
        }
    }
    s->used = 1;
    s->d = *d;
    s->dirty = 0;
    s_st.up_new++;
    surf_upload(s);
    s->checked = s_frame;
    return s;
}

/* The surface a texture can be sampled from, if the GPU holds one: same
 * address, texel size and layout, and the texture inside it. Not one the
 * CPU has written since (stale): that one's pixels are in guest memory. */
static GpuSurf *surf_for_texture(uint32_t va, uint32_t bpp, uint32_t w,
                                 uint32_t h, uint32_t pitch, int swz)
{
    int i;
    va &= 0x0FFFFFFFu;
    for (i = 0; i < GPU_SURFS; i++) {
        GpuSurf *c = &s_surf[i];
        if (!c->used || c->stale || surf_lo(&c->d) != va || c->d.bpp != bpp)
            continue;
        if (swz ? (c->d.width == w && c->d.height == h)
                : (c->d.pitch == pitch && w <= c->d.width && h <= c->d.height))
            return c;
    }
    return NULL;
}

int nv2a_gpu_surface_texture(uint32_t va, uint32_t bpp, uint32_t w, uint32_t h,
                             uint32_t pitch, int swizzled)
{
    static int no_rtt = -1;
    if (no_rtt < 0)
        no_rtt = getenv("RECOMP_GPU_NO_RTT") != NULL;
    if (s_on <= 0 || no_rtt)
        return 0;
    return surf_for_texture(va, bpp, w, h, pitch, swizzled) != NULL;
}

/* A copy of the surface being drawn into, for a batch that samples its own
 * target (the GPU cannot read and write one texture in one draw). One per
 * size, reused. */
static struct { uint32_t w, h; ID3D11Texture2D *tex; ID3D11ShaderResourceView *srv; } s_scratch[8];

static ID3D11ShaderResourceView *scratch_copy(GpuSurf *s)
{
    int i, slot = -1;
    for (i = 0; i < 8; i++) {
        if (s_scratch[i].tex && s_scratch[i].w == s->d.width && s_scratch[i].h == s->d.height) {
            slot = i;
            break;
        }
        if (slot < 0 && !s_scratch[i].tex)
            slot = i;
    }
    if (slot < 0)
        slot = 0;
    if (!s_scratch[slot].tex || s_scratch[slot].w != s->d.width
        || s_scratch[slot].h != s->d.height) {
        D3D11_TEXTURE2D_DESC td;
        if (s_scratch[slot].srv) ID3D11ShaderResourceView_Release(s_scratch[slot].srv);
        if (s_scratch[slot].tex) ID3D11Texture2D_Release(s_scratch[slot].tex);
        memset(&s_scratch[slot], 0, sizeof s_scratch[slot]);
        ID3D11Texture2D_GetDesc(s->tex, &td);
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_scratch[slot].tex)))
            return NULL;
        if (FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_scratch[slot].tex,
                                                         NULL, &s_scratch[slot].srv)))
            return NULL;
        s_scratch[slot].w = s->d.width;
        s_scratch[slot].h = s->d.height;
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s_scratch[slot].tex,
                                     (ID3D11Resource *)s->tex);
    s_st.rtt_copies++;
    return s_scratch[slot].srv;
}

void nv2a_gpu_sync_range(uint32_t va, uint32_t bytes)
{
    uint32_t lo = va & 0x0FFFFFFFu;
    int i;
    if (s_on <= 0)
        return;
    for (i = 0; i < GPU_SURFS; i++) {
        GpuSurf *c = &s_surf[i];
        if (c->used && c->dirty
            && overlaps(surf_lo(&c->d), surf_bytes(&c->d), lo, bytes))
            surf_readback(c);
    }
}

void nv2a_gpu_cpu_write(uint32_t va, uint32_t bytes, int after)
{
    uint32_t lo = va & 0x0FFFFFFFu;
    int i;
    if (s_on <= 0)
        return;
    if (!after) {
        nv2a_gpu_sync_range(va, bytes);
        return;
    }
    for (i = 0; i < GPU_SURFS; i++) {
        GpuSurf *c = &s_surf[i];
        if (c->used && overlaps(surf_lo(&c->d), surf_bytes(&c->d), lo, bytes))
            c->stale = 1;
    }
}

/* ---- depth buffers ------------------------------------------------------ */

typedef struct {
    int used;
    uint32_t va, w, h, last;
    ID3D11Texture2D *tex;
    ID3D11DepthStencilView *dsv;
} GpuDepth;

#define GPU_DEPTHS 16
static GpuDepth s_depth[GPU_DEPTHS];
static uint32_t s_depth_tick;

static GpuDepth *depth_get(uint32_t va, uint32_t w, uint32_t h)
{
    GpuDepth *z = NULL;
    int i;
    va &= 0x0FFFFFFFu;
    for (i = 0; i < GPU_DEPTHS; i++)
        if (s_depth[i].used && s_depth[i].va == va && s_depth[i].w == w
            && s_depth[i].h == h) {
            s_depth[i].last = ++s_depth_tick;
            return &s_depth[i];
        }
    for (i = 0; i < GPU_DEPTHS; i++)
        if (!s_depth[i].used || !z || s_depth[i].last < z->last) {
            z = &s_depth[i];
            if (!z->used)
                break;
        }
    if (z->used) {
        ID3D11DepthStencilView_Release(z->dsv);
        ID3D11Texture2D_Release(z->tex);
        memset(z, 0, sizeof *z);
    }
    {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = SC_W(w);
        td.Height = SC_H(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &z->tex)))
            return NULL;
        if (FAILED(ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)z->tex,
                                                       NULL, &z->dsv))) {
            ID3D11Texture2D_Release(z->tex);
            z->tex = NULL;
            return NULL;
        }
    }
    /* The software buffer starts at "infinitely far". */
    ID3D11DeviceContext_ClearDepthStencilView(s_ctx, z->dsv,
                                              D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                              1.0f, 0);
    z->used = 1;
    z->va = va;
    z->w = w;
    z->h = h;
    z->last = ++s_depth_tick;
    return z;
}

/* ---- textures ----------------------------------------------------------- */

typedef struct {
    uint32_t version, w, h, levels;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
} GpuTex;

#define GPU_TEX_SLOTS 1024     /* >= TEXDEC_SLOTS (nv2a_pb_exec.c) */
static GpuTex s_tex[GPU_TEX_SLOTS];

static ID3D11ShaderResourceView *tex_get(const Nv2aGpuTexture *t)
{
    GpuTex *g;
    if (!t->valid || t->slot >= GPU_TEX_SLOTS || !t->argb)
        return NULL;
    g = &s_tex[t->slot];
    if (g->srv && g->version == t->version && g->w == t->lv_w[0]
        && g->h == t->lv_h[0] && g->levels == t->levels)
        return g->srv;
    if (g->srv) ID3D11ShaderResourceView_Release(g->srv);
    if (g->tex) ID3D11Texture2D_Release(g->tex);
    memset(g, 0, sizeof *g);
    s_st.textures++;
    {
        D3D11_TEXTURE2D_DESC td;
        D3D11_SUBRESOURCE_DATA init[16];
        uint32_t lv, n = t->levels ? t->levels : 1;
        if (n > 16) n = 16;
        memset(&td, 0, sizeof td);
        td.Width = t->lv_w[0];
        td.Height = t->lv_h[0];
        td.MipLevels = n;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        for (lv = 0; lv < n; lv++) {
            init[lv].pSysMem = t->argb + t->lv_off[lv];
            init[lv].SysMemPitch = t->lv_w[lv] * 4;
            init[lv].SysMemSlicePitch = 0;
        }
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, init, &g->tex)))
            return NULL;
        if (FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)g->tex,
                                                         NULL, &g->srv))) {
            ID3D11Texture2D_Release(g->tex);
            g->tex = NULL;
            return NULL;
        }
        g->version = t->version;
        g->w = t->lv_w[0];
        g->h = t->lv_h[0];
        g->levels = n;
    }
    return g->srv;
}

/* ---- state object caches ------------------------------------------------ */

#define STATE_CACHE 64

/* Set live by the title's settings screen: anisotropic filtering level (1 = off, 2..16) for the
 * game's linear-filtered textures, and FXAA on the frame as it is presented. */
volatile int g_gpu_aniso = 1;
volatile int g_gpu_fxaa = 0;

static struct { D3D11_SAMPLER_DESC d; ID3D11SamplerState *o; } s_samp[STATE_CACHE];
static struct { D3D11_BLEND_DESC d; ID3D11BlendState *o; } s_blend[STATE_CACHE];
static struct { D3D11_DEPTH_STENCIL_DESC d; ID3D11DepthStencilState *o; } s_dss[STATE_CACHE];
static int s_nsamp, s_nblend, s_ndss;

static ID3D11SamplerState *sampler_get(const D3D11_SAMPLER_DESC *d)
{
    int i;
    for (i = 0; i < s_nsamp; i++)
        if (!memcmp(&s_samp[i].d, d, sizeof *d))
            return s_samp[i].o;
    i = s_nsamp < STATE_CACHE ? s_nsamp++ : STATE_CACHE - 1;
    if (s_samp[i].o) ID3D11SamplerState_Release(s_samp[i].o);
    s_samp[i].d = *d;
    s_samp[i].o = NULL;
    ID3D11Device_CreateSamplerState(s_dev, d, &s_samp[i].o);
    return s_samp[i].o;
}

static ID3D11BlendState *blend_get(const D3D11_BLEND_DESC *d)
{
    int i;
    for (i = 0; i < s_nblend; i++)
        if (!memcmp(&s_blend[i].d, d, sizeof *d))
            return s_blend[i].o;
    i = s_nblend < STATE_CACHE ? s_nblend++ : STATE_CACHE - 1;
    if (s_blend[i].o) ID3D11BlendState_Release(s_blend[i].o);
    s_blend[i].d = *d;
    s_blend[i].o = NULL;
    ID3D11Device_CreateBlendState(s_dev, d, &s_blend[i].o);
    return s_blend[i].o;
}

static ID3D11DepthStencilState *dss_get(const D3D11_DEPTH_STENCIL_DESC *d)
{
    int i;
    for (i = 0; i < s_ndss; i++)
        if (!memcmp(&s_dss[i].d, d, sizeof *d))
            return s_dss[i].o;
    i = s_ndss < STATE_CACHE ? s_ndss++ : STATE_CACHE - 1;
    if (s_dss[i].o) ID3D11DepthStencilState_Release(s_dss[i].o);
    s_dss[i].d = *d;
    s_dss[i].o = NULL;
    ID3D11Device_CreateDepthStencilState(s_dev, d, &s_dss[i].o);
    return s_dss[i].o;
}

static D3D11_TEXTURE_ADDRESS_MODE addr_mode(uint32_t m, int border)
{
    switch (m) {
    case 1:  return D3D11_TEXTURE_ADDRESS_WRAP;
    case 2:  return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 4:  return border ? D3D11_TEXTURE_ADDRESS_BORDER : D3D11_TEXTURE_ADDRESS_CLAMP;
    default: return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
}

static D3D11_BLEND blend_factor(uint32_t f, int alpha)
{
    switch (f) {
    case 0x0000: return D3D11_BLEND_ZERO;
    case 0x0001: return D3D11_BLEND_ONE;
    case 0x0300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x0301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D11_BLEND_SRC_ALPHA;
    case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D11_BLEND_DEST_ALPHA;
    case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x0306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x0307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default:     return f ? D3D11_BLEND_ONE : D3D11_BLEND_ZERO;
    }
}

static D3D11_BLEND_OP blend_op(uint32_t eq)
{
    switch (eq) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default:     return D3D11_BLEND_OP_ADD;
    }
}

static D3D11_COMPARISON_FUNC cmp_func(uint32_t f)
{
    /* GL_NEVER 0x200 .. GL_ALWAYS 0x207 are D3D11's 1..8 in the same order. */
    return (f >= 0x200 && f <= 0x207) ? (D3D11_COMPARISON_FUNC)(f - 0x200 + 1)
                                      : D3D11_COMPARISON_ALWAYS;
}

static D3D11_STENCIL_OP stencil_op(uint32_t op)
{
    switch (op) {
    case 0x0000: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default:     return D3D11_STENCIL_OP_KEEP;
    }
}

/* ---- occlusion (GET_REPORT) --------------------------------------------- */

#define GPU_QUERIES 64
static ID3D11Query *s_q_free[GPU_QUERIES], *s_q_pend[GPU_QUERIES], *s_q_active;
static int s_nq_free, s_nq_pend;

static void zpass_set(int on)
{
    if (on && !s_q_active) {
        if (s_nq_free) {
            s_q_active = s_q_free[--s_nq_free];
        } else {
            D3D11_QUERY_DESC qd = { D3D11_QUERY_OCCLUSION, 0 };
            if (FAILED(ID3D11Device_CreateQuery(s_dev, &qd, &s_q_active)))
                s_q_active = NULL;
        }
        if (s_q_active)
            ID3D11DeviceContext_Begin(s_ctx, (ID3D11Asynchronous *)s_q_active);
    } else if (!on && s_q_active) {
        ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)s_q_active);
        if (s_nq_pend < GPU_QUERIES)
            s_q_pend[s_nq_pend++] = s_q_active;
        else
            ID3D11Query_Release(s_q_active);
        s_q_active = NULL;
    }
}

uint32_t nv2a_gpu_zpass_collect(void)
{
    uint64_t total = 0;
    int i;
    if (s_on <= 0)
        return 0;
    zpass_set(0);
    for (i = 0; i < s_nq_pend; i++) {
        UINT64 n = 0;
        while (ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)s_q_pend[i],
                                           &n, sizeof n, 0) == S_FALSE)
            ;
        total += n;
        if (s_nq_free < GPU_QUERIES)
            s_q_free[s_nq_free++] = s_q_pend[i];
        else
            ID3D11Query_Release(s_q_pend[i]);
    }
    s_nq_pend = 0;
    total = (uint64_t)((double)total / ((double)s_sx * s_sy)); /* in guest pixels */
    return total > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)total;
}

/* ---- drawing ------------------------------------------------------------ */

/* What the pipeline holds, so a draw sets only what changed.
 *
 * Every draw used to set every piece of state again -- target, viewport,
 * scissor, four textures and samplers, blend, depth, the input stage, both
 * shaders -- and map both constant buffers, the pixel one ~3 KB, with
 * DISCARD. The driver's share of that was 13% of the executor, with most
 * draws changing almost nothing from the one before. Comparing pointers is
 * safe: an object the context has bound is referenced by it, so its address
 * cannot be reused while the cache still names it. bind_forget() is for
 * code that binds anything else (the presenter), and binding a new target
 * forgets the textures, since the runtime unbinds a resource's views from
 * the texture slots when it becomes a target. */
static struct {
    int target, srvs, pipe, blend, dss, vp, sc;
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    UINT vp_w, vp_h;
    D3D11_RECT rect;
    ID3D11ShaderResourceView *srv[4];
    ID3D11SamplerState *smp[4];
    ID3D11BlendState *bs;
    float bf[4];
    ID3D11DepthStencilState *ds;
    UINT sref;
    ID3D11PixelShader *ps;
} s_bind;
/* The constant buffers' contents: they keep them whatever is bound. */
static int s_vc_ok, s_pc_ok;

static void bind_forget(void)
{
    memset(&s_bind, 0, sizeof s_bind);
}

/* RECOMP_GPU_CHECK_BIND: before a draw, what the context really holds
 * against what s_bind says it does. A difference is state the runtime
 * changed behind the cache, which would draw with the wrong state. */
static void bind_check(void)
{
    static unsigned bad, checks;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11DepthStencilView *dsv = NULL;
    ID3D11ShaderResourceView *srv[4] = { 0 };
    ID3D11SamplerState *smp[4] = { 0 };
    ID3D11BlendState *bs = NULL;
    ID3D11DepthStencilState *ds = NULL;
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    ID3D11RasterizerState *rs = NULL;
    float bf[4];
    UINT mask, ref, nvp = 1, nsc = 1;
    D3D11_VIEWPORT vp;
    D3D11_RECT sc;
    char what[200] = "";
    int i;

    ID3D11DeviceContext_OMGetRenderTargets(s_ctx, 1, &rtv, &dsv);
    if (rtv != s_bind.rtv) strcat(what, " target");
    if (dsv != s_bind.dsv) strcat(what, " depth-target");
    ID3D11DeviceContext_PSGetShaderResources(s_ctx, 0, 4, srv);
    ID3D11DeviceContext_PSGetSamplers(s_ctx, 0, 4, smp);
    for (i = 0; i < 4; i++) {
        if (srv[i] != s_bind.srv[i]) sprintf(what + strlen(what), " tex%d", i);
        if (s_bind.srv[i] && smp[i] != s_bind.smp[i])
            sprintf(what + strlen(what), " sampler%d", i);
    }
    ID3D11DeviceContext_OMGetBlendState(s_ctx, &bs, bf, &mask);
    if (bs != s_bind.bs || memcmp(bf, s_bind.bf, sizeof bf)) strcat(what, " blend");
    ID3D11DeviceContext_OMGetDepthStencilState(s_ctx, &ds, &ref);
    if (ds != s_bind.ds || ref != s_bind.sref) strcat(what, " depth");
    ID3D11DeviceContext_VSGetShader(s_ctx, &vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(s_ctx, &ps, NULL, NULL);
    ID3D11DeviceContext_RSGetState(s_ctx, &rs);
    if (vs != (s_bind.pipe == 2 ? s_vs_prog : s_vs) || ps != s_bind.ps || rs != s_rs)
        strcat(what, " pipeline");
    ID3D11DeviceContext_RSGetViewports(s_ctx, &nvp, &vp);
    if (nvp != 1 || vp.Width != (float)s_bind.vp_w || vp.Height != (float)s_bind.vp_h)
        strcat(what, " viewport");
    ID3D11DeviceContext_RSGetScissorRects(s_ctx, &nsc, &sc);
    if (nsc != 1 || memcmp(&sc, &s_bind.rect, sizeof sc)) strcat(what, " scissor");
    if (rtv) ID3D11RenderTargetView_Release(rtv);
    if (dsv) ID3D11DepthStencilView_Release(dsv);
    for (i = 0; i < 4; i++) {
        if (srv[i]) ID3D11ShaderResourceView_Release(srv[i]);
        if (smp[i]) ID3D11SamplerState_Release(smp[i]);
    }
    if (bs) ID3D11BlendState_Release(bs);
    if (ds) ID3D11DepthStencilState_Release(ds);
    if (vs) ID3D11VertexShader_Release(vs);
    if (ps) ID3D11PixelShader_Release(ps);
    if (rs) ID3D11RasterizerState_Release(rs);
    checks++;
    if (what[0] && bad++ < 30)
        fprintf(stderr, "[CHECK-BIND] draw %u:%s\n", checks, what);
    if (checks % 50000 == 0)
        fprintf(stderr, "[CHECK-BIND] %u draws checked, %u differ\n", checks, bad);
}

static void set_target(GpuSurf *s, GpuDepth *z)
{
    ID3D11DepthStencilView *dsv = z ? z->dsv : NULL;
    if (!s_bind.target || s_bind.rtv != s->rtv || s_bind.dsv != dsv) {
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s->rtv, dsv);
        s_bind.target = 1;
        s_bind.rtv = s->rtv;
        s_bind.dsv = dsv;
        s_bind.srvs = 0;
    }
    if (!s_bind.vp || s_bind.vp_w != SC_W(s->d.width)
        || s_bind.vp_h != SC_H(s->d.height)) {
        D3D11_VIEWPORT vp;
        vp.TopLeftX = 0;
        vp.TopLeftY = 0;
        vp.Width = (float)SC_W(s->d.width);
        vp.Height = (float)SC_H(s->d.height);
        vp.MinDepth = 0;
        vp.MaxDepth = 1;
        ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
        s_bind.vp = 1;
        s_bind.vp_w = SC_W(s->d.width);
        s_bind.vp_h = SC_H(s->d.height);
    }
}

static void fill_in(PSConsts *c, int i, const Nv2aRcIn *in)
{
    c->inreg[i][0] = in->reg;
    c->inreg[i][1] = in->rep;
    c->inmap[i][0] = in->lo < -1e30f ? -1e30f : in->lo;
    c->inmap[i][1] = in->hi > 1e30f ? 1e30f : in->hi;
    c->inmap[i][2] = in->s;
    c->inmap[i][3] = in->b;
}

/* RECOMP_GPU_PROF=1: cycles per draw in each stage of draw_common, printed
 * with the RECOMP_GPU_STATS line. */
#include <intrin.h>
static int s_prof_on = -1;
static uint64_t s_prof[12], s_prof_t, s_prof_draws;
static const char *const s_prof_name[12] = {
    "target", "vin", "consts", "ib", "vsc", "pcbuild",
    "tex", "ps", "blend+ds", "pipe", "draw", "tail" };
#define PROF_MARK(k) do { if (s_prof_on > 0) { uint64_t _t = __rdtsc(); \
    s_prof[k] += _t - s_prof_t; s_prof_t = _t; } } while (0)

static void prof_report(void)
{
    int k;
    if (s_prof_on <= 0 || !s_prof_draws)
        return;
    fprintf(stderr, "[GPU-PROF] cycles/draw over %llu draws:",
            (unsigned long long)s_prof_draws);
    for (k = 0; k < 12; k++)
        fprintf(stderr, " %s %llu", s_prof_name[k],
                (unsigned long long)(s_prof[k] / s_prof_draws));
    fprintf(stderr, "\n");
    memset(s_prof, 0, sizeof s_prof);
    s_prof_draws = 0;
}

/* Ctrl+F12 report frame (the "black ground" hunt): one line per draw saying what it was drawn with --
 * each texture stage's decode and its average colour (a texture decoded from zeros shows as 00000000),
 * whether the combiner's own compiled shader or the generic one ran, and non-finite numbers in the
 * vertex constants and inputs. A player sends play_log.txt; the black draws are the ones with a dark
 * stage, a NaN, or a compiled shader where the generic one is right. Only runs in a traced frame. */
static const char *pss_peek(const PSConsts *pc, uint64_t *hout)
{
    uint32_t k[PSS_KEY_MAX], n, i;
    uint64_t h;

    *hout = 0;
    if (s_pss_on <= 0)
        return "off";
    n = pss_key(pc, k);
    h = pss_hash(k, n);
    *hout = h;
    for (i = 0; i < PSS_SLOTS; i++) {
        const PsSpec *c = &s_pss[(h + i) & (PSS_SLOTS - 1)];
        if (!c->key)
            return "new";
        if (c->hash == h && c->nkey == n && !memcmp(c->key, k, n * 4))
            return c->state == 2 ? "ready" : c->state == 3 ? "failed" : "queued";
    }
    return "full";
}

static void diag_tex(const Nv2aGpuTexture *t, int i)
{
    if (!t->valid && !t->from_surface) {
        fprintf(stderr, " | t%d off", i);
        return;
    }
    if (t->from_surface) {
        fprintf(stderr, " | t%d surface %08X %ux%u", i, t->surf_va, t->surf_w, t->surf_h);
        return;
    }
    {
        uint32_t w = t->lv_w ? t->lv_w[0] : 0, h = t->lv_h ? t->lv_h[0] : 0, n = w * h, step, k, cnt = 0, zero = 0;
        uint64_t r = 0, g = 0, b = 0, a = 0;
        const GpuTex *g2 = t->slot < GPU_TEX_SLOTS ? &s_tex[t->slot] : NULL;
        if (t->argb && n) {
            step = n / 512u ? n / 512u : 1u;
            for (k = 0; k < n; k += step) {
                uint32_t p = t->argb[(t->lv_off ? t->lv_off[0] : 0) + k];
                r += (p >> 16) & 255u; g += (p >> 8) & 255u; b += p & 255u; a += p >> 24;
                if (!(p & 0x00FFFFFFu))
                    zero++;
                cnt++;
            }
        }
        if (cnt)
            fprintf(stderr, " | t%d slot %u ver %u %ux%u lv %u avg %02X%02X%02X%02X black %u%% gpu %s",
                    i, t->slot, t->version, w, h, t->levels, (unsigned)(a / cnt), (unsigned)(r / cnt),
                    (unsigned)(g / cnt), (unsigned)(b / cnt), zero * 100u / cnt,
                    g2 && g2->srv ? "ok" : "NONE");
        else
            fprintf(stderr, " | t%d slot %u ver %u %ux%u NO-PIXELS gpu %s", i, t->slot, t->version, w, h,
                    g2 && g2->srv ? "ok" : "NONE");
    }
}

static void draw_diag(const Nv2aGpuState *st, const Nv2aGpuProgram *pg, const PSConsts *pc,
                      const Nv2aGpuVertex *v, uint32_t nverts, uint32_t nidx, uint32_t sw, uint32_t sh)
{
    static uint32_t n;
    uint64_t h;
    const char *state = pss_peek(pc, &h);
    int i;

    fprintf(stderr, "[BDIAG] #%u %s into %ux%u n %u idx %u | rc %u prog_ver %u | ps %s %s %016llX | blend %u %X/%X | z %u/%u",
            n++, pg ? "PRG" : "FIX", sw, sh, nverts, nidx, (unsigned)pc->misc[0], st->prog_ver,
            s_bind.ps == s_ps ? "generic" : "compiled", state, (unsigned long long)h,
            st->rs.blend_enable, st->rs.blend_src, st->rs.blend_dst, st->rs.depth_test_enable, st->rs.depth_write);
    for (i = 0; i < 4; i++)
        diag_tex(&st->tex[i], i);
    if (pg) {
        uint32_t k, bad_c = 0, bad_v = 0, nv = nverts * pg->nused;
        const float *c = &pg->consts[0][0], *in = &pg->vin[0][0];
        for (k = 0; k < 192u * 4u; k++)
            if ((((const uint32_t *)c)[k] & 0x7F800000u) == 0x7F800000u)
                bad_c++;
        for (k = 0; k < nv * 4u && k < 16384u; k++)
            if ((((const uint32_t *)in)[k] & 0x7F800000u) == 0x7F800000u)
                bad_v++;
        fprintf(stderr, " | cver %u nins %u nonfinite c %u v %u", pg->const_version, pg->nins, bad_c, bad_v);
        if (pg->slot[3] < pg->nused && nverts) {
            float sum[4] = {0, 0, 0, 0};
            uint32_t m = nverts < 32u ? nverts : 32u;
            for (k = 0; k < m; k++)
                for (i = 0; i < 4; i++)
                    sum[i] += pg->vin[k * pg->nused + pg->slot[3]][i];
            fprintf(stderr, " | v3 avg %.2f %.2f %.2f %.2f", sum[0] / m, sum[1] / m, sum[2] / m, sum[3] / m);
        }
    } else if (v && nverts) {
        fprintf(stderr, " | d0 %.2f %.2f %.2f %.2f", v[0].d0[0], v[0].d0[1], v[0].d0[2], v[0].d0[3]);
    }
    fputc('\n', stderr);
}

/* Counters for the head of a report frame (nv2a_pb_exec.c's frame_trace_flip). */
void nv2a_gpu_diag_header(void)
{
    uint32_t i, ready = 0, queued = 0, failed = 0, tex = 0;
    for (i = 0; i < PSS_SLOTS; i++) {
        if (!s_pss[i].key)
            continue;
        if (s_pss[i].state == 2) ready++;
        else if (s_pss[i].state == 3) failed++;
        else queued++;
    }
    for (i = 0; i < GPU_TEX_SLOTS; i++)
        if (s_tex[i].srv)
            tex++;
    fprintf(stderr, "[BDIAG] gpu: compiled shaders ready %u queued %u failed %u | gpu textures live %u created %u | draws %u\n",
            ready, queued, failed, tex, (unsigned)s_st.textures, (unsigned)s_st.draws);
}

/* A batch: nverts vertices, drawn as a triangle list -- in order, or
 * through idx (nidx indices into v) when the executor sends each vertex
 * once (nv2a_gpu_draw_indexed). */
static float wpn_strength(void);              /* the weapon lighting block, after the blit code */
static float wpn_amount(void);
static void wpn_bind(const GpuSurf *s);
static void draw_common(const Nv2aGpuState *st, const Nv2aGpuVertex *v,
                        uint32_t nverts, const uint32_t *idx, uint32_t nidx,
                        const Nv2aGpuProgram *pg)
{
    static PSConsts pc;
    GpuSurf *s;
    GpuDepth *z;
    UINT bytes = nverts * (UINT)sizeof(Nv2aGpuVertex), stride = sizeof(Nv2aGpuVertex);
    UINT first;
    D3D11_MAPPED_SUBRESOURCE m;
    int i, fold_base = 0;
    uint32_t vbase = 0;

    if (s_prof_on < 0)
        s_prof_on = getenv("RECOMP_GPU_PROF") != NULL;
    if (s_prof_on > 0) {
        s_prof_t = __rdtsc();
        s_prof_draws++;
    }

    if (pg)
        bytes = nverts * pg->nused * 16u;
    if (s_on <= 0 || !nverts || bytes > (pg ? GPU_VIN_BYTES : s_vb_bytes))
        return;
    s = surf_get(&st->color);
    if (!s)
        return;
    z = depth_get(st->zeta_va, s->d.width, s->d.height);
    set_target(s, z);
    {
        D3D11_RECT r;
        r.left = SC_X(st->clip_x);
        r.top = SC_Y(st->clip_y);
        r.right = SC_X(st->clip_x + st->clip_w);
        r.bottom = SC_Y(st->clip_y + st->clip_h);
        if (!s_bind.sc || memcmp(&r, &s_bind.rect, sizeof r)) {
            ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &r);
            s_bind.sc = 1;
            s_bind.rect = r;
        }
    }

    PROF_MARK(0);
    if (pg) {
        /* The program, the constants and the vertices' inputs. */
        VDrawConsts dc;
        static VDrawConsts last_dc;
        static int last_dc_ok;
        /* The inputs' start: D3D's SV_VertexID leaves out the base vertex,
         * so a base vertex cannot carry it. An indexed draw puts its inputs
         * at a whole vertex and adds that vertex to its indices, which
         * leaves the per-draw constants the same from draw to draw (a
         * DISCARD map each draw was a good part of the driver's time);
         * a non-indexed one (and the stream-out check) passes the start in
         * the constants. */
        UINT unit = pg->nused * 16u;
        if (idx && unit && !s_cap_out) {
            s_vin_pos = (s_vin_pos + unit - 1u) / unit * unit;
            fold_base = 1;
        }
        if (s_vin_pos + bytes > GPU_VIN_BYTES)
            s_vin_pos = 0;
        if (fold_base)
            vbase = s_vin_pos / unit;
        if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vin, 0,
                                           s_vin_pos ? D3D11_MAP_WRITE_NO_OVERWRITE
                                                     : D3D11_MAP_WRITE_DISCARD, 0, &m)))
            return;
        memcpy((uint8_t *)m.pData + s_vin_pos, pg->vin, bytes);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vin, 0);
        PROF_MARK(1);
        first = 0;
        if (pg->prog_version != s_prog_loaded_ver || pg->nins != s_prog_loaded_n
            || (const void *)pg->prog != s_prog_loaded_ptr) {
            if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_cb_prog, 0,
                                               D3D11_MAP_WRITE_DISCARD, 0, &m)))
                return;
            memcpy(m.pData, pg->prog, (size_t)(pg->nins > 136 ? 136 : pg->nins) * 16);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb_prog, 0);
            s_prog_loaded_ver = pg->prog_version;
            s_prog_loaded_n = pg->nins;
            s_prog_loaded_ptr = pg->prog;
        }
        if (pg->const_version != s_const_loaded_ver) {
            if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_cb_const, 0,
                                               D3D11_MAP_WRITE_DISCARD, 0, &m)))
                return;
            memcpy(m.pData, pg->consts, 192 * 16);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb_const, 0);
            s_const_loaded_ver = pg->const_version;
        }
        memset(&dc, 0, sizeof dc);
        dc.di[0] = pg->nins > 136 ? 136 : pg->nins;
        dc.di[1] = pg->nused;
        dc.di[2] = fold_base ? 0u : s_vin_pos / 16u;
        {
            static int noclip = -1;
            if (noclip < 0)
                noclip = getenv("RECOMP_GPU_VS_NOCLIP") != NULL;
            dc.di[3] = (uint32_t)pg->near_z | (noclip ? 2u : 0u);
        }
        dc.df[0] = pg->aa_sx;
        dc.df[1] = pg->aa_sy;
        dc.df[2] = pg->near_o2;
        dc.df[3] = pg->hud_k;
        dc.hud[0] = pg->hud_shift;
        dc.fogp[0] = pg->fog_param[0];
        dc.fogp[1] = pg->fog_param[1];
        dc.fogp[2] = pg->fog_enable ? 1.0f : 0.0f;
        memcpy(&dc.fogp[3], &pg->fog_mode, 4);
        memcpy(dc.slot, pg->slot, sizeof dc.slot);
        if (!last_dc_ok || memcmp(&dc, &last_dc, sizeof dc)) {
            if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_cb_draw, 0,
                                               D3D11_MAP_WRITE_DISCARD, 0, &m)))
                return;
            memcpy(m.pData, &dc, sizeof dc);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb_draw, 0);
            last_dc = dc;
            last_dc_ok = 1;
        }
        s_vin_pos += bytes;
    } else {
        /* Vertices: a ring, rewound (DISCARD) when it fills. */
        if (s_vb_pos + bytes > s_vb_bytes)
            s_vb_pos = 0;
        if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vb, 0,
                                           s_vb_pos ? D3D11_MAP_WRITE_NO_OVERWRITE
                                                    : D3D11_MAP_WRITE_DISCARD, 0, &m)))
            return;
        memcpy((uint8_t *)m.pData + s_vb_pos, v, bytes);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
        first = s_vb_pos / stride;
        s_vb_pos += bytes;
    }
    PROF_MARK(2);
    if (idx) {
        UINT ibytes = nidx * 4u;
        if (!nidx || ibytes > GPU_IB_BYTES || !s_ib)
            return;
        if (s_ib_pos + ibytes > GPU_IB_BYTES)
            s_ib_pos = 0;
        if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_ib, 0,
                                           s_ib_pos ? D3D11_MAP_WRITE_NO_OVERWRITE
                                                    : D3D11_MAP_WRITE_DISCARD, 0, &m)))
            return;
        if (vbase) {
            uint32_t *d = (uint32_t *)((uint8_t *)m.pData + s_ib_pos), k;
            for (k = 0; k < nidx; k++)
                d[k] = idx[k] + vbase;
        } else
            memcpy((uint8_t *)m.pData + s_ib_pos, idx, ibytes);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_ib, 0);
    }

    PROF_MARK(3);
    /* Constants. */
    {
        VSConsts vc;
        vc.v[0] = 1.0f / (float)s->d.width;
        vc.v[1] = 1.0f / (float)s->d.height;
        vc.v[2] = 1.0f / (st->zscale > 0 ? st->zscale : 16777215.0f);
        vc.v[3] = 0;
        {
            static VSConsts last;
            if ((!s_vc_ok || memcmp(&vc, &last, sizeof vc))
                && SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_cb_vs, 0,
                                                     D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                memcpy(m.pData, &vc, sizeof vc);
                ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb_vs, 0);
                last = vc;
                s_vc_ok = 1;
            }
        }
    }
    PROF_MARK(4);
    /* The combiner's part of the constants depends on the program alone, and
     * the program is the same for most draws: keep it (st->prog_ver). */
    static PSConsts pc_rc;
    static uint32_t pc_rc_ver;
    int pc_hit = st->prog_ver && st->use_rc && st->prog && pc_rc_ver == st->prog_ver;
    if (pc_hit)
        memcpy(&pc, &pc_rc, sizeof pc);
    else
        memset(&pc, 0, sizeof pc);
    if (!pc_hit && st->use_rc && st->prog) {
        const Nv2aRcProg *p = st->prog;
        uint32_t k;
        pc.misc[0] = (int32_t)p->n;
        pc.misc[1] = (int32_t)p->mux_msb;
        pc.misc[2] = (int32_t)p->stage0_on;
        pc.misc[3] = (int32_t)p->has_final;
        pc.misc2[0] = (int32_t)p->fflags;
        memcpy(pc.fc0, p->fc0, 16);
        memcpy(pc.fc1, p->fc1, 16);
        for (k = 0; k < p->n && k < 8; k++) {
            const Nv2aRcStage *g = &p->st[k];
            memcpy(pc.c0[k], g->c0, 16);
            memcpy(pc.c1[k], g->c1, 16);
            pc.fl[k][0] = (int32_t)g->fl;
            pc.fl[k][1] = (int32_t)g->afl;
            pc.fl[k][2] = g->dst_ab | (g->dst_cd << 4) | (g->dst_sum << 8);
            pc.fl[k][3] = g->adst_ab | (g->adst_cd << 4) | (g->adst_sum << 8);
            pc.outm[k][0] = g->pre;
            pc.outm[k][1] = g->mul;
            pc.outm[k][2] = g->apre;
            pc.outm[k][3] = g->amul;
            fill_in(&pc, k * 8 + 0, &g->A);
            fill_in(&pc, k * 8 + 1, &g->B);
            fill_in(&pc, k * 8 + 2, &g->C);
            fill_in(&pc, k * 8 + 3, &g->D);
            fill_in(&pc, k * 8 + 4, &g->aA);
            fill_in(&pc, k * 8 + 5, &g->aB);
            fill_in(&pc, k * 8 + 6, &g->aC);
            fill_in(&pc, k * 8 + 7, &g->aD);
        }
        fill_in(&pc, 64, &p->E);
        fill_in(&pc, 65, &p->F);
        fill_in(&pc, 66, &p->A);
        fill_in(&pc, 67, &p->B);
        fill_in(&pc, 68, &p->C);
        fill_in(&pc, 69, &p->D);
        fill_in(&pc, 70, &p->G);
        pc.misc2[1] = 1;
        if (st->prog_ver) {
            pc_rc = pc;
            pc_rc_ver = st->prog_ver;
        }
    }
    pc.misc2[2] = (int32_t)(st->alpha_test != 0);
    pc.misc2[3] = (int32_t)st->alpha_func;
    pc.misc3[0] = (int32_t)st->alpha_ref;
    pc.misc3[1] = (int32_t)st->clip_plane_mode;
    pc.misc3[2] = (int32_t)st->stage_program;
    pc.misc3[3] = st->textured;
    memcpy(pc.fogc, st->fog_color, 16);
    pc.wpn[0] = (pg && pg->weapon) ? wpn_strength() : 0.0f;

    PROF_MARK(5);
    /* Textures and samplers, per stage. */
    for (i = 0; i < 4; i++) {
        const Nv2aGpuTexture *t = &st->tex[i];
        ID3D11ShaderResourceView *srv = NULL;
        ID3D11SamplerState *smp;
        D3D11_SAMPLER_DESC sd;
        uint32_t tw = 1, th = 1, levels = 1;
        int lin;
        if (t->valid && t->from_surface) {
            GpuSurf *ss = surf_for_texture(t->surf_va, t->surf_bpp, t->surf_w,
                                           t->surf_h, t->surf_pitch, t->surf_swizzled);
            if (ss) {
                s_st.rtt++;
                if (ss == s) {
                    srv = scratch_copy(ss);
                } else {
                    if (!ss->srv)
                        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)ss->tex,
                                                              NULL, &ss->srv);
                    srv = ss->srv;
                }
                tw = ss->d.width;
                th = ss->d.height;
            }
        } else if ((srv = tex_get(t)) != NULL) {
            tw = t->lv_w[0];
            th = t->lv_h[0];
            levels = t->levels;
        }
        if (!srv) {
            pc.tscale[i][2] = 0;
            if (!s_bind.srvs || s_bind.srv[i]) {
                ID3D11DeviceContext_PSSetShaderResources(s_ctx, (UINT)i, 1, &srv);
                s_bind.srv[i] = NULL;
            }
            continue;
        }
        pc.tscale[i][0] = t->normalized ? 1.0f : 1.0f / (float)tw;
        pc.tscale[i][1] = t->normalized ? 1.0f : 1.0f / (float)th;
        pc.tscale[i][2] = 1.0f;
        pc.tscale[i][3] = t->force_alpha ? 1.0f : 0.0f;
        memset(&sd, 0, sizeof sd);
        lin = t->mag == 2 || t->min == 2 || t->min == 4 || t->min == 6;
        if (levels > 1 && t->min >= 3 && t->min <= 6)
            sd.Filter = lin ? D3D11_FILTER_MIN_MAG_MIP_LINEAR
                            : D3D11_FILTER_MIN_MAG_MIP_POINT;
        else
            sd.Filter = lin ? D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT
                            : D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = addr_mode(t->addr_u, t->border_color);
        sd.AddressV = addr_mode(t->addr_v, t->border_color);
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxAnisotropy = 1;
        {   /* a new anisotropy level: start the sampler cache afresh (it is small; stepping through the
             * levels would otherwise fill it and make every bind create a sampler) */
            static int seen_aniso = 1;
            if (g_gpu_aniso != seen_aniso) {
                int k;
                seen_aniso = g_gpu_aniso;
                for (k = 0; k < s_nsamp; k++)
                    if (s_samp[k].o) {
                        ID3D11SamplerState_Release(s_samp[k].o);
                        s_samp[k].o = NULL;
                    }
                s_nsamp = 0;
                bind_forget();
            }
        }
        if (g_gpu_aniso > 1 && lin) {                 /* anisotropic filtering (the settings screen) */
            sd.Filter = D3D11_FILTER_ANISOTROPIC;
            sd.MaxAnisotropy = (UINT)g_gpu_aniso;
        }
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.BorderColor[0] = (float)((t->border >> 16) & 0xFF) / 255.0f;
        sd.BorderColor[1] = (float)((t->border >> 8) & 0xFF) / 255.0f;
        sd.BorderColor[2] = (float)(t->border & 0xFF) / 255.0f;
        sd.BorderColor[3] = (float)(t->border >> 24) / 255.0f;
        sd.MinLOD = 0;
        sd.MaxLOD = levels > 1 ? (float)(levels - 1) : 0.0f;
        smp = sampler_get(&sd);
        if (!s_bind.srvs || s_bind.srv[i] != srv) {
            ID3D11DeviceContext_PSSetShaderResources(s_ctx, (UINT)i, 1, &srv);
            s_bind.srv[i] = srv;
        }
        if (!s_bind.srvs || s_bind.smp[i] != smp) {
            ID3D11DeviceContext_PSSetSamplers(s_ctx, (UINT)i, 1, &smp);
            s_bind.smp[i] = smp;
        }
    }
    PROF_MARK(6);
    s_bind.srvs = 1;
    if (pc.wpn[0] > 0.0f)
        wpn_bind(s);
    {
        /* The same constants as the draw before (most of a mesh's draws):
         * the same shader, and nothing to upload. The look-up still runs
         * while the bound shader is the generic one, so a specialised one
         * whose compile has finished is taken up. */
        static PSConsts last;
        int same = s_pc_ok && !memcmp(&pc, &last, sizeof pc);
        if (!same || s_bind.ps == s_ps || !s_bind.ps) {
            ID3D11PixelShader *want = pss_lookup(&pc);
            if (s_bind.ps != want) {
                ID3D11DeviceContext_PSSetShader(s_ctx, want, NULL, 0);
                s_bind.ps = want;
            }
        }
        if (!same
            && SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_cb_ps, 0,
                                                 D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, &pc, sizeof pc);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb_ps, 0);
            last = pc;
            s_pc_ok = 1;
        }
    }
    if (st->trace)
        draw_diag(st, pg, &pc, v, nverts, nidx, s->d.width, s->d.height);

    PROF_MARK(7);
    /* Blend, colour mask. */
    {
        const Nv2aRenderState *rs = &st->rs;
        D3D11_BLEND_DESC bd;
        D3D11_RENDER_TARGET_BLEND_DESC *b = &bd.RenderTarget[0];
        float factor[4];
        uint32_t bc = rs->blend_color;
        UINT8 mask = 0;
        memset(&bd, 0, sizeof bd);
        if (rs->blend_enable && !(rs->blend_src == 1 && rs->blend_dst == 0)) {
            b->BlendEnable = TRUE;
            b->SrcBlend = blend_factor(rs->blend_src, 0);
            b->DestBlend = blend_factor(rs->blend_dst, 0);
            b->BlendOp = blend_op(rs->blend_eq);
            b->SrcBlendAlpha = blend_factor(rs->blend_src, 1);
            b->DestBlendAlpha = blend_factor(rs->blend_dst, 1);
            b->BlendOpAlpha = b->BlendOp;
        } else {
            b->SrcBlend = b->SrcBlendAlpha = D3D11_BLEND_ONE;
            b->DestBlend = b->DestBlendAlpha = D3D11_BLEND_ZERO;
            b->BlendOp = b->BlendOpAlpha = D3D11_BLEND_OP_ADD;
        }
        if (rs->color_mask & 0x00010000u) mask |= D3D11_COLOR_WRITE_ENABLE_RED;
        if (rs->color_mask & 0x00000100u) mask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
        if (rs->color_mask & 0x00000001u) mask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
        if (rs->color_mask & 0x01000000u) mask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
        b->RenderTargetWriteMask = mask;
        if (rs->blend_src == 0x8003 || rs->blend_src == 0x8004
            || rs->blend_dst == 0x8003 || rs->blend_dst == 0x8004) {
            factor[0] = factor[1] = factor[2] = factor[3] = (float)(bc >> 24) / 255.0f;
        } else {
            factor[0] = (float)((bc >> 16) & 0xFF) / 255.0f;
            factor[1] = (float)((bc >> 8) & 0xFF) / 255.0f;
            factor[2] = (float)(bc & 0xFF) / 255.0f;
            factor[3] = (float)(bc >> 24) / 255.0f;
        }
        {
            ID3D11BlendState *bs = blend_get(&bd);
            if (!s_bind.blend || s_bind.bs != bs || memcmp(factor, s_bind.bf, sizeof factor)) {
                ID3D11DeviceContext_OMSetBlendState(s_ctx, bs, factor, 0xFFFFFFFFu);
                s_bind.blend = 1;
                s_bind.bs = bs;
                memcpy(s_bind.bf, factor, sizeof factor);
            }
        }
    }
    /* Depth and stencil. The software path writes depth whenever the mask
     * is on, testing or not, so "write, no test" is ALWAYS here. */
    {
        const Nv2aRenderState *rs = &st->rs;
        D3D11_DEPTH_STENCIL_DESC dd;
        memset(&dd, 0, sizeof dd);
        dd.DepthEnable = (rs->depth_test_enable || rs->depth_write) && z;
        dd.DepthWriteMask = rs->depth_write ? D3D11_DEPTH_WRITE_MASK_ALL
                                            : D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = rs->depth_test_enable ? cmp_func(rs->depth_func)
                                             : D3D11_COMPARISON_ALWAYS;
        static int no_stencil = -1;
        if (no_stencil < 0)
            no_stencil = getenv("RECOMP_GPU_NO_STENCIL") != NULL;
        dd.StencilEnable = rs->stencil_enable && z && !no_stencil;
        dd.StencilReadMask = (UINT8)rs->stencil_func_mask;
        dd.StencilWriteMask = (UINT8)rs->stencil_write_mask;
        dd.FrontFace.StencilFailOp = stencil_op(rs->stencil_fail);
        dd.FrontFace.StencilDepthFailOp = stencil_op(rs->stencil_zfail);
        dd.FrontFace.StencilPassOp = stencil_op(rs->stencil_zpass);
        dd.FrontFace.StencilFunc = cmp_func(rs->stencil_func);
        dd.BackFace = dd.FrontFace;
        {
            ID3D11DepthStencilState *ds = dss_get(&dd);
            UINT ref = rs->stencil_ref & 0xFF;
            if (!s_bind.dss || s_bind.ds != ds || s_bind.sref != ref) {
                ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, ds, ref);
                s_bind.dss = 1;
                s_bind.ds = ds;
                s_bind.sref = ref;
            }
        }
    }

    PROF_MARK(8);
    if (s_bind.pipe != (pg ? 2 : 1)) {
        /* 1: vertices already transformed (s_vs); 2: the program (s_vs_prog). */
        UINT off = 0;
        if (pg) {
            ID3D11Buffer *cbs[3];
            cbs[0] = s_cb_prog; cbs[1] = s_cb_const; cbs[2] = s_cb_draw;
            ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
            ID3D11DeviceContext_VSSetShader(s_ctx, s_vs_prog, NULL, 0);
            ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 2, 3, cbs);
            ID3D11DeviceContext_VSSetShaderResources(s_ctx, 0, 1, &s_vin_srv);
        } else {
            ID3D11DeviceContext_IASetInputLayout(s_ctx, s_il);
            ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride, &off);
            ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
        }
        ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (s_ib)
            ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_ib, DXGI_FORMAT_R32_UINT, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_cb_vs);
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 1, 1, &s_cb_ps);
        s_bind.ps = NULL;
        ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
        s_bind.pipe = pg ? 2 : 1;
    }

    zpass_set(st->zpass);
    {
        static int chk = -1;
        if (chk < 0)
            chk = getenv("RECOMP_GPU_CHECK_BIND") != NULL;
        if (chk)
            bind_check();
    }
    PROF_MARK(9);
    if (idx) {
        ID3D11DeviceContext_DrawIndexed(s_ctx, nidx, s_ib_pos / 4u, (INT)first);
        s_ib_pos += nidx * 4u;
        if (pg && s_cap_out && s_so_gs && nverts <= SO_MAX_VERTS) {
            /* The same vertices once more, as points into the stream-out
             * buffer, and back to the executor (nv2a_gpu_program_capture). */
            UINT off = 0;
            D3D11_MAPPED_SUBRESOURCE cm;
            ID3D11DeviceContext_GSSetShader(s_ctx, s_so_gs, NULL, 0);
            ID3D11DeviceContext_SOSetTargets(s_ctx, 1, &s_so_buf, &off);
            ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            ID3D11DeviceContext_Draw(s_ctx, nverts, 0);
            ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11DeviceContext_SOSetTargets(s_ctx, 0, NULL, NULL);
            ID3D11DeviceContext_GSSetShader(s_ctx, NULL, NULL, 0);
            ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s_so_stage,
                                             (ID3D11Resource *)s_so_buf);
            if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_so_stage,
                                                  0, D3D11_MAP_READ, 0, &cm))) {
                memcpy(s_cap_out, cm.pData, (size_t)nverts * SO_FLOATS * 4);
                ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_so_stage, 0);
            }
            s_cap_out = NULL;
        }
    } else
        ID3D11DeviceContext_Draw(s_ctx, nverts, first);
    PROF_MARK(10);
    s_st.draws++;
    /* RECOMP_GPU_PIXWATCH=x,y in a traced frame: after every draw into a
     * surface that covers the pixel, read it back and say when it changed --
     * printed before the batch's [FTRACE] line, so the batch that drew a
     * wrong pixel is the one that follows. */
    if (st->trace) {
        static int px = -2, py;
        static uint32_t last = 0xDEADBEEFu;
        static ID3D11Texture2D *one;
        if (px == -2) {
            const char *pw = getenv("RECOMP_GPU_PIXWATCH");
            px = -1;
            if (pw)
                sscanf(pw, "%d,%d", &px, &py);
        }
        if (px >= 0 && (uint32_t)px < s->d.width && (uint32_t)py < s->d.height
            && s->d.width >= 320) {
            D3D11_BOX box;
            D3D11_MAPPED_SUBRESOURCE pm;
            if (!one) {
                D3D11_TEXTURE2D_DESC td;
                memset(&td, 0, sizeof td);
                td.Width = td.Height = 1;
                td.MipLevels = td.ArraySize = 1;
                td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_STAGING;
                td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &one);
            }
            box.left = (UINT)px; box.right = (UINT)px + 1;
            box.top = (UINT)py; box.bottom = (UINT)py + 1;
            box.front = 0; box.back = 1;
            if (one) {
                ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)one, 0, 0, 0, 0,
                                                          (ID3D11Resource *)s->tex, 0, &box);
                if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)one, 0,
                                                      D3D11_MAP_READ, 0, &pm))) {
                    uint32_t v = *(const uint32_t *)pm.pData;
                    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)one, 0);
                    if (v != last) {
                        fprintf(stderr, "[GPU-PIX] %d,%d on %08X now %08X (was %08X)\n",
                                px, py, s->d.va, v, last);
                        last = v;
                    }
                }
            }
        }
    }
    /* RECOMP_GPU_REDWATCH in a traced frame (F9): after every draw into a
     * screen-sized surface, count its strongly red pixels and say when a
     * draw added a lot of them -- printed before that batch's [FTRACE]
     * line. For the red ribbons and rails: which batch, so its program and
     * inputs can be looked at. One read-back per draw, one frame only. */
    if (st->trace && s->d.width >= 320 && !s->d.swizzled) {
        static int on = -1;
        static uint32_t last_va, last_n;
        static ID3D11Texture2D *full;
        static UINT fw, fh;
        if (on < 0)
            on = getenv("RECOMP_GPU_REDWATCH") != NULL;
        if (on) {
            D3D11_MAPPED_SUBRESOURCE pm;
            uint32_t x, y, n = 0;
            if (full && (fw != s->d.width || fh != s->d.height)) {
                ID3D11Texture2D_Release(full);
                full = NULL;
            }
            if (!full) {
                D3D11_TEXTURE2D_DESC td;
                ID3D11Texture2D_GetDesc(s->tex, &td);
                td.Usage = D3D11_USAGE_STAGING;
                td.BindFlags = 0;
                td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                td.MiscFlags = 0;
                ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &full);
                fw = s->d.width;
                fh = s->d.height;
            }
            if (full) {
                ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)full,
                                                 (ID3D11Resource *)s->tex);
                if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)full, 0,
                                                      D3D11_MAP_READ, 0, &pm))) {
                    for (y = 0; y < fh; y++) {
                        const uint8_t *row = (const uint8_t *)pm.pData + (size_t)y * pm.RowPitch;
                        for (x = 0; x < fw; x++) {
                            const uint8_t *px = row + x * 4;   /* B G R A */
                            if (px[2] > 120 && px[2] > 3 * px[1] && px[2] > 3 * px[0])
                                n++;
                        }
                    }
                    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)full, 0);
                    if (s->d.va == last_va && n > last_n + 64)
                        fprintf(stderr, "[REDWATCH] surface %08X: +%u red pixels"
                                " (%u now) from the batch below\n",
                                s->d.va, n - last_n, n);
                    last_va = s->d.va;
                    last_n = n;
                }
            }
        }
    }
    /* Hand the driver its queue every few batches: left to itself it holds
     * a whole frame of work until the flip's read-back asks for it, and
     * then the GPU starts only when the CPU has already stopped to wait. */
    {
        /* RECOMP_GPU_FLUSH_EVERY (default 1024): each Flush is a submission
         * to the kernel, and at 16 one frame of ~650 draws paid for 40. */
        static unsigned since_flush, every;
        if (!every) {
            const char *e = getenv("RECOMP_GPU_FLUSH_EVERY");
            every = e && atoi(e) > 0 ? (unsigned)atoi(e) : 1024u;
        }
        if (++since_flush >= every) {
            since_flush = 0;
            ID3D11DeviceContext_Flush(s_ctx);
        }
    }
    s->dirty = 1;
    PROF_MARK(11);
}

void nv2a_gpu_clear(const Nv2aGpuSurface *d, uint32_t zeta_va, uint32_t flags,
                    uint32_t argb, float depth01, uint32_t stencil,
                    uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    GpuSurf *s;
    if (s_on <= 0)
        return;
    s = surf_get(d);
    if (!s)
        return;
    if (flags & 0xF0) {
        float c[4];
        uint32_t px = argb;      /* A8R8G8B8 whatever the surface format */
        c[0] = (float)((px >> 16) & 0xFF) / 255.0f;
        c[1] = (float)((px >> 8) & 0xFF) / 255.0f;
        c[2] = (float)(px & 0xFF) / 255.0f;
        c[3] = (float)(px >> 24) / 255.0f;
        if (x + w > s->d.width) w = x < s->d.width ? s->d.width - x : 0;
        if (y + h > s->d.height) h = y < s->d.height ? s->d.height - y : 0;
        if (x == 0 && y == 0 && w == s->d.width && h == s->d.height) {
            ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s->rtv, c);
        } else if (w && h && s_ctx1) {
            D3D11_RECT r;
            r.left = SC_X(x); r.top = SC_Y(y);
            r.right = SC_X(x + w); r.bottom = SC_Y(y + h);
            ID3D11DeviceContext1_ClearView(s_ctx1, (ID3D11View *)s->rtv, c, &r, 1);
        }
        s->dirty = 1;
    }
    if (flags & 0x3) {
        GpuDepth *z = depth_get(zeta_va, s->d.width, s->d.height);
        UINT f = ((flags & 1) ? D3D11_CLEAR_DEPTH : 0)
               | ((flags & 2) ? D3D11_CLEAR_STENCIL : 0);
        if (z)
            ID3D11DeviceContext_ClearDepthStencilView(s_ctx, z->dsv, f, depth01,
                                                      (UINT8)stencil);
    }
}


void nv2a_gpu_draw(const Nv2aGpuState *st, const Nv2aGpuVertex *v,
                   uint32_t nverts)
{
    draw_common(st, v, nverts, NULL, 0, NULL);
}

void nv2a_gpu_draw_indexed(const Nv2aGpuState *st, const Nv2aGpuVertex *v,
                           uint32_t nverts, const uint32_t *idx, uint32_t nidx)
{
    draw_common(st, v, nverts, idx, nidx, NULL);
}

void nv2a_gpu_draw_program(const Nv2aGpuState *st, const Nv2aGpuProgram *pg,
                           uint32_t nverts, const uint32_t *idx, uint32_t nidx)
{
    if (!nv2a_gpu_program_ok() || !nidx)
        return;
    draw_common(st, NULL, nverts, idx, nidx, pg);
    s_cap_out = NULL;
}

int nv2a_gpu_program_capture(float *out)
{
    if (!s_so_gs)
        return 0;
    s_cap_out = out;
    return 1;
}

/* ---- presenting straight from the GPU ------------------------------------
 *
 * The flip used to read the finished frame back into guest memory (a copy to
 * a staging texture and a Map that waits for the GPU to finish the frame),
 * convert it, and hand it to the window thread, which drew it with GDI's
 * StretchDIBits -- a stall on the executor every frame and most of a core on
 * the window thread. Here the surface the title flipped is drawn into a swap
 * chain on the same window, letterboxed and filtered by the GPU, and the
 * guest copy is left dirty: anything that does read those bytes
 * (nv2a_gpu_sync_range) still gets them read back on demand.
 * RECOMP_GPU_PRESENT=0 goes back to the old path. */
#include <dxgi1_2.h>

static IDXGISwapChain1 *s_sc;
static ID3D11RenderTargetView *s_sc_rtv;
static UINT s_sc_w, s_sc_h;
static ID3D11VertexShader *s_blit_vs;
static ID3D11PixelShader *s_blit_ps;
static ID3D11SamplerState *s_blit_smp;
static ID3D11Buffer *s_blit_cb;
static int s_present_state = -1;        /* -1 not tried, 0 off/failed, 1 up */

static const char k_blit_hlsl[] =
    "cbuffer B : register(b0) { float4 uvmax; };\n"
    "Texture2D t : register(t0);\n"
    "SamplerState s : register(s0);\n"
    "void vs(uint id : SV_VertexID, out float4 p : SV_Position,\n"
    "        out float2 uv : TEXCOORD0)\n"
    "{\n"
    "    float2 q = float2((id << 1) & 2, id & 2);\n"
    "    p = float4(q * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    uv = q * uvmax.xy;\n"
    "}\n"
    "float4 ps(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target\n"
    "{\n"
    "    return float4(t.Sample(s, uv).rgb, 1);\n"
    "}\n"
    "float4 pscopy(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target\n"
    "{\n"
    "    return t.Sample(s, uv);\n"
    "}\n"
    /* FXAA (after T. Lottes' FXAA 2): find the edge direction from the luma of the four corners and blend
     * along it, falling back to the narrower blend when the wide one leaves the local luma range. */
    "float lum(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
    "float4 psfxaa(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target\n"
    "{\n"
    "    uint tw, th; t.GetDimensions(tw, th);\n"
    "    float2 px = 1.0 / float2(tw, th);\n"
    "    float2 lo = px * 0.5, hi = uvmax.xy - px * 0.5;\n"
    "    float3 cM = t.SampleLevel(s, clamp(uv, lo, hi), 0).rgb;\n"
    "    float3 cNW = t.SampleLevel(s, clamp(uv + float2(-1, -1) * px, lo, hi), 0).rgb;\n"
    "    float3 cNE = t.SampleLevel(s, clamp(uv + float2( 1, -1) * px, lo, hi), 0).rgb;\n"
    "    float3 cSW = t.SampleLevel(s, clamp(uv + float2(-1,  1) * px, lo, hi), 0).rgb;\n"
    "    float3 cSE = t.SampleLevel(s, clamp(uv + float2( 1,  1) * px, lo, hi), 0).rgb;\n"
    "    float lM = lum(cM), lNW = lum(cNW), lNE = lum(cNE), lSW = lum(cSW), lSE = lum(cSE);\n"
    "    float lmin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));\n"
    "    float lmax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));\n"
    "    if (lmax - lmin < max(0.0312, lmax * 0.125))\n"
    "        return float4(cM, 1);\n"
    "    float2 dir;\n"
    "    dir.x = -((lNW + lNE) - (lSW + lSE));\n"
    "    dir.y =  ((lNW + lSW) - (lNE + lSE));\n"
    "    float dirReduce = max((lNW + lNE + lSW + lSE) * 0.03125, 1.0 / 128.0);\n"
    "    float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);\n"
    "    dir = clamp(dir * rcpMin, -8.0, 8.0) * px;\n"
    "    float3 a1 = t.SampleLevel(s, clamp(uv + dir * (1.0 / 3.0 - 0.5), lo, hi), 0).rgb;\n"
    "    float3 a2 = t.SampleLevel(s, clamp(uv + dir * (2.0 / 3.0 - 0.5), lo, hi), 0).rgb;\n"
    "    float3 rgbA = 0.5 * (a1 + a2);\n"
    "    float3 b1 = t.SampleLevel(s, clamp(uv - dir * 0.5, lo, hi), 0).rgb;\n"
    "    float3 b2 = t.SampleLevel(s, clamp(uv + dir * 0.5, lo, hi), 0).rgb;\n"
    "    float3 rgbB = rgbA * 0.5 + 0.25 * (b1 + b2);\n"
    "    float lB = lum(rgbB);\n"
    "    return float4((lB < lmin || lB > lmax) ? rgbA : rgbB, 1);\n"
    "}\n";

static ID3DBlob *compile_blit(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(k_blit_hlsl, sizeof k_blit_hlsl - 1, "nv2a_blit",
                            NULL, NULL, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[NV2A-GPU] blit %s: compile failed (0x%08lX)\n%s\n",
                entry, (unsigned long)hr,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

/* The blit shaders and their state, for presenting and for scaling. */
static ID3D11PixelShader *s_copy_ps;
static ID3D11PixelShader *s_fxaa_ps;      /* optional: left NULL when it does not compile */
static ID3D11SamplerState *s_point_smp;

static int blit_init(void)
{
    static int state = -1;
    ID3DBlob *b;
    HRESULT hr;
    if (state >= 0)
        return state;
    state = 0;
    if (!(b = compile_blit("vs", "vs_4_0")))
        return 0;
    hr = ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(b),
                                         ID3D10Blob_GetBufferSize(b), NULL, &s_blit_vs);
    ID3D10Blob_Release(b);
    if (FAILED(hr) || !(b = compile_blit("ps", "ps_4_0")))
        return 0;
    hr = ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b),
                                        ID3D10Blob_GetBufferSize(b), NULL, &s_blit_ps);
    ID3D10Blob_Release(b);
    if (FAILED(hr) || !(b = compile_blit("pscopy", "ps_4_0")))
        return 0;
    hr = ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b),
                                        ID3D10Blob_GetBufferSize(b), NULL, &s_copy_ps);
    ID3D10Blob_Release(b);
    if (FAILED(hr))
        return 0;
    if ((b = compile_blit("psfxaa", "ps_4_0")) != NULL) {
        if (FAILED(ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b),
                                                  ID3D10Blob_GetBufferSize(b), NULL, &s_fxaa_ps)))
            s_fxaa_ps = NULL;
        ID3D10Blob_Release(b);
    }
    {
        D3D11_SAMPLER_DESC smp;
        D3D11_BUFFER_DESC bd;
        memset(&smp, 0, sizeof smp);
        smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        smp.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(ID3D11Device_CreateSamplerState(s_dev, &smp, &s_blit_smp)))
            return 0;
        smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        if (FAILED(ID3D11Device_CreateSamplerState(s_dev, &smp, &s_point_smp)))
            return 0;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_blit_cb)))
            return 0;
    }
    state = 1;
    return 1;
}

/* src stretched over all of dst (w x h), alpha kept: point sampling to
 * enlarge (each guest pixel a solid block, as the CPU drew it), linear to
 * shrink (2x and 4x average whole blocks). */
static int scale_blit(ID3D11ShaderResourceView *src, ID3D11RenderTargetView *dst,
                      UINT w, UINT h, int linear)
{
    static const float uv[4] = { 1, 1, 0, 0 };
    ID3D11ShaderResourceView *none = NULL;
    D3D11_VIEWPORT vp;
    if (!blit_init())
        return 0;
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_blit_cb, 0, NULL, uv, 0, 0);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &dst, NULL);
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)w; vp.Height = (float)h;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(s_ctx, NULL);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_blit_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_blit_cb);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_copy_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &src);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, linear ? &s_blit_smp : &s_point_smp);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
    bind_forget();
    return 1;
}

/* ---- WEAPON LIGHTING: state, environment copy, constants (the shader is k_wpn_hlsl, near the top) ------------------------------ */
#define ENV_W 256
#define ENV_H 144
#define WPN_BLOOD_N 8
typedef struct { float wp0[4], wp1[4], wp2[4], wp3[4], bl[WPN_BLOOD_N][4], bs[WPN_BLOOD_N][4], wl0[4], wl1[4]; } WpnCB;
typedef struct { int used; double t0; float x, y, size, seed, str; } WpnBlood;   /* one splatter: a hit */
static volatile int s_wpn_level;                /* 0 off, 1 low, 2 normal, 3 high, 4 extreme */
static float s_wpn_tan_h = 0.70f, s_wpn_tan_v = 0.525f;
static volatile float s_wpn_dust, s_wpn_wet;
static WpnBlood s_wpn_bl[WPN_BLOOD_N];
static float s_wpn_bcol[3] = { 0.40f, 0.027f, 0.022f };   /* the blood colour setting (set by the game side) */
static volatile float s_wpn_off[2];                       /* where the gun sits on the screen now, in view units (sway, run pose) */
static double s_wpn_now_s;                                /* seconds, updated at every flip */
static unsigned s_wpn_rng = 0x1234567u;
static volatile LONG s_wpn_rain_x1000;
static volatile DWORD s_wpn_rain_until;
static double s_wpn_tick_ms;
static uint32_t s_wpn_draw_frame;               /* the frame of the last weapon draw: the environment copy is only kept up to date near it */
static float s_wpn_seed = 3.7f;
static ID3D11Buffer *s_cb_wpn;
static ID3D11Texture2D *s_env_tex;
static ID3D11ShaderResourceView *s_env_srv;
static ID3D11RenderTargetView *s_env_rtv;
static ID3D11SamplerState *s_env_smp;
static int s_env_state;                         /* 0 not made yet, 1 ready, -1 failed */
static int s_env_have;                          /* a real frame has been copied into it */
static ID3D11Texture2D *s_env_stg;              /* the 8x4 level of the environment copy, read back (the light's direction is found from it) */
static int s_env_stg_pending;
static float s_wpn_ldir[3] = { 0.5f, 0.6f, -0.3f }, s_wpn_lcol[3] = { 1.0f, 1.0f, 1.0f }, s_wpn_lint;   /* the light the weapon is lit by (smoothed) */

void nv2a_gpu_weapon_config(int level) { s_wpn_level = level < 0 ? 0 : level > 4 ? 4 : level; }
int  nv2a_gpu_weapon_level(void) { return s_wpn_level; }
void nv2a_gpu_weapon_view(float tan_h, float tan_v)
{
    if (tan_h > 0.05f && tan_h < 4.0f && tan_v > 0.05f && tan_v < 4.0f) {
        s_wpn_tan_h = tan_h;
        s_wpn_tan_v = tan_v;
    }
}
void nv2a_gpu_weapon_get_tan(float *h, float *v) { *h = s_wpn_tan_h; *v = s_wpn_tan_v; }
static float wpn_rnd(void)
{
    s_wpn_rng ^= s_wpn_rng << 13;
    s_wpn_rng ^= s_wpn_rng >> 17;
    s_wpn_rng ^= s_wpn_rng << 5;
    return (float)(s_wpn_rng & 0xFFFFFFu) / 16777216.0f;
}

/* A hit: a new splatter on the gun and hands (random place on the gun's part of the view). A hit right after another one (a bullet
 * and the screen blood of the same hit, a burst) only makes the newest one bigger. 8 at most, the oldest goes first. */
static void wpn_blood_add(float strength, int fresh)
{
    double now = s_wpn_now_s > 0.0 ? s_wpn_now_s : now_ms() * 0.001;
    int i, slot = -1, newest = -1;
    for (i = 0; i < WPN_BLOOD_N; i++) {
        if (!s_wpn_bl[i].used) {
            if (slot < 0 || s_wpn_bl[slot].used)
                slot = i;
        } else {
            if (newest < 0 || s_wpn_bl[i].t0 > s_wpn_bl[newest].t0)
                newest = i;
            if (slot < 0 || (s_wpn_bl[slot].used && s_wpn_bl[i].t0 < s_wpn_bl[slot].t0))
                slot = i;
        }
    }
    if (!fresh && newest >= 0 && now - s_wpn_bl[newest].t0 < 0.5) {
        WpnBlood *b = &s_wpn_bl[newest];
        float s = b->str + 0.2f * strength + 0.08f;
        b->str = s > 1.5f ? 1.5f : s;
        return;
    }
    if (slot < 0)
        return;
    {
        WpnBlood *b = &s_wpn_bl[slot];
        b->used = 0;
        b->t0 = now;
        b->x = 0.15f + 0.70f * wpn_rnd();       /* a place on the textures of the gun and hands (their own uv: the blood sticks to the surface) */
        b->y = 0.15f + 0.70f * wpn_rnd();
        b->size = 0.10f + 0.10f * strength;     /* the setting (Low 0.6 / Normal 1 / High 1.5) scales these when drawn (wpn_bind) */
        b->seed = wpn_rnd() * 10.0f;
        b->str = 0.35f + 0.65f * strength;
        b->used = 1;
    }
}

void nv2a_gpu_weapon_trigger(int kind, float strength)
{
    float v;
    if (s_wpn_level <= 0)
        return;
    strength = strength < 0.0f ? 0.0f : strength > 1.0f ? 1.0f : strength;
    if (kind == 1) {                            /* an explosion near the player: dust */
        v = s_wpn_dust + 0.35f + 0.40f * strength;
        s_wpn_dust = v > 1.0f ? 1.0f : v;
    } else {                                    /* a hit on the player or near him: blood */
        wpn_blood_add(strength, 0);
        if (s_wpn_level >= 4) {                 /* Extreme: three splatters for every hit, the gun and hands are nearly covered */
            wpn_blood_add(strength, 1);
            wpn_blood_add(strength, 1);
        }
    }
}
void nv2a_gpu_weapon_blood_color(float r, float g, float b)
{
    s_wpn_bcol[0] = r < 0.0f ? 0.0f : r > 1.0f ? 1.0f : r;
    s_wpn_bcol[1] = g < 0.0f ? 0.0f : g > 1.0f ? 1.0f : g;
    s_wpn_bcol[2] = b < 0.0f ? 0.0f : b > 1.0f ? 1.0f : b;
}
static volatile float s_wpn_aim;
void nv2a_gpu_weapon_aim(float f)
{
    s_wpn_aim = f;
}
void nv2a_gpu_weapon_offset(float x, float y)
{
    s_wpn_off[0] = x;
    s_wpn_off[1] = y;
}
void nv2a_gpu_weapon_rain(float strength)
{
    if (strength <= 0.0f) {
        s_wpn_rain_until = 0;
        return;
    }
    s_wpn_rain_x1000 = (LONG)((strength > 1.0f ? 1.0f : strength) * 1000.0f);
    s_wpn_rain_until = GetTickCount() + 1500;
}
void nv2a_gpu_weapon_clean(void)
{
    int i;
    s_wpn_dust = s_wpn_wet = 0.0f;
    s_wpn_rain_until = 0;
    for (i = 0; i < WPN_BLOOD_N; i++)
        s_wpn_bl[i].used = 0;
}

/* Once a frame (the flip): the amounts follow time. Rain wets the weapon in about 5 s and it dries in about 30; dust fades in 50 s,
 * blood in 80 s. */
static void wpn_tick(void)
{
    double now = now_ms();
    float dt = s_wpn_tick_ms > 0.0 ? (float)((now - s_wpn_tick_ms) * 0.001) : 0.0f, w;
    static int force = -1;
    int i;
    nv2a_gpu_weapon_frame_end();
    g_nv_wpn_pass = 0;
    s_wpn_tick_ms = now;
    s_wpn_now_s = now * 0.001;
    for (i = 0; i < WPN_BLOOD_N; i++)           /* old splatters are gone (the shader fades them out by 190 s) */
        if (s_wpn_bl[i].used && s_wpn_now_s - s_wpn_bl[i].t0 > 195.0)
            s_wpn_bl[i].used = 0;
    if (force < 0) {                            /* RECOMP_WPN_FORCE=1: dust and rain drops always on, a new splatter every 7 s (to judge the look) */
        const char *e = getenv("RECOMP_WPN_FORCE");
        force = e && e[0] == '1';
    }
    if (force) {
        double newest = -1e9;
        s_wpn_dust = 0.9f;
        s_wpn_wet = 1.0f;
        for (i = 0; i < WPN_BLOOD_N; i++)
            if (s_wpn_bl[i].used && s_wpn_bl[i].t0 > newest)
                newest = s_wpn_bl[i].t0;
        if (s_wpn_level > 0 && s_wpn_now_s - newest > 7.0)
            nv2a_gpu_weapon_trigger(0, 0.7f);
        return;
    }
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.25f) dt = 0.25f;
    if ((int)(s_wpn_rain_until - GetTickCount()) > 0)
        w = s_wpn_wet + dt * 0.2f * (float)s_wpn_rain_x1000 * 0.001f;
    else
        w = s_wpn_wet - dt * 0.035f;
    s_wpn_wet = w < 0.0f ? 0.0f : w > 1.0f ? 1.0f : w;
    w = s_wpn_dust - dt * 0.02f;
    s_wpn_dust = w < 0.0f ? 0.0f : w;
}

/* how much blood / dust / rain the setting means: Low 0.6, Normal 1, High 1.5, Extreme 3 (the gun and hands nearly covered) */
static float wpn_amount(void)
{
    int l = s_wpn_level;
    return l <= 0 ? 0.0f : l == 1 ? 0.6f : l == 2 ? 1.0f : l == 3 ? 1.5f : 3.0f;
}

static float wpn_strength(void)
{
    int l = s_wpn_level;
    return l <= 0 ? 0.0f : l == 1 ? 0.6f : l == 2 ? 1.0f : l == 3 ? 1.5f : 1.8f;
}

static int wpn_env_make(void)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SAMPLER_DESC sd;
    if (s_env_state)
        return s_env_state > 0;
    s_env_state = -1;
    memset(&td, 0, sizeof td);
    td.Width = ENV_W;
    td.Height = ENV_H;
    td.MipLevels = 0;                           /* the whole chain */
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_env_tex))
        || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_env_tex, NULL, &s_env_srv))
        || FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s_env_tex, NULL, &s_env_rtv)))
        return 0;
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (FAILED(ID3D11Device_CreateSamplerState(s_dev, &sd, &s_env_smp)))
        return 0;
    td.Width = 8;                               /* level 5 of 256x144 */
    td.Height = 4;
    td.MipLevels = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.MiscFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_env_stg)))
        s_env_stg = NULL;                       /* no light direction then: the weapon keeps the default */
    s_env_state = 1;
    return 1;
}

/* Before a weapon draw: the constants of the effect and the environment copy bound to t4 / s4 / b5. */
static void wpn_bind(const GpuSurf *s)
{
    WpnCB cb;
    static WpnCB last;
    static int have_last;
    int i;
    s_wpn_draw_frame = s_frame;
    if (!s_cb_wpn) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = sizeof(WpnCB);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb_wpn)))
            return;
    }
    wpn_env_make();
    memset(&cb, 0, sizeof cb);
    cb.wp0[0] = wpn_strength();
    cb.wp0[1] = s_wpn_tan_h;
    cb.wp0[2] = s_wpn_tan_v;
    cb.wp0[3] = (float)fmod(s_wpn_now_s, 10000.0);
    cb.wp1[1] = s_wpn_aim;
    cb.wp1[0] = s_wpn_dust * wpn_amount();    /* the setting is how much dust too */
    cb.wp1[2] = s_wpn_wet * (wpn_amount() > 2.0f ? 2.0f : wpn_amount());     /* the setting is how much rain too */
    cb.wl0[0] = s_wpn_ldir[0];
    cb.wl0[1] = s_wpn_ldir[1];
    cb.wl0[2] = s_wpn_ldir[2];
    cb.wl0[3] = s_wpn_lint;
    cb.wl1[0] = s_wpn_lcol[0];
    cb.wl1[1] = s_wpn_lcol[1];
    cb.wl1[2] = s_wpn_lcol[2];
    cb.wp1[3] = s_env_have ? 1.0f : 0.0f;
    cb.wp2[0] = s_wpn_bcol[0] * 0.65f;          /* blood colour */
    cb.wp2[1] = s_wpn_bcol[1] * 0.65f;
    cb.wp2[2] = s_wpn_bcol[2] * 0.65f;
    cb.wp2[3] = s_wpn_seed;
    cb.wp3[0] = (float)SC_W(s->d.width);
    cb.wp3[1] = (float)SC_H(s->d.height);
    cb.wp3[2] = s_wpn_off[0];
    cb.wp3[3] = s_wpn_off[1];
    for (i = 0; i < WPN_BLOOD_N; i++) {
        const WpnBlood *b = &s_wpn_bl[i];
        float age = (float)(s_wpn_now_s - b->t0);
        cb.bl[i][3] = -1.0f;
        if (b->used && age >= 0.0f && age < 195.0f) {
            cb.bl[i][0] = b->x;
            cb.bl[i][1] = b->y;
            float lv = wpn_amount();          /* the setting is how much blood: applied here, so changing it changes the old splatters too */
            float st = b->str * lv;
            cb.bl[i][2] = b->size * (0.55f + 0.45f * lv);
            cb.bl[i][3] = age;
            cb.bs[i][0] = b->seed;
            cb.bs[i][1] = st > 3.0f ? 3.0f : st;
        }
    }
    if (!have_last || memcmp(&cb, &last, sizeof cb)) {
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_cb_wpn, 0, NULL, &cb, 0, 0);
        last = cb;
        have_last = 1;
    }
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 5, 1, &s_cb_wpn);
    if (s_env_srv) {
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 4, 1, &s_env_srv);
        ID3D11DeviceContext_PSSetSamplers(s_ctx, 4, 1, &s_env_smp);
    }
}

/* The light the weapon is lit by, found from the picture: the 8x4 level of the environment copy is read back (the copy asked for at the
 * last capture, so the CPU never waits), the bright places are weighted by brightness cubed: their centre says where the light is
 * (screen x -> left / right, screen y -> up / down; a bit of up is added, lights are mostly overhead), their colour is its colour,
 * their contrast with the rest is how directional it is (a grey, even picture gives a weak light). Smoothed over ~0.3 s. */
static void wpn_light_update(void)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (!s_env_stg)
        return;
    if (s_env_stg_pending && SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_env_stg, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
        float sw = 0.0f, cx = 0.0f, cy = 0.0f, cr = 0.0f, cg = 0.0f, cb = 0.0f, mean = 0.0f, mx = 0.0f;
        int x, y;
        for (y = 0; y < 4; y++) {
            const unsigned char *row = (const unsigned char *)m.pData + (size_t)y * m.RowPitch;
            for (x = 0; x < 8; x++) {
                float r = row[x * 4 + 0] / 255.0f, g = row[x * 4 + 1] / 255.0f, b = row[x * 4 + 2] / 255.0f;
                float l = 0.299f * r + 0.587f * g + 0.114f * b, w = l * l * l + 1e-4f;
                if (y == 3)
                    w *= 0.1f;                  /* the bottom row holds the gun, the hands and the HUD: they must not light themselves */
                sw += w;
                cx += w * ((float)x + 0.5f) / 8.0f;
                cy += w * ((float)y + 0.5f) / 4.0f;
                cr += w * r; cg += w * g; cb += w * b;
                mean += l;
                if (l > mx) mx = l;
            }
        }
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_env_stg, 0);
        s_env_stg_pending = 0;
        if (sw > 0.0f) {
            float lx, ly, lz = -0.3f, ln, cm, contrast, inten, a = 0.12f, nd[3], nc[3];
            cx /= sw; cy /= sw; cr /= sw; cg /= sw; cb /= sw;
            mean /= 32.0f;
            lx = (cx - 0.5f) * 2.4f;
            ly = -(cy - 0.5f) * 2.0f + 0.3f;
            ln = sqrtf(lx * lx + ly * ly + lz * lz);
            nd[0] = lx / ln; nd[1] = ly / ln; nd[2] = lz / ln;
            cm = cr > cg ? (cr > cb ? cr : cb) : (cg > cb ? cg : cb);
            cm = cm > 1e-4f ? cm : 1e-4f;
            nc[0] = 0.45f + 0.55f * cr / cm;    /* the light's colour, pulled towards white */
            nc[1] = 0.45f + 0.55f * cg / cm;
            nc[2] = 0.45f + 0.55f * cb / cm;
            contrast = mx / (mean + 0.05f);
            {   /* how one-sided the picture is (the bright part away from the middle) counts most, contrast a little */
                float off = sqrtf((cx - 0.5f) * (cx - 0.5f) + (cy - 0.5f) * (cy - 0.5f)) * 3.5f;
                float con = (contrast - 1.0f) / 2.5f;
                off = off > 1.0f ? 1.0f : off;
                con = con < 0.0f ? 0.0f : con > 1.0f ? 1.0f : con;
                inten = 0.65f * off + 0.35f * con;
            }
            inten *= mx * 1.6f > 1.0f ? 1.0f : mx * 1.6f;
            s_wpn_ldir[0] += a * (nd[0] - s_wpn_ldir[0]);
            s_wpn_ldir[1] += a * (nd[1] - s_wpn_ldir[1]);
            s_wpn_ldir[2] += a * (nd[2] - s_wpn_ldir[2]);
            s_wpn_lcol[0] += a * (nc[0] - s_wpn_lcol[0]);
            s_wpn_lcol[1] += a * (nc[1] - s_wpn_lcol[1]);
            s_wpn_lcol[2] += a * (nc[2] - s_wpn_lcol[2]);
            s_wpn_lint += a * (inten - s_wpn_lint);
        }
    }
    if (!s_env_stg_pending) {
        ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)s_env_stg, 0, 0, 0, 0, (ID3D11Resource *)s_env_tex, 5, NULL);
        s_env_stg_pending = 1;
    }
}

/* At the present: the frame just made, shrunk into the 256x144 environment texture and mipped (the shader reads a blurred level of it).
 * Only while weapon draws are happening, and every third present. uv: the part of the surface the picture covers (as the
 * present's own blit takes it). */
static void wpn_env_capture(ID3D11ShaderResourceView *src, const float *uv)
{
    static unsigned n;
    ID3D11ShaderResourceView *none = NULL;
    D3D11_VIEWPORT vp;
    if (s_wpn_level <= 0 || s_frame - s_wpn_draw_frame > 90u || !blit_init() || !wpn_env_make())
        return;
    if ((n++ % 3u) != 0 && s_env_have)
        return;
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_blit_cb, 0, NULL, uv, 0, 0);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_env_rtv, NULL);
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)ENV_W; vp.Height = (float)ENV_H;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(s_ctx, NULL);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_blit_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_blit_cb);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_copy_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &src);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_blit_smp);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
    ID3D11DeviceContext_GenerateMips(s_ctx, s_env_srv);
    s_env_have = 1;
    wpn_light_update();
    bind_forget();
}

static int present_init(HWND hwnd)
{
    IDXGIDevice *xd = NULL;
    IDXGIAdapter *ad = NULL;
    IDXGIFactory2 *fac = NULL;
    DXGI_SWAP_CHAIN_DESC1 sd;
    ID3DBlob *b;
    HRESULT hr;
    const char *e = getenv("RECOMP_GPU_PRESENT");

    if (e && e[0] == '0')
        return 0;
    if (FAILED(ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice, (void **)&xd)))
        return 0;
    hr = IDXGIDevice_GetAdapter(xd, &ad);
    IDXGIDevice_Release(xd);
    if (FAILED(hr))
        return 0;
    hr = IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&fac);
    IDXGIAdapter_Release(ad);
    if (FAILED(hr))
        return 0;
    memset(&sd, 0, sizeof sd);
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    hr = IDXGIFactory2_CreateSwapChainForHwnd(fac, (IUnknown *)s_dev, hwnd, &sd,
                                              NULL, NULL, &s_sc);
    if (SUCCEEDED(hr))
        IDXGIFactory2_MakeWindowAssociation(fac, hwnd, DXGI_MWA_NO_ALT_ENTER);
    IDXGIFactory2_Release(fac);
    if (FAILED(hr)) {
        fprintf(stderr, "[NV2A-GPU] swap chain: 0x%08lX; presenting through"
                        " the window thread\n", (unsigned long)hr);
        s_sc = NULL;
        return 0;
    }
    (void)b;
    if (!blit_init())
        return 0;
    fprintf(stderr, "[NV2A-GPU] presenting from the GPU (swap chain on the"
                    " window; RECOMP_GPU_PRESENT=0 for the old path)\n");
    return 1;
}

/* Hand the window back to GDI (the video overlay is drawn there): a flip
 * swap chain on it would cover whatever GDI draws. */
void nv2a_gpu_present_stop(void)
{
    if (!s_sc)
        return;
    if (s_sc_rtv) {
        ID3D11RenderTargetView_Release(s_sc_rtv);
        s_sc_rtv = NULL;
    }
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
    bind_forget();
    ID3D11DeviceContext_Flush(s_ctx);
    IDXGISwapChain1_Release(s_sc);
    s_sc = NULL;
    s_sc_w = s_sc_h = 0;
    s_present_state = -1;
}

/* F12 (fb_present.c sets g_screenshot_request): the picture about to be presented, saved as screenshots\MoH_<date>.png.
 * The back buffer is copied into plain memory here; a worker thread encodes the PNG through GDI+ so the game does not
 * stall for it. The overlays (fps counter, Recomp Settings, the mouse pointer) are separate windows: not in the file. */
extern volatile LONG g_screenshot_request;
extern void recomp_osd_show(const char *msg);

typedef struct { uint8_t *px; UINT w, h; wchar_t path[MAX_PATH]; } ShotJob;

static DWORD WINAPI shot_thread(LPVOID p)
{
    ShotJob *j = (ShotJob *)p;
    typedef struct { UINT32 ver; void *cb; BOOL nobg, noext; } GpIn;
    typedef int (WINAPI *startup_fn)(ULONG_PTR *, const GpIn *, void *);
    typedef int (WINAPI *scan0_fn)(INT, INT, INT, INT, BYTE *, void **);
    typedef int (WINAPI *save_fn)(void *, const WCHAR *, const CLSID *, const void *);
    typedef int (WINAPI *disp_fn)(void *);
    static const CLSID png = { 0x557CF406, 0x1A04, 0x11D3, { 0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E } };
    HMODULE gp = LoadLibraryA("gdiplus.dll");
    int ok = 0;
    if (gp) {
        startup_fn startup = (startup_fn)GetProcAddress(gp, "GdiplusStartup");
        scan0_fn scan0 = (scan0_fn)GetProcAddress(gp, "GdipCreateBitmapFromScan0");
        save_fn save = (save_fn)GetProcAddress(gp, "GdipSaveImageToFile");
        disp_fn dispose = (disp_fn)GetProcAddress(gp, "GdipDisposeImage");
        ULONG_PTR token = 0;
        GpIn in;
        void *bmp = NULL;
        memset(&in, 0, sizeof in);
        in.ver = 1;
        if (startup && scan0 && save && dispose && startup(&token, &in, NULL) == 0) {
            /* 0x22009 = PixelFormat32bppRGB: B, G, R, ignored; the alpha of the swap chain is not used */
            if (scan0((INT)j->w, (INT)j->h, (INT)(j->w * 4), 0x22009, j->px, &bmp) == 0 && bmp) {
                ok = save(bmp, j->path, &png, NULL) == 0;
                dispose(bmp);
            }
        }
    }
    recomp_osd_show(ok ? "Screenshot saved (screenshots folder)" : "Screenshot failed");
    free(j->px);
    free(j);
    return 0;
}

static void shot_take(IDXGISwapChain1 *sc)
{
    ID3D11Texture2D *bb = NULL, *st = NULL;
    D3D11_TEXTURE2D_DESC d;
    D3D11_MAPPED_SUBRESOURCE m;
    ShotJob *j;
    SYSTEMTIME t;
    UINT y;
    if (FAILED(IDXGISwapChain1_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb)))
        return;
    ID3D11Texture2D_GetDesc(bb, &d);
    if (d.Format != DXGI_FORMAT_B8G8R8A8_UNORM || !d.Width || !d.Height) {
        ID3D11Texture2D_Release(bb);
        return;
    }
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &st))) {
        ID3D11Texture2D_Release(bb);
        return;
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)st, (ID3D11Resource *)bb);
    ID3D11Texture2D_Release(bb);
    j = (ShotJob *)calloc(1, sizeof *j);
    if (!j || FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        free(j);
        ID3D11Texture2D_Release(st);
        return;
    }
    j->w = d.Width;
    j->h = d.Height;
    j->px = (uint8_t *)malloc((size_t)d.Width * d.Height * 4);
    if (j->px)
        for (y = 0; y < d.Height; y++)
            memcpy(j->px + (size_t)y * d.Width * 4, (const uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)d.Width * 4);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
    ID3D11Texture2D_Release(st);
    if (!j->px) {
        free(j);
        return;
    }
    CreateDirectoryW(L"screenshots", NULL);
    GetLocalTime(&t);
    swprintf(j->path, MAX_PATH, L"screenshots\\MoH_%04u%02u%02u_%02u%02u%02u_%03u.png", t.wYear, t.wMonth, t.wDay,
             t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    {
        HANDLE th = CreateThread(NULL, 0, shot_thread, j, 0, NULL);
        if (th)
            CloseHandle(th);
        else {
            free(j->px);
            free(j);
        }
    }
}

/* HUD bar drawn into the presented frame (the stamina bar): a vertical capsule in the HUD's grey, filled from the bottom.
 * The game side calls nv2a_gpu_hud_bar every frame it wants the bar (any thread; lock-free, the values expire after
 * 200 ms); the present thread draws it after the frame, inside the letterboxed picture, so it scales with the picture and
 * is in F12 screenshots. Position and size are fractions of the picture: x, y = top-left, h = height, aspect = width/height. */
static volatile LONG s_hb_v[5];         /* x, y, h, aspect, fill, all x 100000 */
static volatile DWORD s_hb_until;
static ID3D11PixelShader *s_hb_ps;
static ID3D11Buffer *s_hb_cb;
static ID3D11BlendState *s_hb_bs;
static int s_hb_state;                  /* 0 not tried, 1 up, -1 failed */

void nv2a_gpu_hud_bar(float x, float y, float h, float aspect, float fill)
{
    if (fill < 0.0f) fill = 0.0f;
    if (fill > 1.0f) fill = 1.0f;
    s_hb_v[0] = (LONG)(x * 100000.0f);
    s_hb_v[1] = (LONG)(y * 100000.0f);
    s_hb_v[2] = (LONG)(h * 100000.0f);
    s_hb_v[3] = (LONG)(aspect * 100000.0f);
    s_hb_v[4] = (LONG)(fill * 100000.0f + 0.5f);
    s_hb_until = GetTickCount() + 200;
}

static const char k_hb_hlsl[] =
    "cbuffer P : register(b1) { float4 pb; };\n"        /* bar width, height in pixels, fill 0..1, outline thickness */
    "float4 psbar(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target\n"
    "{\n"
    "    float w = pb.x, h = pb.y, t = pb.w;\n"
    "    float2 q = uv * float2(w, h);\n"
    "    float r = w * 0.5;\n"
    "    float d = length(float2(q.x - r, q.y - clamp(q.y, r, h - r))) - r;\n"      /* capsule distance, < 0 inside */
    "    float cov = saturate(0.5 - d);\n"
    "    float ring = saturate(d + t + 0.5);\n"
    "    float fy = pb.z >= 0.999 ? -1.0 : (h - t) - pb.z * (h - 2.0 * t);\n"          /* top of the fill, grows upwards; full = no gap */
    "    float fil = saturate(q.y - fy + 0.5);\n"
    "    float solid = max(ring, fil);\n"
    "    float3 col = lerp(float3(0.02, 0.025, 0.02), float3(0.54, 0.545, 0.54), solid);\n"
    "    return float4(col, cov * lerp(0.40, 0.96, solid));\n"
    "}\n";

static int hud_bar_init(void)
{
    ID3DBlob *code = NULL, *err = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_BLEND_DESC bl;
    HRESULT hr;

    if (s_hb_state)
        return s_hb_state > 0;
    s_hb_state = -1;
    hr = D3DCompile(k_hb_hlsl, sizeof k_hb_hlsl - 1, "hud_bar", NULL, NULL, "psbar", "ps_4_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[NV2A-GPU] hud bar: compile failed (0x%08lX)\n%s\n", (unsigned long)hr,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        if (err) ID3D10Blob_Release(err);
        return 0;
    }
    if (err) ID3D10Blob_Release(err);
    hr = ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                        ID3D10Blob_GetBufferSize(code), NULL, &s_hb_ps);
    ID3D10Blob_Release(code);
    if (FAILED(hr))
        return 0;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_hb_cb)))
        return 0;
    memset(&bl, 0, sizeof bl);
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(ID3D11Device_CreateBlendState(s_dev, &bl, &s_hb_bs)))
        return 0;
    s_hb_state = 1;
    return 1;
}

/* Called inside nv2a_gpu_present right after the frame blit: the render target is bound, the blit's VS / constant
 * buffer / topology are set. frame = the picture's rectangle inside the window. */
static void hud_bar_draw(const D3D11_VIEWPORT *frame)
{
    D3D11_VIEWPORT vp;
    float pb[4], bw, bh;

    if ((int)(s_hb_until - GetTickCount()) <= 0 || !hud_bar_init())
        return;
    bh = (float)(int)(frame->Height * (float)s_hb_v[2] * 0.00001f + 0.5f);
    bw = (float)(int)(bh * (float)s_hb_v[3] * 0.00001f + 0.5f);
    if (bw < 4.0f || bh < 8.0f)
        return;
    vp.TopLeftX = (float)(int)(frame->TopLeftX + frame->Width * (float)s_hb_v[0] * 0.00001f + 0.5f);
    vp.TopLeftY = (float)(int)(frame->TopLeftY + frame->Height * (float)s_hb_v[1] * 0.00001f + 0.5f);
    vp.Width = bw;
    vp.Height = bh;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    pb[0] = bw;
    pb[1] = bh;
    pb[2] = (float)s_hb_v[4] * 0.00001f;
    pb[3] = bw * 0.10f > 1.5f ? bw * 0.10f : 1.5f;
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_hb_cb, 0, NULL, pb, 0, 0);
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, s_hb_bs, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_hb_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 1, 1, &s_hb_cb);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFFu);
}

/* Hit marker drawn into the presented frame: an X of four short diagonal strokes around the picture's centre, white for a
 * hit and red for a kill. The game side calls nv2a_gpu_hud_hitmark(kind) once per hit (any thread; lock-free); the present
 * thread draws it for HM_MS, popping in and fading out. It shares the bar's blend state and constant buffer. */
#define HM_MS 320
static volatile LONG s_hm_kind;          /* 0 none, 1 white (hit), 2 red (kill) */
static volatile DWORD s_hm_t0;
static ID3D11PixelShader *s_hm_ps;
static int s_hm_state;                   /* 0 not tried, 1 up, -1 failed */

void nv2a_gpu_hud_hitmark(int kind)
{
    s_hm_t0 = GetTickCount();
    s_hm_kind = kind == 2 ? 2 : 1;
}

static const char k_hm_hlsl[] =
    "cbuffer P : register(b1) { float4 pb; };\n"        /* size in pixels, stroke half-width in pixels, kind (1 white, 2 red), alpha */
    "float4 psx(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target\n"
    "{\n"
    "    float2 q = abs(uv - 0.5) * 2.0;\n"                                         /* one quadrant, 0..1 each way */
    "    float t = (q.x + q.y) * 0.70710678;\n"                                      /* distance along the diagonal */
    "    float2 c = clamp(t, 0.30, 0.92) * float2(0.70710678, 0.70710678);\n"       /* stroke runs from 0.30 to 0.92 */
    "    float d = length(q - c) * pb.x * 0.5;\n"                                    /* pixels from the stroke's centre line */
    "    float core = saturate(pb.y + 0.5 - d);\n"
    "    float glow = saturate(1.0 - (d - pb.y) / (pb.y * 2.2 + 1.0)) * 0.30;\n"
    "    float3 col = pb.z > 1.5 ? float3(0.95, 0.10, 0.08) : float3(1.0, 1.0, 1.0);\n"
    "    float a = saturate(core + glow * (1.0 - core)) * pb.w;\n"
    "    return float4(lerp(float3(0.0, 0.0, 0.0), col, saturate(core + 0.55)), a);\n"
    "}\n";

static int hud_mark_init(void)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr;

    if (s_hm_state)
        return s_hm_state > 0;
    s_hm_state = -1;
    if (!hud_bar_init())                     /* the blend state and the constant buffer live there */
        return 0;
    hr = D3DCompile(k_hm_hlsl, sizeof k_hm_hlsl - 1, "hud_mark", NULL, NULL, "psx", "ps_4_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[NV2A-GPU] hit marker: compile failed (0x%08lX)\n%s\n", (unsigned long)hr,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        if (err) ID3D10Blob_Release(err);
        return 0;
    }
    if (err) ID3D10Blob_Release(err);
    hr = ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                        ID3D10Blob_GetBufferSize(code), NULL, &s_hm_ps);
    ID3D10Blob_Release(code);
    if (FAILED(hr))
        return 0;
    s_hm_state = 1;
    return 1;
}

static void hud_mark_draw(const D3D11_VIEWPORT *frame)
{
    D3D11_VIEWPORT vp;
    float pb[4], side, age, a, pop;
    LONG kind = s_hm_kind;

    if (!kind)
        return;
    age = (float)(int)(GetTickCount() - s_hm_t0);
    if (age < 0.0f || age >= (float)HM_MS) {
        s_hm_kind = 0;
        return;
    }
    if (!hud_mark_init())
        return;
    pop = age < 70.0f ? 1.0f + 0.25f * (1.0f - age / 70.0f) : 1.0f;                 /* a quick pop-in */
    a = age < 190.0f ? 1.0f : 1.0f - (age - 190.0f) / ((float)HM_MS - 190.0f);        /* then a fade */
    side = (float)(int)(frame->Height * 0.058f * pop + 0.5f);
    if (side < 12.0f)
        return;
    vp.TopLeftX = (float)(int)(frame->TopLeftX + frame->Width * 0.5f - side * 0.5f + 0.5f);
    vp.TopLeftY = (float)(int)(frame->TopLeftY + frame->Height * 0.5f - side * 0.5f + 0.5f);
    vp.Width = side;
    vp.Height = side;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    pb[0] = side;
    pb[1] = side * 0.030f > 1.0f ? side * 0.030f : 1.0f;
    pb[2] = (float)kind;
    pb[3] = a;
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_hb_cb, 0, NULL, pb, 0, 0);
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, s_hb_bs, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_hm_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 1, 1, &s_hb_cb);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFFu);
}

int nv2a_gpu_present(uint32_t va, uint32_t logical_w, uint32_t logical_h,
                     float aa_sx, float aa_sy, void *hwnd_)
{
    HWND hwnd = (HWND)hwnd_;
    GpuSurf *s = NULL;
    RECT r;
    UINT cw, ch;
    int i;

    if (s_on <= 0 || !hwnd || !logical_w || !logical_h || !s_present_state)
        return 0;
    va &= 0x0FFFFFFFu;
    for (i = 0; i < GPU_SURFS; i++) {
        GpuSurf *c = &s_surf[i];
        if (c->used && !c->d.swizzled && surf_lo(&c->d) == va
            && (!s || c->d.height > s->d.height))
            s = c;
    }
    if (!s)
        return 0;
    if (s_present_state < 0) {
        s_present_state = present_init(hwnd);
        if (!s_present_state)
            return 0;
    }
    if (s->stale)
        surf_upload(s);         /* the CPU drew it last: the GPU copy follows */
    if (!s->srv && FAILED(ID3D11Device_CreateShaderResourceView(
                               s_dev, (ID3D11Resource *)s->tex, NULL, &s->srv)))
        return 0;

    GetClientRect(hwnd, &r);
    cw = (UINT)(r.right - r.left);
    ch = (UINT)(r.bottom - r.top);
    if (!cw || !ch)
        return 1;                               /* minimised: nothing to show */
    if (cw != s_sc_w || ch != s_sc_h || !s_sc_rtv) {
        ID3D11Texture2D *bb = NULL;
        if (s_sc_rtv) {
            ID3D11RenderTargetView_Release(s_sc_rtv);
            s_sc_rtv = NULL;
        }
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
        bind_forget();
        ID3D11DeviceContext_Flush(s_ctx);
        if (FAILED(IDXGISwapChain1_ResizeBuffers(s_sc, 0, cw, ch,
                                                 DXGI_FORMAT_UNKNOWN, 0))
            || FAILED(IDXGISwapChain1_GetBuffer(s_sc, 0, &IID_ID3D11Texture2D,
                                                (void **)&bb)))
            return 0;
        if (FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)bb,
                                                       NULL, &s_sc_rtv))) {
            ID3D11Texture2D_Release(bb);
            return 0;
        }
        ID3D11Texture2D_Release(bb);
        s_sc_w = cw;
        s_sc_h = ch;
    }
    {
        /* The frame letterboxed into the client area, as the window thread
         * drew it; the part of the surface the frame covers is its logical
         * size times the anti-aliasing scale. */
        static const float black[4] = { 0, 0, 0, 1 };
        D3D11_VIEWPORT vp;
        float uv[4];
        UINT w, h;
        ID3D11ShaderResourceView *none = NULL;

        /* RECOMP_WIDESCREEN=1: the title draws 16:9 squeezed into the
         * frame, so it is shown 16:9. */
        static int wide = -1;
        float asp;
        if (wide < 0) {
            const char *e = getenv("RECOMP_WIDESCREEN");
            wide = e && e[0] == '1';
        }
        asp = wide ? 16.0f / 9.0f : (float)logical_w / (float)logical_h;
        recomp_wide_client(cw, ch);
        asp = recomp_present_aspect(asp);
        if ((float)cw <= (float)ch * asp) {
            w = cw;
            h = (UINT)((float)cw / asp + 0.5f);
        } else {
            h = ch;
            w = (UINT)((float)ch * asp + 0.5f);
        }
        uv[0] = logical_w * aa_sx / (float)s->d.width;
        uv[1] = logical_h * aa_sy / (float)s->d.height;
        uv[2] = uv[3] = 0.0f;
        if (uv[0] > 1.0f) uv[0] = 1.0f;
        if (uv[1] > 1.0f) uv[1] = 1.0f;
        wpn_env_capture(s->srv, uv);
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_blit_cb,
                                              0, NULL, uv, 0, 0);
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_sc_rtv, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_sc_rtv, black);
        vp.TopLeftX = (float)((cw - w) / 2);
        vp.TopLeftY = (float)((ch - h) / 2);
        vp.Width = (float)w;
        vp.Height = (float)h;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
        ID3D11DeviceContext_RSSetState(s_ctx, NULL);
        ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFFu);
        ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, NULL, 0);
        ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(s_ctx, s_blit_vs, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_blit_cb);
        ID3D11DeviceContext_PSSetShader(s_ctx, g_gpu_fxaa && s_fxaa_ps ? s_fxaa_ps : s_blit_ps, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_blit_cb);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &s->srv);
        ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_blit_smp);
        ID3D11DeviceContext_Draw(s_ctx, 3, 0);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
        hud_bar_draw(&vp);
        hud_mark_draw(&vp);
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
        bind_forget();
    }
    if (InterlockedExchange(&g_screenshot_request, 0))
        shot_take(s_sc);
    {
        HRESULT phr = IDXGISwapChain1_Present(s_sc, 0, 0);
        if (FAILED(phr)) {      /* a driver reset or a lost GPU after a long session looks like a silent close: say so, once */
            static int said;
            if (!said) {
                said = 1;
                fprintf(stderr, "[DXGI] Present failed 0x%08lX; device removed reason 0x%08lX\n", (unsigned long)phr,
                        (unsigned long)ID3D11Device_GetDeviceRemovedReason(s_dev));
                fflush(stderr);
            }
        }
    }
    return 1;
}

#else   /* !_WIN32: no D3D11, the software rasteriser draws everything */

int  nv2a_gpu_enabled(void) { return 0; }
void nv2a_gpu_draw(const Nv2aGpuState *st, const Nv2aGpuVertex *v, uint32_t n)
{ (void)st; (void)v; (void)n; }
void nv2a_gpu_clear(const Nv2aGpuSurface *s, uint32_t z, uint32_t f, uint32_t c,
                    float d, uint32_t st, uint32_t x, uint32_t y, uint32_t w,
                    uint32_t h)
{ (void)s; (void)z; (void)f; (void)c; (void)d; (void)st; (void)x; (void)y; (void)w; (void)h; }
void nv2a_gpu_sync_range(uint32_t va, uint32_t bytes) { (void)va; (void)bytes; }
void nv2a_gpu_cpu_write(uint32_t va, uint32_t bytes, int after)
{ (void)va; (void)bytes; (void)after; }
uint32_t nv2a_gpu_zpass_collect(void) { return 0; }
void nv2a_gpu_frame(void) {}
void nv2a_gpu_weapon_get_tan(float *h, float *v) { *h = *v = 0.0f; }
void nv2a_gpu_weapon_config(int level) { (void)level; }
int  nv2a_gpu_weapon_level(void) { return 0; }
void nv2a_gpu_weapon_view(float h, float v) { (void)h; (void)v; }
void nv2a_gpu_weapon_trigger(int k, float s) { (void)k; (void)s; }
void nv2a_gpu_weapon_rain(float s) { (void)s; }
void nv2a_gpu_weapon_clean(void) {}
void nv2a_gpu_weapon_blood_color(float r, float g, float b) { (void)r; (void)g; (void)b; }
void nv2a_gpu_weapon_offset(float x, float y) { (void)x; (void)y; }
void nv2a_gpu_weapon_aim(float f) { (void)f; }
int nv2a_gpu_present(uint32_t va, uint32_t lw, uint32_t lh, float sx, float sy,
                     void *hwnd)
{ (void)va; (void)lw; (void)lh; (void)sx; (void)sy; (void)hwnd; return 0; }
void nv2a_gpu_present_stop(void) {}
void nv2a_gpu_hud_bar(float x, float y, float h, float aspect, float fill)
{ (void)x; (void)y; (void)h; (void)aspect; (void)fill; }
void nv2a_gpu_hud_hitmark(int kind) { (void)kind; }
float recomp_wide_target(void) { return 0.0f; }
void recomp_wide_frame(float asp) { (void)asp; }
float recomp_present_aspect(float dflt) { return dflt; }
float recomp_wide_hud_k(void) { return 0.0f; }
void recomp_wide_set_gate(int (*gate)(void)) { (void)gate; }
void recomp_wide_client(unsigned cw, unsigned ch) { (void)cw; (void)ch; }
int nv2a_gpu_program_ok(void) { return 0; }
int nv2a_gpu_program_capture(float *out) { (void)out; return 0; }
void nv2a_gpu_draw_program(const Nv2aGpuState *st, const Nv2aGpuProgram *pg,
                           uint32_t n, const uint32_t *idx, uint32_t ni)
{ (void)st; (void)pg; (void)n; (void)idx; (void)ni; }
void nv2a_gpu_draw_indexed(const Nv2aGpuState *st, const Nv2aGpuVertex *v,
                           uint32_t n, const uint32_t *idx, uint32_t ni)
{ (void)st; (void)v; (void)n; (void)idx; (void)ni; }

#endif
