/*
 * memc.c - MEMC1A model with CAM walk (WP4). See memc.h for the contract.
 * Licence: Apache-2.0 (see LICENSE).
 */
#include "memc.h"
#include "machine.h"

#include <string.h>

#define BIT(x, b, n) (((x) >> (b)) & ((1u << (n)) - 1u))
#define BIT1(x, b) (((x) >> (b)) & 1u)

void memc_init(Memc *memc, Machine *mach)
{
    memset(memc, 0, sizeof *memc);
    memc->mach = mach;
    memc->latchrom = 1;                 /* reset: boot ROM latched into logical space */
    memc->pagesize_idx = 0;             /* 4K pages */
    for (uint32_t i = 0; i < MEMC_PAGES; i++)
        memc->pages[i] = -1;            /* unmapped */
}

void memc_page_w(Memc *memc, uint32_t data)
{
    uint32_t logpage, phyaddr, memc_id;
    switch (memc->pagesize_idx) {
      case 0: /* 4K */
        phyaddr = BIT(data, 0, 7);
        logpage = BIT(data, 12, 11) | (BIT(data, 10, 2) << 11);
        memc_id = BIT1(data, 7);
        break;
      case 1: /* 8K */
        phyaddr = BIT(data, 1, 6) | (BIT1(data, 0) << 6);
        logpage = BIT(data, 13, 10) | (BIT(data, 10, 2) << 10);
        memc_id = BIT1(data, 7) | (BIT1(data, 12) << 1);
        break;
      case 2: /* 16K */
        phyaddr = BIT(data, 2, 5) | (BIT(data, 0, 2) << 5);
        logpage = BIT(data, 14, 9) | (BIT(data, 10, 2) << 9);
        memc_id = BIT1(data, 7) | (BIT1(data, 12) << 1);
        break;
      default: /* 32K */
        phyaddr = BIT(data, 3, 4) | (BIT1(data, 0) << 4) | (BIT1(data, 1) << 6) | (BIT1(data, 2) << 5);
        logpage = BIT(data, 15, 8) | (BIT(data, 10, 2) << 8);
        memc_id = BIT1(data, 7) | (BIT1(data, 12) << 1);
        break;
    }
    phyaddr += memc_id * 0x80;
    if (logpage >= (uint32_t)MEMC_PAGES)
        return;

    /* unmap other logical pages resolving to the same physical page */
    for (uint32_t i = 0; i < MEMC_PAGES; i++)
        if (memc->pages[i] == (int16_t)phyaddr)
            memc->pages[i] = -1;

    memc->pages[logpage] = (int16_t)phyaddr;
    memc->ppl[logpage] = (uint8_t)BIT(data, 8, 2);
    memc->latchrom = 0; /* any CAM write disconnects the boot ROM latch */
}

void memc_map_page(Memc *memc, uint32_t logpage, uint32_t phypage)
{
    /* build the data-encoded CAM word for logpage -> phypage */
    uint32_t data = 0;
    switch (memc->pagesize_idx) {
      case 0:
        data = ((logpage & 0x7FF) << 12) | (((logpage >> 11) & 3) << 10)
             | (phypage & 0x7F) | (((phypage >> 7) & 1) << 7);
        break;
      case 1:
        data = ((logpage & 0x3FF) << 13) | (((logpage >> 10) & 3) << 10)
             | ((phypage & 0x3F) << 1) | (((phypage >> 6) & 1));
        break;
      case 2:
        data = ((logpage & 0x1FF) << 14) | (((logpage >> 9) & 3) << 10)
             | ((phypage & 0x1F) << 2) | (((phypage >> 5) & 3));
        break;
      default:
        data = ((logpage & 0xFF) << 15) | (((logpage >> 8) & 3) << 10)
             | ((phypage & 0xF) << 3) | (((phypage >> 4) & 1) << 0)
             | (((phypage >> 5) & 1) << 2) | (((phypage >> 6) & 1) << 1);
        break;
    }
    memc_page_w(memc, data);
}

void memc_reg_w(Memc *memc, uint32_t v)
{
    if ((v & 0x03E00000u) != 0x03600000u)
        return; /* not a MEMC register write */
    switch ((v >> 17) & 7) {
      case 0: memc->vidinit  = ((v >> 2) & 0x7FFF) * 16; break;
      case 1: memc->vidstart = ((v >> 2) & 0x7FFF) * 16; break;
      case 2: memc->vidend   = ((v >> 2) & 0x7FFF) * 16; break;
      case 3: memc->cinit    = ((v >> 2) & 0x7FFF) * 16; break;
      case 4: memc->sndstart = ((v >> 2) & 0x7FFF) * 16; break;
      case 5: memc->sndend   = ((v >> 2) & 0x7FFF) * 16; break;
      case 6: break; /* sound pointer reset */
      case 7: /* control */
        memc->ctrl = v & 0xFFFF;
        memc->pagesize_idx  = (v >> 2) & 3;
        memc->video_dma_on  = (v >> 10) & 1;
        memc->sound_dma_on  = (v >> 11) & 1;
        memc->os_mode       = (v >> 12) & 1;
        break;
    }
}

/* ---------- logical window (CAM walk) ---------- */

static uint32_t memc_translate(Memc *m, uint32_t addr, int *mapped)
{
    if (m->latchrom) {
        *mapped = 1;
        return 0; /* ROM latch: reads come from the ROM window, writes ignored */
    }
    uint32_t pagesize = MEMC_PAGE_SIZE(m->pagesize_idx);
    uint32_t page = addr / pagesize; /* addr < 32MB */
    if (page >= (uint32_t)MEMC_PAGES || m->pages[page] < 0) {
        *mapped = 0;
        return 0;
    }
    *mapped = 1;
    return MEMC_PHYS_BASE | ((uint32_t)m->pages[page] * pagesize + (addr % pagesize));
}

uint8_t memc_logical_rd8(void *ctx, uint32_t addr)
{
    Memc *m = ctx;
    Machine *mach = m->mach;
    int mapped;
    uint32_t phys = memc_translate(m, addr, &mapped);
    if (m->latchrom)
        return (addr & 0x1FFFFF) < mach->rom_size ? mach->rom[addr & 0x1FFFFF] : 0xFF;
    if (!mapped)
        return 0xFF; /* unmapped logical page: open bus */
    return mem_rd8(&mach->bus, phys);
}

void memc_logical_wr8(void *ctx, uint32_t addr, uint8_t v)
{
    Memc *m = ctx;
    if (m->latchrom)
        return; /* writes ignored while the boot ROM is latched */
    int mapped;
    uint32_t phys = memc_translate(m, addr, &mapped);
    if (!mapped)
        return;
    mem_wr8(&m->mach->bus, phys, v);
}

uint32_t memc_logical_rd32(void *ctx, uint32_t addr)
{
    return (uint32_t)memc_logical_rd8(ctx, addr)
         | ((uint32_t)memc_logical_rd8(ctx, addr + 1) << 8)
         | ((uint32_t)memc_logical_rd8(ctx, addr + 2) << 16)
         | ((uint32_t)memc_logical_rd8(ctx, addr + 3) << 24);
}

void memc_logical_wr32(void *ctx, uint32_t addr, uint32_t v)
{
    memc_logical_wr8(ctx, addr, (uint8_t)v);
    memc_logical_wr8(ctx, addr + 1, (uint8_t)(v >> 8));
    memc_logical_wr8(ctx, addr + 2, (uint8_t)(v >> 16));
    memc_logical_wr8(ctx, addr + 3, (uint8_t)(v >> 24));
}

/* ---------- register window ---------- */

uint8_t memc_rd8(void *ctx, uint32_t addr)
{
    (void)ctx; (void)addr;
    return 0xFF; /* MEMC register window is write-only in this model */
}

void memc_wr8(void *ctx, uint32_t addr, uint8_t v)
{
    (void)addr;
    /* ARM STRB replicates the byte on all four data lanes */
    memc_reg_w(ctx, (uint32_t)v * 0x01010101u);
}

uint32_t memc_rd32(void *ctx, uint32_t addr)
{
    (void)ctx; (void)addr;
    return 0xFFFFFFFFu;
}

void memc_wr32(void *ctx, uint32_t addr, uint32_t v)
{
    (void)addr;
    memc_reg_w(ctx, v);
}
