/* The software rasteriser drawing a batch of triangles on the worker pool.
 *
 * A batch's triangles are queued and rasterised together, worker k taking
 * rows y % n == k of every triangle in submission order. Two ways that can go
 * wrong, both silent in a game frame:
 *
 *   - a row nobody owns (a wrong first row for some worker): a stripe of
 *     pixels never drawn;
 *   - triangles out of order within a row: where they overlap, an earlier
 *     triangle shows through a later one.
 *
 * So: many overlapping, pixel-aligned solid quads in one batch, with four
 * raster threads. Pixel centres sit at .5, never on a quad edge, so which quad
 * covers a pixel is exact, and the expected colour of every pixel is that of
 * the last quad drawn over it.
 *
 * Harness as tests/nv2a_ffp_backend, but no back end is registered, so the
 * software path draws. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"

#define W 256
#define H 256
#define COLOR_AT 0x10000u
#define POS_AT   0x60000u
#define DIFF_AT  0x70000u
#define MEM_BYTES 0x80000u
static uint8_t s_mem[MEM_BYTES];

/* ---- what the executor expects the runtime to provide ------------------ */
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
uint32_t xbox_Nv2aFenceWordVa(void) { return 0; }
int nv2a_pb_dma_walk(void) { return 1; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_Nv2aFrameCounterFlip(void) {}
/* The flip hold (#192, nv2a_flip_hold.c, linked in): no vblank moves here,
 * so the executor never holds a flip. */
int nv2a_pb_scan_in_call(void) { return 0; }
uint32_t xbox_VblankCount(void) { return 0; }
uint64_t xbox_HostNowNs(void) { return 0; }
uint32_t xbox_Nv2aFrameCountersOwned(void) { return 0; }
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void mf(uint32_t method, float f) { m(method, f2u(f)); }

#define QUADS 96
static int qx0[QUADS], qy0[QUADS], qx1[QUADS], qy1[QUADS];
static uint32_t qc[QUADS];

static uint32_t lcg(void)
{
    static uint32_t s = 12345u;
    s = s * 1103515245u + 12345u;
    return s >> 8;
}

/* Channel values 32 apart, so a rounding difference cannot make one quad's
 * colour read as another's. */
static uint32_t colour(int i)
{
    uint32_t r = 32u * (uint32_t)(i % 8) + 16u;
    uint32_t g = 32u * (uint32_t)((i / 8) % 8) + 16u;
    uint32_t b = 32u * (uint32_t)((i / 64) % 8) + 16u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static int close_rgb(uint32_t a, uint32_t b)
{
    int k;
    for (k = 0; k < 24; k += 8) {
        int d = (int)((a >> k) & 0xFF) - (int)((b >> k) & 0xFF);
        if (d < -4 || d > 4)
            return 0;
    }
    return 1;
}

static void put_vertex(uint32_t n, float x, float y, uint32_t c)
{
    float p[3] = { x, y, 0.0f };
    memcpy(s_mem + POS_AT + 12u * n, p, 12);
    memcpy(s_mem + DIFF_AT + 4u * n, &c, 4);
}

int main(void)
{
    static const float I[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    uint32_t n = 0, start;
    int i, x, y, bad = 0, drawn = 0;

#ifdef _WIN32
    _putenv("RECOMP_RASTER_THREADS=4");
#else
    setenv("RECOMP_RASTER_THREADS", "4", 1);
#endif

    m(0x0208, 0x0008);                       /* SET_SURFACE_FORMAT: A8R8G8B8, no AA */
    m(0x0200, (uint32_t)W << 16);            /* CLIP_HORIZONTAL */
    m(0x0204, (uint32_t)H << 16);            /* CLIP_VERTICAL */
    m(0x020C, W * 4);                        /* SET_SURFACE_PITCH */
    m(0x0210, COLOR_AT);                     /* SET_SURFACE_COLOR_OFFSET */
    for (i = 0; i < 16; i++)
        mf(0x0680 + 4 * i, I[i]);            /* composite: positions are pixels */
    m(0x1E94, 0);                            /* TRANSFORM_EXECUTION_MODE: fixed */

    /* Quads with integer corners, overlapping heavily, two triangles each,
     * both wound the same way. */
    for (i = 0; i < QUADS; i++) {
        int w = 16 + (int)(lcg() % 80), h = 16 + (int)(lcg() % 80);
        qx0[i] = (int)(lcg() % (W - w)); qy0[i] = (int)(lcg() % (H - h));
        qx1[i] = qx0[i] + w; qy1[i] = qy0[i] + h;
        qc[i] = colour(i);
        put_vertex(n++, (float)qx0[i], (float)qy0[i], qc[i]);
        put_vertex(n++, (float)qx1[i], (float)qy0[i], qc[i]);
        put_vertex(n++, (float)qx1[i], (float)qy1[i], qc[i]);
        put_vertex(n++, (float)qx0[i], (float)qy0[i], qc[i]);
        put_vertex(n++, (float)qx1[i], (float)qy1[i], qc[i]);
        put_vertex(n++, (float)qx0[i], (float)qy1[i], qc[i]);
    }
    m(0x1720, POS_AT);  m(0x1760, 2 | (3 << 4) | (12 << 8));       /* attr 0: float3 */
    m(0x172C, DIFF_AT); m(0x176C, 0 | (4 << 4) | (4 << 8));        /* attr 3: D3DCOLOR */

    /* One batch: BEGIN, several DRAW_ARRAYS of at most 256 vertices, END. */
    m(0x17FC, 5);                            /* BEGIN_END: triangles */
    for (start = 0; start < n; start += 240)
        m(0x1810, ((n - start > 240 ? 240 : n - start) - 1) << 24 | start);
    m(0x17FC, 0);

    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            uint32_t want = 0, got;
            memcpy(&got, s_mem + COLOR_AT + 4u * (uint32_t)(y * W + x), 4);
            for (i = 0; i < QUADS; i++)
                if (x >= qx0[i] && x < qx1[i] && y >= qy0[i] && y < qy1[i])
                    want = qc[i];
            if (want)
                drawn++;
            if (want ? !close_rgb(got, want) : (got & 0xFFFFFFu) != 0) {
                if (bad++ < 8)
                    printf("  pixel %d,%d: got %08X want %08X\n", x, y, got, want);
            }
        }
    }
    printf("%d of %d covered pixels checked, %d wrong\n", drawn, W * H, bad);
    if (bad || drawn < 8192) {
        printf("FAIL\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}
