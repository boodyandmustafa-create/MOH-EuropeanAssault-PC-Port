/*
 * NV2A pixel work on the host GPU (Direct3D 11).
 *
 * The pushbuffer executor (nv2a_pb_exec.c) still decodes the command stream
 * and runs vertex programs on the CPU; with RECOMP_GPU set, the triangles it
 * produces -- surface pixels, 1/w, both colours, fog and all four texture
 * coordinates -- are drawn here instead of by the software rasteriser. The
 * register combiners run in one pixel shader that interprets the program
 * nv2a_rc_compile decoded (the same semantics as nv2a_rc_run, read off the
 * same structure), so a title's combiner setups need no shader generation.
 *
 * Guest memory stays the reference copy of every surface: a colour surface
 * is uploaded when the GPU first draws into it (or when the CPU wrote it
 * since), and read back whenever the guest side needs the pixels -- the flip
 * that shows a frame, a texture fetch from a surface the GPU drew, a CPU
 * write into it. So everything downstream of guest memory (the window, the
 * frame dumps, render-to-texture) keeps working unchanged.
 *
 * Everything here runs on the executor's thread.
 */
#ifndef NV2A_GPU_H
#define NV2A_GPU_H

#include <stdint.h>
#include "nv2a_backend.h"     /* Nv2aRenderState */
#include "nv2a_combiner.h"    /* Nv2aRcProg */

/* One vertex as the executor hands it over: x, y in surface pixels, z in
 * depth-buffer units, w the clip w (perspective-correct interpolation of the
 * texture coordinates divides by it, as the software path does). */
typedef struct {
    float pos[4];
    float d0[4], d1[4];
    float fog[4];          /* x: fog factor 0..1 (fog_factor of oFog.x) */
    float tex[4][4];
} Nv2aGpuVertex;

/* A colour surface in guest memory. */
typedef struct {
    uint32_t va;           /* guest address of pixel (0,0), dma-resolved */
    uint32_t pitch, bpp;   /* bpp 2 (R5G6B5) or 4 (A8R8G8B8) */
    uint32_t width, height;
    int      swizzled;     /* then width/height are the swizzled image's */
} Nv2aGpuSurface;

/* A texture stage's image, decoded to 0xAARRGGBB with its mip levels. The
 * slot/version pair identifies the decode: same pair, same texels. */
typedef struct {
    int valid;
    uint32_t slot, version;
    const uint32_t *argb;
    uint32_t levels;
    const uint32_t *lv_off, *lv_w, *lv_h;
    uint32_t addr_u, addr_v;          /* NV097 wrap modes */
    uint32_t border;                  /* 0xAARRGGBB */
    int border_color;
    uint32_t mag, min;                /* SET_TEXTURE_FILTER fields */
    int normalized;                   /* coordinates 0..1, else texels */
    /* Render-to-texture: the image is a surface the GPU drew (see
     * nv2a_gpu_surface_texture), sampled where it is, no read-back. */
    int from_surface;
    uint32_t surf_va, surf_bpp, surf_w, surf_h, surf_pitch;
    int surf_swizzled, force_alpha;   /* force_alpha: an X8 format */
} Nv2aGpuTexture;

typedef struct {
    Nv2aGpuSurface color;
    uint32_t zeta_va;                 /* 0: the executor's anonymous one */
    float    zscale;                  /* 16777215 (Z24) or 65535 (Z16) */
    uint32_t clip_x, clip_y, clip_w, clip_h;
    Nv2aRenderState rs;
    int use_rc, textured;             /* textured: the fixed path's stage 0 */
    const Nv2aRcProg *prog;
    uint32_t prog_ver;                /* changes with prog's contents; 0 = unknown */
    uint32_t stage_program, clip_plane_mode;
    float fog_color[4];
    uint32_t alpha_test, alpha_func, alpha_ref;
    int zpass;                        /* count passing pixels (GET_REPORT) */
    int trace;                        /* a RECOMP_FRAME_TRACE frame */
    Nv2aGpuTexture tex[4];
} Nv2aGpuState;

/* RECOMP_GPU set, and the device could be created. */
int  nv2a_gpu_enabled(void);

void nv2a_gpu_draw(const Nv2aGpuState *st, const Nv2aGpuVertex *v,
                   uint32_t nverts);
/* A vertex program for the GPU to run (nv2a_gpu_draw_program): the NV2A
 * program and constants (nv2a_vsh_program / nv2a_vsh_constants), and each
 * vertex's inputs, nused float4s a vertex in vin, input register r at
 * slot[r]. The rest is what the executor applies after the program. */
typedef struct {
    const uint32_t (*prog)[4];
    uint32_t nins, prog_version;
    const float (*consts)[4];
    uint32_t const_version;
    const float (*vin)[4];
    uint32_t nused;
    uint32_t slot[16];
    float aa_sx, aa_sy;
    float near_o2;              /* the near plane: z >= near_o2 */
    int near_z;
    int fog_enable;
    uint32_t fog_mode;
    float fog_param[2];
    float hud_k;                /* ultrawide HUD batch: x around the picture's centre times this (0 = off) */
    float hud_shift;            /* ... and moved by this fraction of the picture width (edge anchoring, per batch) */
    int weapon;                 /* a first-person weapon draw (inside the stream markers of the weapon pass): lit by nv2a_gpu_weapon_* */
} Nv2aGpuProgram;

/* WEAPON LIGHTING (the first-person weapon, like a modern shooter): the game side puts a marker (a NO_OPERATION method with
 * magic 0x57504Cxx) into the pushbuffer where the weapon pass begins and ends; the executor sets g_nv_wpn_pass while it is
 * between them and the weapon's opaque draws get a pixel shader variant that adds environment light and reflection (from a
 * 256x144 mip copy of the last presented frame) and dirt, blood and rain drops. level 0 = off, 1..3 = low / normal / high. */
extern volatile int g_nv_wpn_pass;
void nv2a_gpu_weapon_config(int level);
int  nv2a_gpu_weapon_level(void);
void nv2a_gpu_weapon_view(float tan_h, float tan_v);   /* tan of the weapon pass projection's half angles, across / down (game side) */
void nv2a_gpu_weapon_trigger(int kind, float strength);   /* 0 = blood, 1 = dust; any thread */
void nv2a_gpu_weapon_rain(float strength);                /* 0 = dry; any thread, every ~100 ms while it rains */
void nv2a_gpu_weapon_get_tan(float *h, float *v);          /* what the weapon shader uses (for the log) */
void nv2a_gpu_weapon_clean(void);                         /* wipe the weapon (a level load) */
void nv2a_gpu_weapon_blood_color(float r, float g, float b);   /* the blood colour setting, 0..1 */
void nv2a_gpu_weapon_frame_end(void);                     /* every flip: the blink detector (nv2a_pb_exec.c) */
void nv2a_gpu_weapon_aim(float f);                       /* 0 = hip, 1 = aiming down the sights (the gun is in the middle of the view) */
void nv2a_gpu_weapon_offset(float x, float y);            /* where the gun sits on the screen now, in view units (0.45 m * angle): the drops and blood follow it */

/* RECOMP_GPU_VS_CHECK: the next nv2a_gpu_draw_program also writes its
 * vertices' outputs to out, 34 floats each (pos as the rasteriser gets it,
 * d0, d1, fog, t0..t3, the two clip distances). 0: not available. */
int nv2a_gpu_program_capture(float *out);

/* Whether the GPU can run vertex programs (nv2a_gpu_draw_program). */
int nv2a_gpu_program_ok(void);
/* nverts vertices' inputs in pg->vin; idx: nidx indices, three a triangle. */
void nv2a_gpu_draw_program(const Nv2aGpuState *st, const Nv2aGpuProgram *pg,
                           uint32_t nverts, const uint32_t *idx, uint32_t nidx);

/* The same, each vertex sent once: idx holds nidx indices into v, three to
 * a triangle. */
void nv2a_gpu_draw_indexed(const Nv2aGpuState *st, const Nv2aGpuVertex *v,
                           uint32_t nverts, const uint32_t *idx, uint32_t nidx);

/* CLEAR_SURFACE: flags as the method (Z 0x1, stencil 0x2, colour 0xF0). */
void nv2a_gpu_clear(const Nv2aGpuSurface *s, uint32_t zeta_va, uint32_t flags,
                    uint32_t argb, float depth01, uint32_t stencil,
                    uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* Write back every surface the GPU drew that overlaps guest bytes
 * [va, va + bytes), so the guest copy is current. */
void nv2a_gpu_sync_range(uint32_t va, uint32_t bytes);

/* The CPU is about to write / has written guest bytes [va, va + bytes):
 * before, pending GPU writes are synced; after, any GPU copy of them is
 * re-uploaded before it is drawn into again. */
void nv2a_gpu_cpu_write(uint32_t va, uint32_t bytes, int after);

/* Whether a texture at guest address va (texel size bpp, w x h, pitch for
 * a linear one) can be sampled straight from a surface the GPU holds. */
int nv2a_gpu_surface_texture(uint32_t va, uint32_t bpp, uint32_t w, uint32_t h,
                             uint32_t pitch, int swizzled);

/* Pixels that passed the depth test in zpass draws since the last call. */
uint32_t nv2a_gpu_zpass_collect(void);

/* A new frame: once-per-frame checks are due again. */
void nv2a_gpu_frame(void);

/* The flip: show the surface at va (logical_w x logical_h, times the
 * anti-aliasing scale) in the window hwnd from the GPU. 1 when it did; 0 to
 * fall back on reading it back for the window thread. */
int nv2a_gpu_present(uint32_t va, uint32_t logical_w, uint32_t logical_h,
                     float aa_sx, float aa_sy, void *hwnd);
void nv2a_gpu_present_stop(void);   /* back to the window thread */
/* HUD bar drawn into the presented picture (any thread, call every frame it is wanted; gone 200 ms after the last call).
 * x, y = top-left, h = height, aspect = width / height, all fractions of the picture; fill 0..1 rises from the bottom. */
void nv2a_gpu_hud_bar(float x, float y, float h, float aspect, float fill);
/* Hit marker drawn into the presented picture: an X at the centre for about 320 ms; kind 1 = white (hit), 2 = red (kill). */
void nv2a_gpu_hud_hitmark(int kind);

/* Ultrawide (RECOMP_ASPECT = "21:9", "2.37" or "auto"; see nv2a_gpu.c). recomp_wide_target: the configured gameplay shape (0 =
 * plain 16:9). recomp_wide_frame: the game side says the gameplay picture it is drawing now has this shape (0 = normal).
 * recomp_present_aspect: the presenters' shape for the frame (dflt without ultrawide). recomp_wide_set_gate: a callback that
 * returns 0 while a menu is up (the picture is then 16:9). recomp_wide_client: the presenters report the window's client size. */
float recomp_wide_target(void);
void  recomp_wide_frame(float asp);
float recomp_present_aspect(float dflt);
float recomp_wide_hud_k(void);   /* 2D HUD squeeze (and edge anchoring) in a wide picture (0 = none) */
void  recomp_wide_set_gate(int (*gate)(void));
void  recomp_wide_client(unsigned cw, unsigned ch);

#endif /* NV2A_GPU_H */
