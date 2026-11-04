/**
 * NV2A vertex program interpreter. See nv2a_vsh_interp.h for why it exists.
 *
 * Encoding. An instruction is four dwords; dword 0 is unused. Fields, as
 * (dword, lowest bit, width):
 *
 *   ILU op        1 25 3      MAC op        1 21 4
 *   const index   1 13 8      input index   1  9 4   (shared by A, B, C)
 *   A negate      1  8 1      A swizzle     1  0 8   (x y z w, 2 bits each,
 *   A temp        2 28 4      A mux         2 26 2    x highest)
 *   B negate      2 25 1      B swizzle     2 17 8
 *   B temp        2 13 4      B mux         2 11 2
 *   C negate      2 10 1      C swizzle     2  2 8
 *   C temp        2 0 2 (high) : 3 30 2 (low)          C mux 3 28 2
 *   MAC temp mask 3 24 4      temp dest     3 20 4
 *   ILU temp mask 3 16 4      out mask      3 12 4
 *   out to o[]    3 11 1      out address   3  3 8   (0: to c[])
 *   out from ILU  3  2 1      a0.x relative 3  1 1   final 3 0 1
 *
 * Masks are x=8 y=4 z=2 w=1. A source mux of 1 reads a temp, 2 an input,
 * 3 a constant. Temp 12 is oPos. When an instruction carries both a MAC and
 * an ILU op, the ILU's temp write goes to R1 whatever the temp field says,
 * and both units read their sources before either writes.
 *
 * The layout is checked against real programs rather than taken on trust:
 * RECOMP_VSH_DUMP disassembles each program the title uploads, and an Xbox
 * D3D program is recognisable -- DP4s against the matrix constants, ending in
 * the runtime's screen-space epilogue (RCC of R12.w, then a MAD into oPos).
 */
#include "nv2a_vsh_interp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t s_program[NV2A_VSH_SLOTS][4];
static float    s_const[NV2A_VSH_CONSTANTS][4];
static uint32_t s_load_slot, s_load_word, s_start_slot;
static uint32_t s_const_load, s_const_word;
static uint32_t s_cxt_write;
static uint32_t s_prog_version = 1;    /* bumped whenever s_program changes */
static uint32_t s_const_version = 1;   /* bumped whenever s_const changes */
static int      s_const_trace = -1;      /* RECOMP_VSH_DUMP: constant traffic */

static int vsh_dump_enabled(void)
{
    if (s_const_trace < 0)
        s_const_trace = getenv("RECOMP_VSH_DUMP") != NULL;
    return s_const_trace;
}

static uint32_t field(const uint32_t *ins, int dw, int lo, int width)
{
    return (ins[dw] >> lo) & ((1u << width) - 1u);
}

/* ---- uploads ------------------------------------------------------------ */

void nv2a_vsh_set_load_slot(uint32_t slot)
{
    s_load_slot = slot;
    s_load_word = 0;
}

void nv2a_vsh_program_word(uint32_t word)
{
    if (s_load_slot < NV2A_VSH_SLOTS) {
        /* A title reloads the program it already has before many draws:
         * only a changed word makes a new version (a new version is a
         * re-decode here and a re-upload to the GPU). */
        if (s_program[s_load_slot][s_load_word] != word) {
            s_program[s_load_slot][s_load_word] = word;
            s_prog_version++;
        }
    }
    if (++s_load_word == 4) {
        s_load_word = 0;
        s_load_slot++;
    }
}

void nv2a_vsh_set_start_slot(uint32_t slot)
{
    if (s_start_slot != slot) {
        s_start_slot = slot;
        s_prog_version++;
    }
}
void nv2a_vsh_set_cxt_write(uint32_t enable) { s_cxt_write = enable; }
int nv2a_vsh_cxt_write(void) { return s_cxt_write != 0; }

void nv2a_vsh_set_constant_load(uint32_t index)
{
    s_const_load = index;
    s_const_word = 0;
    if (vsh_dump_enabled())
        fprintf(stderr, "  [VSH] constant load -> c[%u]\n", index);
}

void nv2a_vsh_constant_word(uint32_t word)
{
    if (s_const_load < NV2A_VSH_CONSTANTS) {
        if (memcmp(&s_const[s_const_load][s_const_word], &word, 4)) {
            memcpy(&s_const[s_const_load][s_const_word], &word, 4);
            s_const_version++;
        }
    }
    else if (vsh_dump_enabled()) {
        static int warned;
        if (warned++ < 8)
            fprintf(stderr, "  [VSH] constant write DROPPED (c[%u] >= %u)\n",
                    s_const_load, NV2A_VSH_CONSTANTS);
    }
    if (vsh_dump_enabled() && s_const_word == 3 && s_const_load >= 64) {
        static int shown;
        if (shown++ < 96)
            fprintf(stderr, "  [VSH] c[%u] = %.6g %.6g %.6g %.6g\n",
                    s_const_load, s_const[s_const_load][0],
                    s_const[s_const_load][1], s_const[s_const_load][2],
                    s_const[s_const_load][3]);
    }
    if (++s_const_word == 4) {
        s_const_word = 0;
        s_const_load++;
    }
}

void nv2a_vsh_set_constant(uint32_t index, const float v[4])
{
    if (index < NV2A_VSH_CONSTANTS && memcmp(s_const[index], v, sizeof s_const[index])) {
        memcpy(s_const[index], v, sizeof s_const[index]);
        s_const_version++;
    }
}

void nv2a_vsh_set_instruction(uint32_t slot, const uint32_t words[4])
{
    if (slot < NV2A_VSH_SLOTS && memcmp(s_program[slot], words, sizeof s_program[slot])) {
        memcpy(s_program[slot], words, sizeof s_program[slot]);
        s_prog_version++;
    }
}

/* ---- disassembly (RECOMP_VSH_DUMP) --------------------------------------- */

static const char *const k_mac[16] = {
    "nop", "mov", "mul", "add", "mad", "dp3", "dph", "dp4",
    "dst", "min", "max", "slt", "sge", "arl", "mac14", "mac15"
};
static const char *const k_ilu[8] = {
    "nop", "mov", "rcp", "rcc", "rsq", "exp", "log", "lit"
};

static void dis_src(char *b, size_t n, const uint32_t *ins, int which)
{
    static const char sw[] = "xyzw";
    uint32_t neg, swz, mux, reg;

    if (which == 0) {
        neg = field(ins, 1, 8, 1); swz = field(ins, 1, 0, 8);
        mux = field(ins, 2, 26, 2); reg = field(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = field(ins, 2, 25, 1); swz = field(ins, 2, 17, 8);
        mux = field(ins, 2, 11, 2); reg = field(ins, 2, 13, 4);
    } else {
        neg = field(ins, 2, 10, 1); swz = field(ins, 2, 2, 8);
        mux = field(ins, 3, 28, 2);
        reg = (field(ins, 2, 0, 2) << 2) | field(ins, 3, 30, 2);
    }
    if (mux == 1)
        snprintf(b, n, "%sR%u", neg ? "-" : "", reg);
    else if (mux == 2)
        snprintf(b, n, "%sv%u", neg ? "-" : "", field(ins, 1, 9, 4));
    else
        snprintf(b, n, "%sc[%s%u]", neg ? "-" : "",
                 field(ins, 3, 1, 1) ? "a0.x+" : "", field(ins, 1, 13, 8));
    {
        size_t l = strlen(b);
        if (l + 6 < n)
            snprintf(b + l, n - l, ".%c%c%c%c", sw[(swz >> 6) & 3],
                     sw[(swz >> 4) & 3], sw[(swz >> 2) & 3], sw[swz & 3]);
    }
}

static void dump_program(uint32_t start)
{
    uint32_t s;
    fprintf(stderr, "  [VSH] program at slot %u:\n", start);
    for (s = start; s < NV2A_VSH_SLOTS; s++) {
        const uint32_t *ins = s_program[s];
        char a[40], bb[40], c[40];
        dis_src(a, sizeof a, ins, 0);
        dis_src(bb, sizeof bb, ins, 1);
        dis_src(c, sizeof c, ins, 2);
        fprintf(stderr, "  [VSH] %3u %-4s %-4s A=%s B=%s C=%s  R%u mac%X ilu%X"
                " out%s%u m%X%s%s\n", s,
                k_mac[field(ins, 1, 21, 4)], k_ilu[field(ins, 1, 25, 3)],
                a, bb, c, field(ins, 3, 20, 4), field(ins, 3, 24, 4),
                field(ins, 3, 16, 4), field(ins, 3, 11, 1) ? "o" : "c",
                field(ins, 3, 3, 8), field(ins, 3, 12, 4),
                field(ins, 3, 2, 1) ? " ilu" : " mac",
                field(ins, 3, 0, 1) ? " FINAL" : "");
        if (field(ins, 3, 0, 1))
            break;
    }
    fflush(stderr);
}

const float *nv2a_vsh_constant(uint32_t index)
{
    return s_const[index < NV2A_VSH_CONSTANTS ? index : 0];
}

void nv2a_vsh_constant_component(uint32_t index, uint32_t comp, uint32_t word)
{
    if (index < NV2A_VSH_CONSTANTS && comp < 4
        && memcmp(&s_const[index][comp], &word, 4)) {
        memcpy(&s_const[index][comp], &word, 4);
        s_const_version++;
    }
}

/* ---- execution ----------------------------------------------------------- */

typedef struct { float v[4]; } vec4;

static vec4 read_src(const uint32_t *ins, int which,
                     const float in[NV2A_VSH_INPUTS][4],
                     const vec4 *temp, const vec4 *opos, int a0)
{
    uint32_t neg, swz, mux, reg;
    const float *s;
    vec4 r;

    if (which == 0) {
        neg = field(ins, 1, 8, 1); swz = field(ins, 1, 0, 8);
        mux = field(ins, 2, 26, 2); reg = field(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = field(ins, 2, 25, 1); swz = field(ins, 2, 17, 8);
        mux = field(ins, 2, 11, 2); reg = field(ins, 2, 13, 4);
    } else {
        neg = field(ins, 2, 10, 1); swz = field(ins, 2, 2, 8);
        mux = field(ins, 3, 28, 2);
        reg = (field(ins, 2, 0, 2) << 2) | field(ins, 3, 30, 2);
    }

    if (mux == 1)
        s = (reg == 12) ? opos->v : (reg < 12 ? temp[reg].v : temp[0].v);
    else if (mux == 2)
        s = in[field(ins, 1, 9, 4)];
    else {
        static const float zero[4] = { 0, 0, 0, 0 };
        int ci = (int)field(ins, 1, 13, 8);
        if (field(ins, 3, 1, 1))
            ci += a0;
        /* Out of range reads zero (D3D's rule for relative addressing).
         * Titles rely on it: MoH: European Assault's four-light program
         * names an unused light by index 200, so its colour reads c[204];
         * clamped to c[0], a row of the transform became a light colour and
         * lit every rail and cable saturated red. */
        s = (ci < 0 || ci >= NV2A_VSH_CONSTANTS) ? zero : s_const[ci];
    }
    r.v[0] = s[(swz >> 6) & 3];
    r.v[1] = s[(swz >> 4) & 3];
    r.v[2] = s[(swz >> 2) & 3];
    r.v[3] = s[swz & 3];
    if (neg) {
        r.v[0] = -r.v[0]; r.v[1] = -r.v[1];
        r.v[2] = -r.v[2]; r.v[3] = -r.v[3];
    }
    return r;
}

static void write_masked(float *dst, const vec4 *src, uint32_t mask)
{
    if (mask & 8) dst[0] = src->v[0];
    if (mask & 4) dst[1] = src->v[1];
    if (mask & 2) dst[2] = src->v[2];
    if (mask & 1) dst[3] = src->v[3];
}

static vec4 splat(float f)
{
    vec4 r;
    r.v[0] = r.v[1] = r.v[2] = r.v[3] = f;
    return r;
}

static vec4 run_mac(uint32_t op, vec4 a, vec4 b, vec4 c, int *a0)
{
    vec4 r = a;
    int i;

    switch (op) {
    case 1: break;                                            /* mov */
    case 2: for (i = 0; i < 4; i++) r.v[i] = a.v[i] * b.v[i]; break;
    case 3: for (i = 0; i < 4; i++) r.v[i] = a.v[i] + c.v[i]; break;
    case 4: for (i = 0; i < 4; i++) r.v[i] = a.v[i] * b.v[i] + c.v[i]; break;
    case 5: r = splat(a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2]); break;
    case 6: r = splat(a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2]
                      + b.v[3]); break;                        /* dph */
    case 7: r = splat(a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2]
                      + a.v[3]*b.v[3]); break;
    case 8: r.v[0] = 1.0f; r.v[1] = a.v[1] * b.v[1];            /* dst */
            r.v[2] = a.v[2]; r.v[3] = b.v[3]; break;
    case 9: for (i = 0; i < 4; i++) r.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i]; break;
    case 10: for (i = 0; i < 4; i++) r.v[i] = a.v[i] >= b.v[i] ? a.v[i] : b.v[i]; break;
    case 11: for (i = 0; i < 4; i++) r.v[i] = a.v[i] < b.v[i] ? 1.0f : 0.0f; break;
    case 12: for (i = 0; i < 4; i++) r.v[i] = a.v[i] >= b.v[i] ? 1.0f : 0.0f; break;
    case 13: *a0 = (int)floorf(a.v[0] + 0.001f); break;         /* arl */
    default: break;
    }
    return r;
}

static vec4 run_ilu(uint32_t op, vec4 c)
{
    float x = c.v[0];
    vec4 r = c;

    switch (op) {
    case 1: break;                                            /* mov */
    case 2: r = splat(x == 0.0f ? INFINITY : 1.0f / x); break;  /* rcp */
    case 3: {                                                  /* rcc */
        float f = 1.0f / x;
        if (fabsf(f) < 5.42101e-20f) f = f < 0 ? -5.42101e-20f : 5.42101e-20f;
        if (fabsf(f) > 1.884467e19f) f = f < 0 ? -1.884467e19f : 1.884467e19f;
        r = splat(f);
        break;
    }
    case 4: r = splat(1.0f / sqrtf(fabsf(x))); break;           /* rsq */
    case 5: {                                                  /* exp */
        float fl = floorf(x);
        r.v[0] = exp2f(fl); r.v[1] = x - fl; r.v[2] = exp2f(x); r.v[3] = 1.0f;
        break;
    }
    case 6: {                                                  /* log */
        float ax = fabsf(x);
        if (ax == 0.0f) {
            r.v[0] = r.v[2] = -INFINITY; r.v[1] = 1.0f;
        } else {
            float e = floorf(log2f(ax));
            r.v[0] = e; r.v[1] = ax / exp2f(e); r.v[2] = log2f(ax);
        }
        r.v[3] = 1.0f;
        break;
    }
    case 7: {                                                  /* lit */
        float nl = c.v[0] > 0.0f ? c.v[0] : 0.0f;
        float nh = c.v[1] > 0.0f ? c.v[1] : 0.0f;
        float p = c.v[3] < -127.9961f ? -127.9961f
                : (c.v[3] > 127.9961f ? 127.9961f : c.v[3]);
        r.v[0] = 1.0f; r.v[1] = nl;
        r.v[2] = c.v[0] > 0.0f ? powf(nh, p) : 0.0f; r.v[3] = 1.0f;
        break;
    }
    default: break;
    }
    return r;
}

/* The program from s_start_slot, its fields extracted once.
 *
 * Every vertex used to pull every field out of the 128-bit words again --
 * a dozen shifts and masks per source, three sources, ~60 instructions, for
 * every vertex of every batch -- and with the pixels on the GPU that was the
 * executor's largest cost. The words only change on a program upload or a new
 * start slot, which bump s_prog_version. */
typedef struct {
    uint8_t mux, reg, neg, in, rel;
    uint8_t sw[4];
    int16_t cidx;
} VshSrc;
typedef struct {
    uint8_t mac, ilu, mac_mask, ilu_mask, tdst, omask, out_ilu, out_o, final;
    uint8_t oaddr;
    VshSrc src[3];
} VshIns;
static VshIns s_dec[NV2A_VSH_SLOTS];
static uint32_t s_dec_version, s_dec_n;

static void dec_src(VshSrc *d, const uint32_t *ins, int which)
{
    uint32_t swz;
    if (which == 0) {
        d->neg = (uint8_t)field(ins, 1, 8, 1); swz = field(ins, 1, 0, 8);
        d->mux = (uint8_t)field(ins, 2, 26, 2); d->reg = (uint8_t)field(ins, 2, 28, 4);
    } else if (which == 1) {
        d->neg = (uint8_t)field(ins, 2, 25, 1); swz = field(ins, 2, 17, 8);
        d->mux = (uint8_t)field(ins, 2, 11, 2); d->reg = (uint8_t)field(ins, 2, 13, 4);
    } else {
        d->neg = (uint8_t)field(ins, 2, 10, 1); swz = field(ins, 2, 2, 8);
        d->mux = (uint8_t)field(ins, 3, 28, 2);
        d->reg = (uint8_t)((field(ins, 2, 0, 2) << 2) | field(ins, 3, 30, 2));
    }
    d->sw[0] = (uint8_t)((swz >> 6) & 3);
    d->sw[1] = (uint8_t)((swz >> 4) & 3);
    d->sw[2] = (uint8_t)((swz >> 2) & 3);
    d->sw[3] = (uint8_t)(swz & 3);
    d->in = (uint8_t)field(ins, 1, 9, 4);
    d->cidx = (int16_t)field(ins, 1, 13, 8);
    d->rel = (uint8_t)field(ins, 3, 1, 1);
}

static void vsh_decode(void)
{
    uint32_t s, n = 0;
    for (s = s_start_slot; s < NV2A_VSH_SLOTS; s++) {
        const uint32_t *ins = s_program[s];
        VshIns *d = &s_dec[n++];
        d->mac = (uint8_t)field(ins, 1, 21, 4);
        d->ilu = (uint8_t)field(ins, 1, 25, 3);
        d->mac_mask = (uint8_t)field(ins, 3, 24, 4);
        d->ilu_mask = (uint8_t)field(ins, 3, 16, 4);
        d->tdst = (uint8_t)field(ins, 3, 20, 4);
        d->omask = (uint8_t)field(ins, 3, 12, 4);
        d->oaddr = (uint8_t)field(ins, 3, 3, 8);
        d->out_ilu = (uint8_t)field(ins, 3, 2, 1);
        d->out_o = (uint8_t)field(ins, 3, 11, 1);
        d->final = (uint8_t)field(ins, 3, 0, 1);
        dec_src(&d->src[0], ins, 0);
        dec_src(&d->src[1], ins, 1);
        dec_src(&d->src[2], ins, 2);
        if (d->final)
            break;
    }
    s_dec_n = n;
    s_dec_version = s_prog_version;
}

static inline vec4 read_dec(const VshSrc *d, const float in[NV2A_VSH_INPUTS][4],
                            const vec4 *temp, const vec4 *opos, int a0)
{
    static const float zero[4] = { 0, 0, 0, 0 };
    const float *p;
    vec4 r;
    if (d->mux == 1)
        p = (d->reg == 12) ? opos->v : (d->reg < 12 ? temp[d->reg].v : temp[0].v);
    else if (d->mux == 2)
        p = in[d->in];
    else {
        int ci = d->cidx + (d->rel ? a0 : 0);
        /* Out of range reads zero (see read_src). */
        p = (ci < 0 || ci >= NV2A_VSH_CONSTANTS) ? zero : s_const[ci];
    }
    r.v[0] = p[d->sw[0]];
    r.v[1] = p[d->sw[1]];
    r.v[2] = p[d->sw[2]];
    r.v[3] = p[d->sw[3]];
    if (d->neg) {
        r.v[0] = -r.v[0]; r.v[1] = -r.v[1];
        r.v[2] = -r.v[2]; r.v[3] = -r.v[3];
    }
    return r;
}

void nv2a_vsh_prepare(void)
{
    if (s_dec_version != s_prog_version)
        vsh_decode();
}

/* Set by the executor to trace one vertex instruction by instruction. */
int nv2a_vsh_step_trace;

int nv2a_vsh_run(const float in[NV2A_VSH_INPUTS][4], Nv2aVshOutput *out)
{
    static int dump = -1;
    static uint32_t dumped[16];
    static int ndumped;
    vec4 temp[12], opos;
    float outregs[13][4];
    int a0 = 0;
    uint32_t s;

    if (dump < 0)
        dump = getenv("RECOMP_VSH_DUMP") != NULL;
    if (dump && ndumped < 16) {
        /* By content: titles reload different programs into the same slot. */
        uint32_t h = 2166136261u, t;
        int i, seen = 0;
        for (t = s_start_slot; t < NV2A_VSH_SLOTS; t++) {
            for (i = 0; i < 4; i++)
                h = (h ^ s_program[t][i]) * 16777619u;
            if (field(s_program[t], 3, 0, 1))
                break;
        }
        for (i = 0; i < ndumped; i++)
            if (dumped[i] == h) seen = 1;
        if (!seen) {
            dumped[ndumped++] = h;
            dump_program(s_start_slot);
        }
    }

    memset(temp, 0, sizeof temp);
    memset(&opos, 0, sizeof opos);
    memset(outregs, 0, sizeof outregs);
    outregs[3][3] = outregs[4][3] = 1.0f;      /* colours default opaque */
    /* Unwritten texture coordinates are (0,0,0,1), as xemu initialises them:
     * a projective stage divides by q, and q = 0 would put every texel of an
     * unwritten stage at infinity. */
    outregs[9][3] = outregs[10][3] = outregs[11][3] = outregs[12][3] = 1.0f;

    if (s_dec_version != s_prog_version)
        vsh_decode();
    for (s = s_start_slot; s < s_start_slot + s_dec_n; s++) {
        const VshIns *di = &s_dec[s - s_start_slot];
        uint32_t mac = di->mac, ilu = di->ilu;
        uint32_t mac_mask = di->mac_mask, ilu_mask = di->ilu_mask;
        uint32_t tdst = di->tdst;
        uint32_t omask = di->omask, oaddr = di->oaddr;
        vec4 a = read_dec(&di->src[0], in, temp, &opos, a0);
        vec4 b = read_dec(&di->src[1], in, temp, &opos, a0);
        vec4 c = read_dec(&di->src[2], in, temp, &opos, a0);
        vec4 mres = {{0, 0, 0, 0}}, ires = {{0, 0, 0, 0}};

        if (mac)
            mres = run_mac(mac, a, b, c, &a0);
        if (ilu)
            ires = run_ilu(ilu, c);
        if (nv2a_vsh_step_trace) {
            if (s == s_start_slot)
                dump_program(s_start_slot);
            fprintf(stderr, "  [VSTEP] %3u A(%g %g %g %g) B(%g %g %g %g) C(%g %g %g %g)"
                    " mac(%g %g %g %g) ilu(%g %g %g %g) a0 %d\n", s,
                    a.v[0], a.v[1], a.v[2], a.v[3], b.v[0], b.v[1], b.v[2], b.v[3],
                    c.v[0], c.v[1], c.v[2], c.v[3], mres.v[0], mres.v[1], mres.v[2],
                    mres.v[3], ires.v[0], ires.v[1], ires.v[2], ires.v[3], a0);
        }

        if (mac && mac != 13 && mac_mask) {
            float *d = tdst == 12 ? opos.v : (tdst < 12 ? temp[tdst].v : NULL);
            if (d) write_masked(d, &mres, mac_mask);
        }
        if (ilu && ilu_mask) {
            /* Paired with any MAC op, the ILU can only write R1 -- even when
             * the MAC half writes no temp at all. The D3D epilogue is exactly
             * that: `mul o[0].xyz` beside `rcc R1.x`, then `mad` with R1.x.
             * Sending the rcc to the temp field (R7 there) left R1.x at 0 and
             * put every vertex of every 3D batch at c[59]. */
            uint32_t it = mac ? 1u : tdst;
            float *d = it == 12 ? opos.v : (it < 12 ? temp[it].v : NULL);
            if (d) write_masked(d, &ires, ilu_mask);
        }
        if (omask) {
            const vec4 *src = di->out_ilu ? &ires : &mres;
            if (!di->out_o) {                             /* to c[] */
                if (s_cxt_write && oaddr < NV2A_VSH_CONSTANTS) {
                    write_masked(s_const[oaddr], src, omask);
                    s_const_version++;
                }
            } else if (oaddr == 0) {
                write_masked(opos.v, src, omask);
            } else if (oaddr < 13) {
                write_masked(outregs[oaddr], src, omask);
            }
        }
        if (di->final) {
            memcpy(out->pos, opos.v, sizeof out->pos);
            /* The NV2A clamps the colour outputs to 0..1 before they are
             * interpolated, as the fixed-function path already does here.
             * Lighting programs push them well above 1 (SSX 3's riders), and
             * unclamped every lit model came out washed out. */
            for (int k = 0; k < 4; k++) {
                float c0 = outregs[3][k], c1 = outregs[4][k];
                out->d0[k] = c0 > 1.0f ? 1.0f : (c0 >= 0.0f ? c0 : 0.0f);
                out->d1[k] = c1 > 1.0f ? 1.0f : (c1 >= 0.0f ? c1 : 0.0f);
            }
            memcpy(out->fog, outregs[5], sizeof out->fog);
            memcpy(out->tex[0], outregs[9], sizeof out->tex[0]);
            memcpy(out->tex[1], outregs[10], sizeof out->tex[1]);
            memcpy(out->tex[2], outregs[11], sizeof out->tex[2]);
            memcpy(out->tex[3], outregs[12], sizeof out->tex[3]);
            return 1;
        }
    }
    return 0;
}

/* ---- four vertices at once ----------------------------------------------
 *
 * The same program, run on four vertices in the lanes of SSE registers:
 * every register holds x, y, z and w as four vectors, one lane per vertex, so
 * a swizzle is a choice of vector rather than a shuffle and every MAC op is
 * one SSE op per component. The decode, the dispatch and the masks are paid
 * once for four vertices instead of once for each. Skinned models (MoH's
 * soldiers) ran the scalar interpreter for most of the executor's time.
 *
 * Bit-exact with nv2a_vsh_run: the same operations in the same order, IEEE
 * SSE arithmetic either way (no FMA contraction under /fp:precise), MINPS
 * and MAXPS being what MSVC makes of the scalar selects (checked by a fuzz
 * against nv2a_vsh_run: a select for max differed on signed zeros), and the
 * rare ILU ops (rcc, exp, log, lit) done lane by lane through run_ilu
 * itself. Writes to c[] (CXT_WRITE) are
 * not supported; the caller keeps those programs on the scalar path. */
#include <emmintrin.h>

typedef struct { __m128 c[4]; } V4;

static uint32_t s_used_version, s_used;

/* The inputs the current program reads; decoded on first use. */
uint32_t nv2a_vsh_inputs_used(void)
{
    uint32_t i, k, m = 0;
    if (s_dec_version != s_prog_version)
        vsh_decode();
    if (s_used_version == s_dec_version)
        return s_used;
    for (i = 0; i < s_dec_n; i++)
        for (k = 0; k < 3; k++)
            if (s_dec[i].src[k].mux == 2)
                m |= 1u << s_dec[i].src[k].in;
    s_used = m;
    s_used_version = s_dec_version;
    return m;
}

static inline void read4(V4 *r, const VshSrc *d, const V4 *in, const V4 *temp,
                         const V4 *opos, const int a0[4])
{
    static const float zero[4] = { 0, 0, 0, 0 };
    const V4 *p;
    int k;

    if (d->mux == 1)
        p = d->reg == 12 ? opos : (d->reg < 12 ? &temp[d->reg] : &temp[0]);
    else if (d->mux == 2)
        p = &in[d->in];
    else {
        if (!d->rel) {
            int ci = d->cidx;
            const float *q = (ci < 0 || ci >= NV2A_VSH_CONSTANTS) ? zero : s_const[ci];
            for (k = 0; k < 4; k++)
                r->c[k] = _mm_set1_ps(q[d->sw[k]]);
        } else {
            const float *q[4];
            int l;
            for (l = 0; l < 4; l++) {
                int ci = d->cidx + a0[l];
                q[l] = (ci < 0 || ci >= NV2A_VSH_CONSTANTS) ? zero : s_const[ci];
            }
            for (k = 0; k < 4; k++) {
                int s = d->sw[k];
                r->c[k] = _mm_setr_ps(q[0][s], q[1][s], q[2][s], q[3][s]);
            }
        }
        p = r;
    }
    if (p != r)
        for (k = 0; k < 4; k++)
            r->c[k] = p->c[d->sw[k]];
    if (d->neg) {
        const __m128 sign = _mm_set1_ps(-0.0f);
        for (k = 0; k < 4; k++)
            r->c[k] = _mm_xor_ps(r->c[k], sign);
    }
}

static inline __m128 sel4(__m128 m, __m128 a, __m128 b)
{
    return _mm_or_ps(_mm_and_ps(m, a), _mm_andnot_ps(m, b));
}

static void mac4(V4 *r, uint32_t op, const V4 *a, const V4 *b, const V4 *c, int a0[4])
{
    const __m128 one = _mm_set1_ps(1.0f);
    __m128 d;
    int k;

    *r = *a;
    switch (op) {
    case 1: break;
    case 2: for (k = 0; k < 4; k++) r->c[k] = _mm_mul_ps(a->c[k], b->c[k]); break;
    case 3: for (k = 0; k < 4; k++) r->c[k] = _mm_add_ps(a->c[k], c->c[k]); break;
    case 4:
        for (k = 0; k < 4; k++)
            r->c[k] = _mm_add_ps(_mm_mul_ps(a->c[k], b->c[k]), c->c[k]);
        break;
    case 5: case 6: case 7:
        d = _mm_add_ps(_mm_add_ps(_mm_mul_ps(a->c[0], b->c[0]),
                                  _mm_mul_ps(a->c[1], b->c[1])),
                       _mm_mul_ps(a->c[2], b->c[2]));
        if (op == 6)
            d = _mm_add_ps(d, b->c[3]);
        else if (op == 7)
            d = _mm_add_ps(d, _mm_mul_ps(a->c[3], b->c[3]));
        r->c[0] = r->c[1] = r->c[2] = r->c[3] = d;
        break;
    case 8:
        r->c[0] = one;
        r->c[1] = _mm_mul_ps(a->c[1], b->c[1]);
        r->c[2] = a->c[2];
        r->c[3] = b->c[3];
        break;
    case 9: for (k = 0; k < 4; k++) r->c[k] = _mm_min_ps(a->c[k], b->c[k]); break;
    case 10:
        for (k = 0; k < 4; k++)
            r->c[k] = _mm_max_ps(a->c[k], b->c[k]);
        break;
    case 11:
        for (k = 0; k < 4; k++)
            r->c[k] = _mm_and_ps(_mm_cmplt_ps(a->c[k], b->c[k]), one);
        break;
    case 12:
        for (k = 0; k < 4; k++)
            r->c[k] = _mm_and_ps(_mm_cmpge_ps(a->c[k], b->c[k]), one);
        break;
    case 13: {
        float x[4];
        int l;
        _mm_storeu_ps(x, a->c[0]);
        for (l = 0; l < 4; l++)
            a0[l] = (int)floorf(x[l] + 0.001f);
        break;
    }
    default: break;
    }
}

static void ilu4(V4 *r, uint32_t op, const V4 *c)
{
    const __m128 one = _mm_set1_ps(1.0f);
    __m128 x = c->c[0], q;

    *r = *c;
    switch (op) {
    case 1: return;
    case 2:
        q = sel4(_mm_cmpeq_ps(x, _mm_setzero_ps()), _mm_set1_ps(INFINITY),
                 _mm_div_ps(one, x));
        break;
    case 4:
        q = _mm_div_ps(one, _mm_sqrt_ps(_mm_andnot_ps(_mm_set1_ps(-0.0f), x)));
        break;
    default: {
        /* rcc, exp, log, lit: lane by lane through the scalar unit. */
        float in[4][4], out[4][4];
        int k, l;
        for (k = 0; k < 4; k++)
            _mm_storeu_ps(in[k], c->c[k]);
        for (l = 0; l < 4; l++) {
            vec4 v, w;
            for (k = 0; k < 4; k++)
                v.v[k] = in[k][l];
            w = run_ilu(op, v);
            for (k = 0; k < 4; k++)
                out[k][l] = w.v[k];
        }
        for (k = 0; k < 4; k++)
            r->c[k] = _mm_loadu_ps(out[k]);
        return;
    }
    }
    r->c[0] = r->c[1] = r->c[2] = r->c[3] = q;
}

static inline void wmask4(V4 *d, const V4 *s, uint32_t m)
{
    if (m & 8) d->c[0] = s->c[0];
    if (m & 4) d->c[1] = s->c[1];
    if (m & 2) d->c[2] = s->c[2];
    if (m & 1) d->c[3] = s->c[3];
}

int nv2a_vsh_run4(const float (*const in[4])[4], uint32_t lanes,
                  Nv2aVshOutput *out)
{
    V4 vin[NV2A_VSH_INPUTS], temp[12], opos, outr[13], vzero;
    int a0[4] = { 0, 0, 0, 0 };
    uint32_t i, a, used;
    const __m128 one = _mm_set1_ps(1.0f);

    used = nv2a_vsh_inputs_used();
    for (a = 0; a < NV2A_VSH_INPUTS; a++) {
        if (used & (1u << a)) {
            __m128 r0 = _mm_loadu_ps(in[0][a]);
            __m128 r1 = _mm_loadu_ps(in[lanes > 1 ? 1 : 0][a]);
            __m128 r2 = _mm_loadu_ps(in[lanes > 2 ? 2 : 0][a]);
            __m128 r3 = _mm_loadu_ps(in[lanes > 3 ? 3 : 0][a]);
            _MM_TRANSPOSE4_PS(r0, r1, r2, r3);
            vin[a].c[0] = r0; vin[a].c[1] = r1; vin[a].c[2] = r2; vin[a].c[3] = r3;
        }
    }
    vzero.c[0] = vzero.c[1] = vzero.c[2] = vzero.c[3] = _mm_setzero_ps();
    for (i = 0; i < 12; i++)
        temp[i] = vzero;
    opos = vzero;
    for (i = 0; i < 13; i++)
        outr[i] = vzero;
    outr[3].c[3] = outr[4].c[3] = one;
    outr[9].c[3] = outr[10].c[3] = outr[11].c[3] = outr[12].c[3] = one;

    for (i = 0; i < s_dec_n; i++) {
        const VshIns *di = &s_dec[i];
        V4 va, vb, vc, mres = vzero, ires = vzero;

        read4(&va, &di->src[0], vin, temp, &opos, a0);
        read4(&vb, &di->src[1], vin, temp, &opos, a0);
        read4(&vc, &di->src[2], vin, temp, &opos, a0);
        if (di->mac)
            mac4(&mres, di->mac, &va, &vb, &vc, a0);
        if (di->ilu)
            ilu4(&ires, di->ilu, &vc);
        if (di->mac && di->mac != 13 && di->mac_mask) {
            V4 *d = di->tdst == 12 ? &opos : (di->tdst < 12 ? &temp[di->tdst] : NULL);
            if (d) wmask4(d, &mres, di->mac_mask);
        }
        if (di->ilu && di->ilu_mask) {
            uint32_t it = di->mac ? 1u : di->tdst;
            V4 *d = it == 12 ? &opos : (it < 12 ? &temp[it] : NULL);
            if (d) wmask4(d, &ires, di->ilu_mask);
        }
        if (di->omask && di->out_o) {
            const V4 *src = di->out_ilu ? &ires : &mres;
            if (di->oaddr == 0)
                wmask4(&opos, src, di->omask);
            else if (di->oaddr < 13)
                wmask4(&outr[di->oaddr], src, di->omask);
        }
        if (di->final) {
            float P[4][4], D0[4][4], D1[4][4], F[4][4], T[4][4][4];
            uint32_t l;
            int k, t;
            for (k = 0; k < 4; k++) {
                _mm_storeu_ps(P[k], opos.c[k]);
                _mm_storeu_ps(D0[k], outr[3].c[k]);
                _mm_storeu_ps(D1[k], outr[4].c[k]);
                _mm_storeu_ps(F[k], outr[5].c[k]);
                for (t = 0; t < 4; t++)
                    _mm_storeu_ps(T[t][k], outr[9 + t].c[k]);
            }
            for (l = 0; l < lanes; l++) {
                Nv2aVshOutput *o = &out[l];
                for (k = 0; k < 4; k++) {
                    float c0 = D0[k][l], c1 = D1[k][l];
                    o->pos[k] = P[k][l];
                    o->d0[k] = c0 > 1.0f ? 1.0f : (c0 >= 0.0f ? c0 : 0.0f);
                    o->d1[k] = c1 > 1.0f ? 1.0f : (c1 >= 0.0f ? c1 : 0.0f);
                    o->fog[k] = F[k][l];
                    for (t = 0; t < 4; t++)
                        o->tex[t][k] = T[t][k][l];
                }
            }
            return 1;
        }
    }
    return 0;
}

/* For a GPU that runs the program itself (nv2a_gpu_draw_program): the
 * instructions from the start slot through the final one, and the constant
 * file, each with a version that changes whenever its contents do. */
const uint32_t (*nv2a_vsh_program(uint32_t *n, uint32_t *version))[4]
{
    if (s_dec_version != s_prog_version)
        vsh_decode();
    *n = s_dec_n;
    *version = s_prog_version;
    return (const uint32_t (*)[4])s_program[s_start_slot];
}

void nv2a_vsh_dump_current(void)
{
    dump_program(s_start_slot);
}

const float (*nv2a_vsh_constants(uint32_t *version))[4]
{
    *version = s_const_version;
    return (const float (*)[4])s_const;
}
