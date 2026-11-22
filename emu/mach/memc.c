/*
 * memc.c - MEMC memory-controller model (WP3). See memc.h.
 * Licence: Apache-2.0 (see LICENSE).
 */
#include "memc.h"
#include "machine.h"

#include <string.h>

void memc_init(Memc *memc, Machine *mach)
{
    memset(memc, 0, sizeof *memc);
    memc->mach = mach;
    memc->rom_mapped = 1; /* reset: ROM mapped at logical 0 */
}

uint8_t memc_rd8(void *ctx, uint32_t addr)
{
    Memc *m = ctx;
    uint32_t off = addr - MEMC_BASE;
    if (off < sizeof m->regs)
        return (uint8_t)(m->regs[off >> 2] >> ((off & 3) * 8));
    return 0xFF;
}

void memc_wr8(void *ctx, uint32_t addr, uint8_t v)
{
    Memc *m = ctx;
    uint32_t off = addr - MEMC_BASE;
    if (off >= sizeof m->regs)
        return;
    /* byte write into the register file */
    uint32_t shift = (off & 3) * 8;
    m->regs[off >> 2] = (m->regs[off >> 2] & ~(0xFFu << shift)) | ((uint32_t)v << shift);
    if (off == MEMC_CTRL_OFF)
        m->rom_mapped = 0; /* any control write leaves the ROM reset map */
}

uint32_t memc_rd32(void *ctx, uint32_t addr)
{
    Memc *m = ctx;
    uint32_t off = addr - MEMC_BASE;
    if (off < sizeof m->regs)
        return m->regs[off >> 2];
    return 0xFFFFFFFFu;
}

void memc_wr32(void *ctx, uint32_t addr, uint32_t v)
{
    Memc *m = ctx;
    uint32_t off = addr - MEMC_BASE;
    if (off >= sizeof m->regs)
        return;
    m->regs[off >> 2] = v;
    if (off == MEMC_CTRL_OFF)
        m->rom_mapped = 0;
}
