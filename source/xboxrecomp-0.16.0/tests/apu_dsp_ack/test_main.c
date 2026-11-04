/* RECOMP_APU_DSP_ACK=auto clears the GP doorbell at SGE[0] + 0x810, where
 * SGE[0] is the first entry of the scatter-gather table GPSADDR points at,
 * and leaves it alone while GPRST holds the GP in reset.
 *
 * A small zeroed buffer stands in for RAM: with no heap, no image and no
 * contiguous allocation above page 0, mcpx_apu_phys resolves a low physical
 * address to g_apu_ram_ptr + address. */
#include "apu_state.h"
#include "apu_shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The runtime links against the title's dispatch; nothing here calls it. */
void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

#define FAKE_RAM   (256u << 10)
#define SGE_TABLE  0x00010000u   /* what GPSADDR points at */
#define GP_PAGE    0x00020000u   /* the scratch page SGE[0] names */
#define DOORBELL   (GP_PAGE + 0x810u)

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static uint32_t *dword(uint8_t *ram, uint32_t phys)
{
    return (uint32_t *)(ram + phys);
}

int main(void)
{
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(*d));
    uint8_t *ram = (uint8_t *)calloc(1, FAKE_RAM);
    const uint32_t run = NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST;

    if (!d || !ram) { fprintf(stderr, "out of memory\n"); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);
    g_apu_ram_ptr = ram;
    d->ram_ptr = ram;

    /* Before GPSADDR is set there is no doorbell to clear. */
    *dword(ram, DOORBELL) = 0x11;
    mcpx_apu_dsp_ack_poll(d);
    check(*dword(ram, DOORBELL) == 0x11, "GPSADDR unset: doorbell left");

    d->regs[NV_PAPU_GPSADDR] = SGE_TABLE;
    mcpx_apu_dsp_ack_poll(d);
    check(*dword(ram, DOORBELL) == 0x11, "SGE[0] empty: doorbell left");

    /* Never written GPRST: the GP counts as running. */
    *dword(ram, SGE_TABLE) = GP_PAGE;
    mcpx_apu_dsp_ack_poll(d);
    check(*dword(ram, DOORBELL) == 0, "SGE[0] filled: poll clears the doorbell");

    mcpx_apu_dsp_gprst_write(0);
    *dword(ram, DOORBELL) = 0x22;
    mcpx_apu_dsp_ack_poll(d);
    check(*dword(ram, DOORBELL) == 0x22, "GPRST 0 (held): poll leaves it");

    mcpx_apu_dsp_gprst_write(NV_PAPU_GPRST_GPRST);
    mcpx_apu_dsp_ack_poll(d);
    check(*dword(ram, DOORBELL) == 0x22, "GPRST 1 (DSP still held): poll leaves it");

    mcpx_apu_dsp_gprst_write(run);
    mcpx_apu_dsp_ack_poll(d);
    check(*dword(ram, DOORBELL) == 0, "GPRST 3 (running): poll clears it again");

    /* The table now names another page: the next poll follows it and leaves
     * the old page alone (it may belong to someone else by now). */
    *dword(ram, SGE_TABLE) = GP_PAGE + 0x1000u;
    *dword(ram, DOORBELL) = 0x33;
    *dword(ram, DOORBELL + 0x1000u) = 0x44;
    mcpx_apu_dsp_ack_poll(d);
    check(*dword(ram, DOORBELL + 0x1000u) == 0 && *dword(ram, DOORBELL) == 0x33,
          "SGE[0] moved: the new page is cleared, the old one left");

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    free(ram);
    free(d);
    return failures ? 1 : 0;
}
