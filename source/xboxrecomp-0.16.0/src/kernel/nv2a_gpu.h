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
} Nv2aGpuProgram;

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

#endif /* NV2A_GPU_H */
