/**
 * NV2A register combiners, evaluated per pixel for the software rasteriser.
 *
 * Every pixel the NV2A writes goes through its register combiners: up to
 * eight general stages and a final stage that mix the vertex colours, the
 * four texture results, two constants per stage and fog. D3D's fixed-function
 * texture stages compile to them too, so a title never draws without them.
 * The executor used to stand in with "texel * diffuse" from stage 0, which is
 * right for a menu quad and wrong for anything that mixes two textures --
 * Burnout 3 composites its whole 3D scene into the frame that way.
 *
 * The encoding (ICW/OCW bit layout, input mappings, output scale/bias, mux,
 * final combiner) follows xemu's pixel-shader generator
 * (hw/xbox/nv2a/pgraph/glsl/psh.c); the comments in nv2a_combiner.c say where
 * each rule comes from. Portable C, no graphics API.
 */
#ifndef XBOXRECOMP_NV2A_COMBINER_H
#define XBOXRECOMP_NV2A_COMBINER_H

#include <stdint.h>

typedef struct {
    uint32_t color_icw[8], alpha_icw[8];    /* SET_COMBINER_COLOR/ALPHA_ICW */
    uint32_t color_ocw[8], alpha_ocw[8];    /* SET_COMBINER_COLOR/ALPHA_OCW */
    uint32_t factor0[8], factor1[8];        /* SET_COMBINER_FACTOR0/1 (ARGB) */
    uint32_t final0, final1;                /* SET_COMBINER_SPECULAR_FOG_CW0/1 */
    uint32_t final_c0, final_c1;            /* SET_SPECULAR_FOG_FACTOR */
    uint32_t control;                       /* SET_COMBINER_CONTROL */
    uint32_t stage_program;                 /* SET_SHADER_STAGE_PROGRAM */
} Nv2aCombiner;

/* One pixel. Colours are r,g,b,a in [0,1]; `fog` is (fog colour, factor).
 * t[i] is what texture stage i produced. The result is clamped to [0,1].
 * Compiles the program every call; per-pixel loops want the pair below. */
void nv2a_rc_eval(const Nv2aCombiner *rc, const float v0[4],
                  const float v1[4], const float fog[4],
                  const float t[4][4], float out[4]);

/* An input after decoding: register, component source, and the input
 * mapping as clamp-then-scale-and-bias, y = s * clamp(x, lo, hi) + b. */
typedef struct {
    uint8_t reg, rep;               /* rep: read .a (or .b) for every channel */
    float lo, hi, s, b;
} Nv2aRcIn;

typedef struct {
    Nv2aRcIn A, B, C, D, aA, aB, aC, aD;
    uint32_t fl, afl;               /* OCW flags (dot, mux, blue-to-alpha) */
    float pre, mul, apre, amul;     /* output scale/bias: (x + pre) * mul */
    uint8_t dst_ab, dst_cd, dst_sum, adst_ab, adst_cd, adst_sum;
    float c0[4], c1[4];
} Nv2aRcStage;

/* A combiner program decoded once, for every pixel of a batch. */
typedef struct {
    uint32_t n, mux_msb, stage0_on, has_final, fflags;
    Nv2aRcStage st[8];
    Nv2aRcIn A, B, C, D, E, F, G;
    float fc0[4], fc1[4];
} Nv2aRcProg;

void nv2a_rc_compile(const Nv2aCombiner *rc, Nv2aRcProg *p);
void nv2a_rc_run(const Nv2aRcProg *p, const float v0[4], const float v1[4],
                 const float fog[4], const float t[4][4], float out[4]);

/* D3DCOLOR (0xAARRGGBB) to r,g,b,a floats. */
void nv2a_rc_unpack(uint32_t argb, float out[4]);

#endif
