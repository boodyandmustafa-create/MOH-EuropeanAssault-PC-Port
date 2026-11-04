/**
 * NV2A register combiners. See nv2a_combiner.h.
 *
 * Register numbers (an input or output byte's low nibble):
 *   0 zero/discard  1 c0  2 c1  3 fog  4 v0  5 v1  8-11 t0-t3  12 r0  13 r1
 *   14 v1+r0 sum and 15 E*F product (final combiner only)
 * An input byte is reg | alpha<<4 | mapping<<5. In the RGB portion the alpha
 * bit replicates .a; in the alpha portion it picks .a over .b.
 */
#include "nv2a_combiner.h"

#include <math.h>
#include <string.h>

enum { R_ZERO = 0, R_C0 = 1, R_C1 = 2, R_FOG = 3, R_V0 = 4, R_V1 = 5,
       R_T0 = 8, R_R0 = 12, R_R1 = 13, R_SUM = 14, R_EF = 15 };

void nv2a_rc_unpack(uint32_t c, float o[4])
{
    o[0] = (float)((c >> 16) & 0xFF) / 255.0f;
    o[1] = (float)((c >>  8) & 0xFF) / 255.0f;
    o[2] = (float)( c        & 0xFF) / 255.0f;
    o[3] = (float)( c >> 24        ) / 255.0f;
}

static float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

/* PS_INPUTMAPPING (psh.c get_input_var), decoded once per program: every
 * mapping is a clamp followed by a scale and bias, so the pixel loop runs one
 * form for all eight instead of a switch per channel. */
static void rc_in(Nv2aRcIn *in, uint32_t byte, int alpha_portion)
{
    static const float lo[8] = { 0, 0, 0, 0, 0, 0, -INFINITY, -INFINITY };
    static const float hi[8] = { INFINITY, 1, INFINITY, INFINITY, INFINITY,
                                 INFINITY, INFINITY, INFINITY };
    static const float sc[8] = { 1, -1, 2, -2, 1, -1, 1, -1 };
    static const float bi[8] = { 0, 1, -1, 1, -0.5f, 0.5f, 0, 0 };
    uint32_t m = (byte >> 5) & 7;
    in->reg = (uint8_t)(byte & 0xF);
    /* rep 1: every channel reads .a. In the alpha portion the alpha bit
     * picks .a over .b, so there rep is 1 (.a) or 2 (.b). */
    if (alpha_portion)
        in->rep = (byte & 0x10) ? 1 : 2;
    else
        in->rep = (byte & 0x10) ? 1 : 0;
    in->lo = lo[m]; in->hi = hi[m]; in->s = sc[m]; in->b = bi[m];
}

/* PS_COMBINEROUTPUT scale/bias as (x + pre) * mul, psh.c get_output. */
static void rc_out(uint32_t mapping, float *pre, float *mul)
{
    switch (mapping) {
    case 0x08: *pre = -0.5f; *mul = 1.0f; break;
    case 0x10: *pre = 0.0f;  *mul = 2.0f; break;
    case 0x18: *pre = -0.5f; *mul = 2.0f; break;
    case 0x20: *pre = 0.0f;  *mul = 4.0f; break;
    case 0x30: *pre = 0.0f;  *mul = 0.5f; break;
    default:   *pre = 0.0f;  *mul = 1.0f; break;
    }
}

void nv2a_rc_compile(const Nv2aCombiner *rc, Nv2aRcProg *p)
{
    uint32_t flags = rc->control >> 8, s;

    memset(p, 0, sizeof *p);
    p->n = rc->control & 0xFF;
    if (p->n > 8)
        p->n = 8;
    p->mux_msb = flags & 1;
    p->stage0_on = (rc->stage_program & 0x1F) != 0;
    for (s = 0; s < p->n; s++) {
        Nv2aRcStage *st = &p->st[s];
        uint32_t icw = rc->color_icw[s], ocw = rc->color_ocw[s];
        uint32_t aicw = rc->alpha_icw[s], aocw = rc->alpha_ocw[s];
        /* One c0/c1 for every stage, or one per stage (COMBINERCOUNT). */
        nv2a_rc_unpack(rc->factor0[(flags & 0x010) ? s : 0], st->c0);
        nv2a_rc_unpack(rc->factor1[(flags & 0x100) ? s : 0], st->c1);
        rc_in(&st->A, icw >> 24, 0); rc_in(&st->B, icw >> 16, 0);
        rc_in(&st->C, icw >> 8, 0);  rc_in(&st->D, icw, 0);
        rc_in(&st->aA, aicw >> 24, 1); rc_in(&st->aB, aicw >> 16, 1);
        rc_in(&st->aC, aicw >> 8, 1);  rc_in(&st->aD, aicw, 1);
        st->fl = ocw >> 12;
        st->afl = aocw >> 12;
        rc_out(st->fl & 0x38, &st->pre, &st->mul);
        rc_out(st->afl & 0x38, &st->apre, &st->amul);
        st->dst_ab = (uint8_t)((ocw >> 4) & 0xF);
        st->dst_cd = (uint8_t)(ocw & 0xF);
        st->dst_sum = (uint8_t)((ocw >> 8) & 0xF);
        st->adst_ab = (uint8_t)((aocw >> 4) & 0xF);
        st->adst_cd = (uint8_t)(aocw & 0xF);
        st->adst_sum = (uint8_t)((aocw >> 8) & 0xF);
    }
    p->has_final = rc->final0 || rc->final1;
    if (p->has_final) {
        uint32_t f0 = rc->final0, f1 = rc->final1;
        p->fflags = f1 & 0xFF;
        nv2a_rc_unpack(rc->final_c0, p->fc0);
        nv2a_rc_unpack(rc->final_c1, p->fc1);
        rc_in(&p->E, f1 >> 24, 0); rc_in(&p->F, f1 >> 16, 0);
        rc_in(&p->A, f0 >> 24, 0); rc_in(&p->B, f0 >> 16, 0);
        rc_in(&p->C, f0 >> 8, 0);  rc_in(&p->D, f0, 0);
        rc_in(&p->G, f1 >> 8, 1);
    }
}

static inline float rc_map(const Nv2aRcIn *in, float x)
{
    float p = x < in->lo ? in->lo : x;
    p = p > in->hi ? in->hi : p;
    return in->s * p + in->b;
}

static inline void rc_rgb(float R[16][4], const Nv2aRcIn *in, float v[3])
{
    const float *r = R[in->reg];
    if (in->rep) {
        float a = rc_map(in, r[3]);
        v[0] = v[1] = v[2] = a;
    } else {
        v[0] = rc_map(in, r[0]);
        v[1] = rc_map(in, r[1]);
        v[2] = rc_map(in, r[2]);
    }
}

static inline float rc_alpha(float R[16][4], const Nv2aRcIn *in)
{
    return rc_map(in, R[in->reg][in->rep == 1 ? 3 : 2]);
}

static inline float rc_res(float x, float pre, float mul)
{
    return clampf((x + pre) * mul, -1.0f, 1.0f);
}

void nv2a_rc_run(const Nv2aRcProg *p, const float v0[4], const float v1[4],
                 const float fog[4], const float t[4][4], float out[4])
{
    float R[16][4];
    uint32_t s;
    int i;

    /* Only the registers a program can read before writing need a value:
     * zero, r0, r1 and the final-combiner specials. */
    for (i = 0; i < 4; i++) {
        R[0][i] = R[6][i] = R[7][i] = 0.0f;
        R[12][i] = R[13][i] = R[14][i] = R[15][i] = 0.0f;
        R[R_FOG][i] = fog[i];
        R[R_V0][i] = v0[i];
        R[R_V1][i] = v1[i];
        R[R_T0 + 0][i] = t[0][i];
        R[R_T0 + 1][i] = t[1][i];
        R[R_T0 + 2][i] = t[2][i];
        R[R_T0 + 3][i] = t[3][i];
    }
    /* r0.a starts as texture 0's alpha, or 1 with stage 0 off (psh.c). */
    R[R_R0][3] = p->stage0_on ? t[0][3] : 1.0f;

    for (s = 0; s < p->n; s++) {
        const Nv2aRcStage *st = &p->st[s];
        uint32_t fl = st->fl, afl = st->afl;
        float A[3], B[3], C[3], D[3], ab[3], cd[3], ms[3];
        float aA, aB, aC, aD, aab, acd, ams, dab, dcd;
        int mux_cd;

        memcpy(R[R_C0], st->c0, 16);
        memcpy(R[R_C1], st->c1, 16);
        mux_cd = p->mux_msb ? R[R_R0][3] >= 0.5f
                            : ((int)(R[R_R0][3] * 255.0f) & 1);

        /* Both portions read before either writes (psh.c emits all the
         * arithmetic, then the assignments). */
        rc_rgb(R, &st->A, A); rc_rgb(R, &st->B, B);
        rc_rgb(R, &st->C, C); rc_rgb(R, &st->D, D);
        aA = rc_alpha(R, &st->aA); aB = rc_alpha(R, &st->aB);
        aC = rc_alpha(R, &st->aC); aD = rc_alpha(R, &st->aD);

        dab = A[0] * B[0] + A[1] * B[1] + A[2] * B[2];
        dcd = C[0] * D[0] + C[1] * D[1] + C[2] * D[2];
        for (i = 0; i < 3; i++) {
            float pab = (fl & 2) ? dab : A[i] * B[i];
            float pcd = (fl & 1) ? dcd : C[i] * D[i];
            float sum = (fl & 4) ? (mux_cd ? pcd : pab) : pab + pcd;
            ab[i] = rc_res(pab, st->pre, st->mul);
            cd[i] = rc_res(pcd, st->pre, st->mul);
            ms[i] = rc_res(sum, st->pre, st->mul);
        }
        {
            float pab = aA * aB, pcd = aC * aD;
            float sum = (afl & 4) ? (mux_cd ? pcd : pab) : pab + pcd;
            aab = rc_res(pab, st->apre, st->amul);
            acd = rc_res(pcd, st->apre, st->amul);
            ams = rc_res(sum, st->apre, st->amul);
        }

        /* RGB destinations; blue-to-alpha also writes the destination's .a. */
        if (st->dst_ab) {
            float *d = R[st->dst_ab];
            d[0] = ab[0]; d[1] = ab[1]; d[2] = ab[2];
            if (fl & 0x80) d[3] = ab[2];
        }
        if (st->dst_cd) {
            float *d = R[st->dst_cd];
            d[0] = cd[0]; d[1] = cd[1]; d[2] = cd[2];
            if (fl & 0x40) d[3] = cd[2];
        }
        if (st->dst_sum) {
            float *d = R[st->dst_sum];
            d[0] = ms[0]; d[1] = ms[1]; d[2] = ms[2];
        }
        if (st->adst_ab)  R[st->adst_ab][3] = aab;
        if (st->adst_cd)  R[st->adst_cd][3] = acd;
        if (st->adst_sum) R[st->adst_sum][3] = ams;
        /* Register 0 is zero whatever was "written" to it. */
        R[R_ZERO][0] = R[R_ZERO][1] = R[R_ZERO][2] = R[R_ZERO][3] = 0.0f;
    }

    if (p->has_final) {
        uint32_t fflags = p->fflags;
        float A[3], B[3], C[3], D[3], E[3], F[3];

        memcpy(R[R_C0], p->fc0, 16);
        memcpy(R[R_C1], p->fc1, 16);
        /* V1R0 sum: optional complements (0x40 v1, 0x20 r0), clamp (0x80). */
        for (i = 0; i < 3; i++) {
            float a = (fflags & 0x40) ? 1.0f - R[R_V1][i] : R[R_V1][i];
            float b = (fflags & 0x20) ? 1.0f - R[R_R0][i] : R[R_R0][i];
            R[R_SUM][i] = (fflags & 0x80) ? clampf(a + b, 0.0f, 1.0f) : a + b;
        }
        R[R_SUM][3] = 0.0f;
        rc_rgb(R, &p->E, E);
        rc_rgb(R, &p->F, F);
        for (i = 0; i < 3; i++)
            R[R_EF][i] = E[i] * F[i];
        R[R_EF][3] = 0.0f;
        rc_rgb(R, &p->A, A); rc_rgb(R, &p->B, B);
        rc_rgb(R, &p->C, C); rc_rgb(R, &p->D, D);
        /* D + mix(C, B, A), alpha from G. */
        for (i = 0; i < 3; i++)
            out[i] = D[i] + C[i] * (1.0f - A[i]) + B[i] * A[i];
        out[3] = rc_alpha(R, &p->G);
    } else {
        for (i = 0; i < 4; i++)
            out[i] = R[R_R0][i];
    }
    for (i = 0; i < 4; i++)
        out[i] = clampf(out[i], 0.0f, 1.0f);
}

void nv2a_rc_eval(const Nv2aCombiner *rc, const float v0[4],
                  const float v1[4], const float fog[4],
                  const float t[4][4], float out[4])
{
    Nv2aRcProg p;
    nv2a_rc_compile(rc, &p);
    nv2a_rc_run(&p, v0, v1, fog, t, out);
}
