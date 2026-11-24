/*
 * memc.h - MEMC1A memory-controller model with CAM walk (WP4).
 *
 * Register/encoding facts per the Acorn MEMC1A datasheet (as documented in
 * public hardware references): the logical space is 32MB (0x00000000-
 * 0x01FFFFFF). At reset the boot ROM is latched into the logical space
 * (reads mirror ROM, writes are ignored) until the OS writes a CAM page
 * entry or touches the high window. The CAM holds one entry per logical
 * page; page size is 4K/8K/16K/32K selected by control-register bits 3:2.
 *
 * CAM page entries are written by storing a data-encoded word anywhere in
 * the ROM window 0x03800000-0x03FFFFFF (reads there still return ROM).
 * MEMC registers are written anywhere in 0x03600000-0x037FFFFF with the
 * register index encoded in the data (see memc_reg_w).
 *
 * Physical window: 0x02000000-0x02FFFFFF is DRAM, 0x03000000+ is I/O.
 * Protection levels (PPL) are stored but not enforced in this model.
 *
 * Licence: Apache-2.0 (see LICENSE). Written from hardware documentation;
 * studied emulators are reference-and-credit only.
 */
#ifndef SOPHIE_MEMC_H
#define SOPHIE_MEMC_H

#include <stdint.h>

#define MEMC_BASE          0x03600000u
#define MEMC_SIZE          0x00200000u
#define MEMC_ROMWIN_BASE   0x03800000u   /* CAM page writes land in the ROM window */
#define MEMC_LOGICAL_SIZE  0x02000000u   /* 32MB logical space */
#define MEMC_PAGES         0x2000u       /* 32MB / 4KB min page */
#define MEMC_PHYS_BASE     0x02000000u

#define MEMC_PAGE_SIZE(idx) (4096u << (idx))

typedef struct Machine Machine;

typedef struct Memc {
    int16_t  pages[MEMC_PAGES];  /* physical page per logical page; -1 = unmapped */
    uint8_t  ppl[MEMC_PAGES];    /* protection level per logical page (stored, not enforced) */
    int      pagesize_idx;       /* 0..3 -> 4K/8K/16K/32K */
    int      latchrom;           /* reset state: ROM latched into logical space */
    int      os_mode, video_dma_on, sound_dma_on;
    uint32_t ctrl;               /* raw control payload */
    uint32_t vidinit, vidstart, vidend, cinit;
    uint32_t sndstart, sndend;
    Machine *mach;
} Memc;

void memc_init(Memc *memc, Machine *mach);

/* CAM entry write: data-encoded per the MEMC1A page-register format. */
void memc_page_w(Memc *memc, uint32_t data);

/* Helper for tests/host code: map logical page -> physical page (identity
 * when logpage == phypage). */
void memc_map_page(Memc *memc, uint32_t logpage, uint32_t phypage);

/* MEMC register write (register index in data bits 19:17, 0x03600000 tag). */
void memc_reg_w(Memc *memc, uint32_t v);

/* Logical window (CAM walk); physical dispatch goes through the bus. */
uint8_t  memc_logical_rd8(void *ctx, uint32_t addr);
void     memc_logical_wr8(void *ctx, uint32_t addr, uint8_t v);
uint32_t memc_logical_rd32(void *ctx, uint32_t addr);
void     memc_logical_wr32(void *ctx, uint32_t addr, uint32_t v);

/* Region callbacks for the register window [MEMC_BASE, MEMC_BASE+MEMC_SIZE) */
uint8_t  memc_rd8(void *ctx, uint32_t addr);
void     memc_wr8(void *ctx, uint32_t addr, uint8_t v);
uint32_t memc_rd32(void *ctx, uint32_t addr);
void     memc_wr32(void *ctx, uint32_t addr, uint32_t v);

#endif /* SOPHIE_MEMC_H */
