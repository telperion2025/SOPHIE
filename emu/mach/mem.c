/*
 * mem.c - SOPHIE emulator memory bus and device registry (WP2).
 * See mem.h for the dispatch contract. Licence: Apache-2.0 (see LICENSE).
 */
#include "mem.h"

#include <assert.h>
#include <string.h>

void mem_bus_init(mem_bus *bus, uint8_t *ram, uint32_t ram_size)
{
    memset(bus, 0, sizeof *bus);
    if (ram != NULL) {
        assert(ram_size != 0 && (ram_size & (ram_size - 1)) == 0);
        /* ram_size must be a power of two so the mask is exact */
        bus->ram_region.base = 0;
        bus->ram_region.size = ram_size;
        bus->ram_region.is_ram = 1;
        bus->ram_region.ram = ram;
        bus->ram_region.ram_mask = ram_size - 1;
        bus->ram_region.ctx = NULL;
        bus->ram_region.rd8 = NULL;
        bus->ram_region.wr8 = NULL;
        bus->ram_region.rd32 = NULL;
        bus->ram_region.wr32 = NULL;
        bus->ram_region.next = NULL;
        bus->regions = &bus->ram_region;
    }
}

void mem_region_add(mem_bus *bus, mem_region *r)
{
    r->next = NULL;
    if (bus->regions == NULL) {
        bus->regions = r;
        return;
    }
    mem_region *t = bus->regions;
    while (t->next != NULL)
        t = t->next;
    t->next = r;
}

static mem_region *mem_find(mem_bus *bus, uint32_t addr)
{
    /* fast path: head region is flat RAM covering base 0 */
    mem_region *head = bus->regions;
    if (head != NULL && head->is_ram && addr < head->size)
        return head;
    for (mem_region *r = bus->regions; r != NULL; r = r->next)
        if (addr >= r->base && addr < r->base + r->size)
            return r;
    return NULL;
}

uint8_t mem_rd8(mem_bus *bus, uint32_t addr)
{
    mem_region *r = mem_find(bus, addr);
    if (r == NULL) {
        bus->unmapped_rd++;
        return 0xFF;
    }
    if (r->is_ram)
        return r->ram[(addr - r->base) & r->ram_mask];
    if (r->rd8 != NULL)
        return r->rd8(r->ctx, addr);
    bus->unmapped_rd++;
    return 0xFF;
}

void mem_wr8(mem_bus *bus, uint32_t addr, uint8_t v)
{
    mem_region *r = mem_find(bus, addr);
    if (r == NULL) {
        bus->unmapped_wr++;
        return;
    }
    if (r->is_ram) {
        r->ram[(addr - r->base) & r->ram_mask] = v;
        return;
    }
    if (r->wr8 != NULL) {
        r->wr8(r->ctx, addr, v);
        return;
    }
    bus->unmapped_wr++;
}

uint32_t mem_rd32(mem_bus *bus, uint32_t addr)
{
    mem_region *r = mem_find(bus, addr);
    if (r == NULL) {
        bus->unmapped_rd++;
        return 0xFFFFFFFFu;
    }
    if (r->is_ram) {
        uint32_t o = (addr - r->base) & r->ram_mask;
        const uint8_t *p = r->ram;
        /* byte-composed: no alignment or endianness assumptions */
        return (uint32_t)p[o]
             | ((uint32_t)p[(o + 1) & r->ram_mask] << 8)
             | ((uint32_t)p[(o + 2) & r->ram_mask] << 16)
             | ((uint32_t)p[(o + 3) & r->ram_mask] << 24);
    }
    if (r->rd32 != NULL)
        return r->rd32(r->ctx, addr);
    /* compose little-endian from byte reads */
    return (uint32_t)mem_rd8(bus, addr)
         | ((uint32_t)mem_rd8(bus, addr + 1) << 8)
         | ((uint32_t)mem_rd8(bus, addr + 2) << 16)
         | ((uint32_t)mem_rd8(bus, addr + 3) << 24);
}

void mem_wr32(mem_bus *bus, uint32_t addr, uint32_t v)
{
    mem_region *r = mem_find(bus, addr);
    if (r == NULL) {
        bus->unmapped_wr++;
        return;
    }
    if (r->is_ram) {
        uint32_t o = (addr - r->base) & r->ram_mask;
        uint8_t *p = r->ram;
        p[o] = (uint8_t)(v & 0xFF);
        p[(o + 1) & r->ram_mask] = (uint8_t)((v >> 8) & 0xFF);
        p[(o + 2) & r->ram_mask] = (uint8_t)((v >> 16) & 0xFF);
        p[(o + 3) & r->ram_mask] = (uint8_t)((v >> 24) & 0xFF);
        return;
    }
    if (r->wr32 != NULL) {
        r->wr32(r->ctx, addr, v);
        return;
    }
    mem_wr8(bus, addr, (uint8_t)(v & 0xFF));
    mem_wr8(bus, addr + 1, (uint8_t)((v >> 8) & 0xFF));
    mem_wr8(bus, addr + 2, (uint8_t)((v >> 16) & 0xFF));
    mem_wr8(bus, addr + 3, (uint8_t)((v >> 24) & 0xFF));
}

uint32_t mem_get32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

void mem_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}
