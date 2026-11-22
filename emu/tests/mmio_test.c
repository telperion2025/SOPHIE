/*
 * mmio_test.c - WP2: MMIO dispatch layer tests.
 *
 * Proves that rd8/wr8/rd32/wr32 route through the device registry
 * (not the flat-RAM mask), that the CPU core reaches devices through
 * the bus, that unmapped accesses are safe, and that the WP1 core
 * regression suite path (RAM head region) still behaves.
 *
 * Run: ./mmio_test   (via `make test`)
 * Licence: Apache-2.0 (see LICENSE).
 */
#include <stdio.h>
#include <inttypes.h>
#include <string.h>

#include "test.h"

#define DEV_BASE 0x10000000u
#define DEV_SIZE 0x100u

typedef struct DummyDev {
    uint8_t  regs[DEV_SIZE];
    uint64_t rd_count;
    uint64_t wr_count;
} DummyDev;

static uint8_t dummy_rd8(void *ctx, uint32_t addr)
{
    DummyDev *d = ctx;
    d->rd_count++;
    return d->regs[(addr - DEV_BASE) & (DEV_SIZE - 1)];
}

static void dummy_wr8(void *ctx, uint32_t addr, uint8_t v)
{
    DummyDev *d = ctx;
    d->wr_count++;
    d->regs[(addr - DEV_BASE) & (DEV_SIZE - 1)] = v;
}

static uint32_t dummy_rd32(void *ctx, uint32_t addr)
{
    DummyDev *d = ctx;
    uint32_t off = (addr - DEV_BASE) & (DEV_SIZE - 1);
    d->rd_count++;
    return (uint32_t)d->regs[off]
         | ((uint32_t)d->regs[(off + 1) & (DEV_SIZE - 1)] << 8)
         | ((uint32_t)d->regs[(off + 2) & (DEV_SIZE - 1)] << 16)
         | ((uint32_t)d->regs[(off + 3) & (DEV_SIZE - 1)] << 24);
}

static void dummy_wr32(void *ctx, uint32_t addr, uint32_t v)
{
    DummyDev *d = ctx;
    uint32_t off = (addr - DEV_BASE) & (DEV_SIZE - 1);
    d->wr_count++;
    d->regs[off] = (uint8_t)v;
    d->regs[(off + 1) & (DEV_SIZE - 1)] = (uint8_t)(v >> 8);
    d->regs[(off + 2) & (DEV_SIZE - 1)] = (uint8_t)(v >> 16);
    d->regs[(off + 3) & (DEV_SIZE - 1)] = (uint8_t)(v >> 24);
}

typedef struct MmioRig {
    Rig        core;      /* RAM head region + CPU, as in WP1 */
    DummyDev   dev;
    mem_region dev_region;
} MmioRig;

static void mmio_rig_init(MmioRig *m)
{
    rig_init(&m->core);
    memset(&m->dev, 0, sizeof m->dev);
    memset(&m->dev_region, 0, sizeof m->dev_region);
    m->dev_region.base = DEV_BASE;
    m->dev_region.size = DEV_SIZE;
    m->dev_region.ctx = &m->dev;
    m->dev_region.rd8 = dummy_rd8;
    m->dev_region.wr8 = dummy_wr8;
    m->dev_region.rd32 = dummy_rd32;
    m->dev_region.wr32 = dummy_wr32;
    mem_region_add(&m->core.bus, &m->dev_region);
}

int main(void)
{
    char det[128];

    /* 1-3: direct bus dispatch to the dummy device */
    {
        MmioRig m; mmio_rig_init(&m);
        mem_wr32(&m.core.bus, DEV_BASE + 0x10, 0xCAFEBABEu);
        snprintf(det, sizeof det, "wr_count=%" PRIu64, m.dev.wr_count);
        check("mmio-wr32-dispatch", m.dev.wr_count == 1, det);
        uint32_t v = mem_rd32(&m.core.bus, DEV_BASE + 0x10);
        snprintf(det, sizeof det, "v=%" PRIx32 " rd_count=%" PRIu64, v, m.dev.rd_count);
        check("mmio-rd32-dispatch", v == 0xCAFEBABEu && m.dev.rd_count == 1, det);
        mem_wr8(&m.core.bus, DEV_BASE + 0x20, 0x5Au);
        uint8_t b = mem_rd8(&m.core.bus, DEV_BASE + 0x20);
        snprintf(det, sizeof det, "b=%" PRIx32, b);
        check("mmio-byte-access", b == 0x5A, det);
    }

    /* 4-6: CPU STR/LDR reach the device through the registry */
    {
        MmioRig m; mmio_rig_init(&m);
        /* r1 = 0x10000000 (MOV r1,#1; MOV r1,r1,LSL#28); r0=0x42; STR r0,[r1]; LDR r2,[r1]; SWI 0 */
        uint32_t prog[] = {
            0xE3A01001u,  /* MOV r1,#1 */
            0xE1A01E01u,  /* MOV r1,r1,LSL#28 */
            0xE3A00042u,  /* MOV r0,#0x42 */
            0xE5810000u,  /* STR r0,[r1] */
            0xE5912000u,  /* LDR r2,[r1] */
            0xEF000000u,  /* SWI 0 (halt) */
        };
        rig_load(&m.core, prog, 6, 0);
        arm26_run(&m.core.cpu, 10);
        snprintf(det, sizeof det, "r2=%" PRIx32 " wr=%" PRIu64 " rd=%" PRIu64,
                 m.core.cpu.r[2], m.dev.wr_count, m.dev.rd_count);
        check("cpu-str-ld hit device",
              m.core.cpu.r[2] == 0x42 && m.dev.wr_count == 1 && m.dev.rd_count == 1, det);
    }

    /* 7: unmapped access is safe (open bus) and counted */
    {
        MmioRig m; mmio_rig_init(&m);
        uint32_t v = mem_rd32(&m.core.bus, 0x30000000u);
        mem_wr32(&m.core.bus, 0x30000000u, 0x12345678u);
        snprintf(det, sizeof det, "v=%" PRIx32 " unmapped_rd=%" PRIu64 " unmapped_wr=%" PRIu64,
                 v, m.core.bus.unmapped_rd, m.core.bus.unmapped_wr);
        check("unmapped-open-bus",
              v == 0xFFFFFFFFu && m.core.bus.unmapped_rd == 1 && m.core.bus.unmapped_wr == 1, det);
    }

    /* 8: RAM head region still serves addresses below ram_size (WP1 regression path) */
    {
        MmioRig m; mmio_rig_init(&m);
        uint32_t prog[] = {
            0xE3A00001u,  /* MOV r0,#1 */
            0xE3A01C02u,  /* MOV r1,#0x200 (imm 0x02 rot 4 -> 0x200) */
            0xE5810000u,  /* STR r0,[r1] */
            0xE5912000u,  /* LDR r2,[r1] */
            0xEF000000u,  /* SWI 0 */
        };
        rig_load(&m.core, prog, 5, 0);
        arm26_run(&m.core.cpu, 10);
        snprintf(det, sizeof det, "r2=%" PRIu32 " dev untouched wr=%" PRIu64,
                 m.core.cpu.r[2], m.dev.wr_count);
        check("ram-region-unaffected", m.core.cpu.r[2] == 1 && m.dev.wr_count == 0, det);
    }

    /* 9: region priority - first registered match wins on overlap */
    {
        MmioRig m; mmio_rig_init(&m);
        DummyDev dev2; memset(&dev2, 0, sizeof dev2);
        mem_region r2;
        memset(&r2, 0, sizeof r2);
        r2.base = DEV_BASE;          /* overlaps the first device exactly */
        r2.size = DEV_SIZE;
        r2.ctx = &dev2;
        r2.rd8 = dummy_rd8;
        r2.wr8 = dummy_wr8;
        r2.rd32 = dummy_rd32;
        r2.wr32 = dummy_wr32;
        mem_region_add(&m.core.bus, &r2);   /* appended: lower priority */
        mem_wr32(&m.core.bus, DEV_BASE + 0x30, 0xABCu);
        snprintf(det, sizeof det, "dev1.wr=%" PRIu64 " dev2.wr=%" PRIu64,
                 m.dev.wr_count, dev2.wr_count);
        check("region-priority-first-wins", m.dev.wr_count == 1 && dev2.wr_count == 0, det);
    }

    /* final word of the RAM region: every byte is masked, so the top word
     * round-trips correctly (the JS reference leaves bytes a+1..a+3 unmasked
     * there; see docs/reference-manual.md section 7) */
    {
        MmioRig m; mmio_rig_init(&m);
        mem_wr32(&m.core.bus, RIG_RAM_SIZE - 4, 0xAABBCCDDu);
        uint32_t v = mem_rd32(&m.core.bus, RIG_RAM_SIZE - 4);
        snprintf(det, sizeof det, "v=%" PRIx32, v);
        check("ram-top-word-masked", v == 0xAABBCCDDu, det);
    }

    return test_summary("mmio") ? 1 : 0;
}
