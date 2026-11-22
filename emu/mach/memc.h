/*
 * memc.h - MEMC memory-controller model (WP3, Archimedes base machine).
 *
 * Model v1 (documented simplifications, refined at WP4/WP5 against the
 * MEMC1A datasheet): the logical window 0x00000000-0x01FFFFFF is ROM at
 * reset (so the CPU fetches its reset vectors from ROM, as on real
 * hardware) and switches to identity-mapped DRAM when the OS writes the
 * control register. The physical window 0x02000000-0x02FFFFFF aliases
 * DRAM directly. The CAM (logical->physical page map) is not walked yet;
 * identity mapping covers the first ram_size bytes.
 *
 * Register region: 0x03600000-0x037FFFFF.
 *   +0x00 control (write: leaves ROM reset map; value stored)
 *   +0x04 video DMA start address (logical) - our model register
 *
 * Licence: Apache-2.0 (see LICENSE). Register layout per Acorn MEMC1A
 * documentation; behaviour reference only from studied emulators.
 */
#ifndef SOPHIE_MEMC_H
#define SOPHIE_MEMC_H

#include <stdint.h>

#define MEMC_BASE      0x03600000u
#define MEMC_SIZE      0x00200000u
#define MEMC_CTRL_OFF  0x00u
#define MEMC_VIDSTART_OFF 0x04u

typedef struct Machine Machine;

typedef struct Memc {
    uint32_t regs[16];   /* word register file at MEMC_BASE */
    int      rom_mapped; /* reset state: logical low window maps to ROM */
    Machine *mach;
} Memc;

void memc_init(Memc *memc, Machine *mach);

/* region callbacks for [MEMC_BASE, MEMC_BASE+MEMC_SIZE) */
uint8_t  memc_rd8(void *ctx, uint32_t addr);
void     memc_wr8(void *ctx, uint32_t addr, uint8_t v);
uint32_t memc_rd32(void *ctx, uint32_t addr);
void     memc_wr32(void *ctx, uint32_t addr, uint32_t v);

#endif /* SOPHIE_MEMC_H */
