/*
 * machine.c - Archimedes base machine wiring (WP3). See machine.h.
 * Licence: Apache-2.0 (see LICENSE).
 */
#include "machine.h"

#include <string.h>

/* ---------- podule space stub ---------- */

static uint8_t podule_rd8(void *ctx, uint32_t addr)
{
    (void)ctx; (void)addr;
    return 0xFF;
}

static void podule_wr8(void *ctx, uint32_t addr, uint8_t v)
{
    (void)ctx; (void)addr; (void)v;
}

/* ---------- ROM window (read-only) ---------- */

static uint8_t mach_rom_rd8(void *ctx, uint32_t addr)
{
    Machine *m = ctx;
    m->memc.latchrom = 0; /* any high-space access drops the boot-ROM latch */
    uint32_t off = addr - MACH_ROM_BASE;
    return off < m->rom_size ? m->rom[off] : 0xFF;
}

static uint32_t mach_rom_rd32(void *ctx, uint32_t addr)
{
    return (uint32_t)mach_rom_rd8(ctx, addr)
         | ((uint32_t)mach_rom_rd8(ctx, addr + 1) << 8)
         | ((uint32_t)mach_rom_rd8(ctx, addr + 2) << 16)
         | ((uint32_t)mach_rom_rd8(ctx, addr + 3) << 24);
}

/* Writes into the ROM window program the MEMC CAM (page registers). */
static void mach_rom_wr8(void *ctx, uint32_t addr, uint8_t v)
{
    Machine *m = ctx;
    (void)addr;
    m->memc.latchrom = 0;
    memc_page_w(&m->memc, ((uint32_t)v << 24) | 0x00FFFFFFu);
}

static void mach_rom_wr32(void *ctx, uint32_t addr, uint32_t v)
{
    Machine *m = ctx;
    (void)addr;
    m->memc.latchrom = 0;
    memc_page_w(&m->memc, v);
}

/* ---------- init ---------- */

static void region_init(mem_region *r, uint32_t base, uint32_t size, void *ctx,
                        uint8_t (*rd8)(void *, uint32_t),
                        void (*wr8)(void *, uint32_t, uint8_t),
                        uint32_t (*rd32)(void *, uint32_t),
                        void (*wr32)(void *, uint32_t, uint32_t))
{
    memset(r, 0, sizeof *r);
    r->base = base;
    r->size = size;
    r->ctx = ctx;
    r->rd8 = rd8;
    r->wr8 = wr8;
    r->rd32 = rd32;
    r->wr32 = wr32;
}

void machine_init(Machine *m, uint8_t *ram, uint32_t ram_size,
                  uint8_t *rom, uint32_t rom_size)
{
    memset(m, 0, sizeof *m);
    m->ram = ram;
    m->ram_size = ram_size;
    m->ram_mask = ram_size - 1; /* power of two required */
    m->rom = rom;
    m->rom_size = rom_size;

    mem_bus_init(&m->bus, NULL, 0); /* no flat head region: logical window is a device */
    arm26_init(&m->cpu, &m->bus);
    memc_init(&m->memc, m);
    vidc_init(&m->vidc, m);
    ioc_init(&m->ioc, m);

    region_init(&m->r_logical, MACH_LOGICAL_BASE, MACH_LOGICAL_SIZE, &m->memc,
                memc_logical_rd8, memc_logical_wr8, memc_logical_rd32, memc_logical_wr32);
    mem_region_add(&m->bus, &m->r_logical);

    /* physical DRAM window: flat-array fast path over the same buffer */
    region_init(&m->r_phys, MACH_PHYS_BASE, MACH_PHYS_SIZE, m, NULL, NULL, NULL, NULL);
    m->r_phys.is_ram = 1;
    m->r_phys.ram = ram;
    m->r_phys.ram_mask = m->ram_mask;
    mem_region_add(&m->bus, &m->r_phys);

    region_init(&m->r_podule, MACH_PODULE_BASE, MACH_PODULE_SIZE, m,
                podule_rd8, podule_wr8, NULL, NULL);
    mem_region_add(&m->bus, &m->r_podule);

    region_init(&m->r_ioc, IOC_BASE, IOC_SIZE, &m->ioc,
                ioc_rd8, ioc_wr8, ioc_rd32, ioc_wr32);
    mem_region_add(&m->bus, &m->r_ioc);

    region_init(&m->r_vidc, VIDC_BASE, VIDC_SIZE, &m->vidc,
                vidc_rd8, vidc_wr8, vidc_rd32, vidc_wr32);
    mem_region_add(&m->bus, &m->r_vidc);

    region_init(&m->r_memc, MEMC_BASE, MEMC_SIZE, &m->memc,
                memc_rd8, memc_wr8, memc_rd32, memc_wr32);
    mem_region_add(&m->bus, &m->r_memc);

    region_init(&m->r_rom, MACH_ROM_BASE, MACH_ROM_SIZE, m,
                mach_rom_rd8, mach_rom_wr8, mach_rom_rd32, mach_rom_wr32);
    mem_region_add(&m->bus, &m->r_rom);
}

void machine_run(Machine *m, uint64_t n)
{
    while (n > 0 && !m->cpu.halted) {
        uint64_t before = m->cpu.cycles;
        uint64_t chunk = n > 64 ? 64 : n;
        arm26_run(&m->cpu, chunk);
        uint64_t did = m->cpu.cycles - before;
        if (did > 0)
            ioc_tick(&m->ioc, &m->cpu, (uint32_t)did);
        n -= did;
        if (did == 0)
            break; /* halted */
    }
}
