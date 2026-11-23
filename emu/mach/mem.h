/*
 * mem.h - SOPHIE emulator memory bus and device registry (WP2).
 *
 * All guest memory access is mediated by this layer: the CPU core never
 * touches raw storage directly. Regions are matched in registration order,
 * first match wins. A region flagged is_ram is a flat byte array and is
 * checked first as a fast path.
 *
 * Portability rule (spec section 3, PowerPC plan): no endianness or
 * alignment assumptions anywhere in this layer; word access is composed
 * byte by byte, so guest little-endian semantics hold on any host.
 *
 * Unmapped reads return 0xFF per byte (open bus); unmapped writes are
 * ignored. Both are counted for diagnostics.
 *
 * Licence: Apache-2.0 (see LICENSE). Written clean from the specification.
 */
#ifndef SOPHIE_MEM_H
#define SOPHIE_MEM_H

#include <stdint.h>

typedef struct mem_region {
    uint32_t base;          /* first covered address */
    uint32_t size;          /* bytes covered: [base, base+size) */
    int      is_ram;        /* flat byte array fast path */
    uint8_t *ram;           /* if is_ram */
    uint32_t ram_mask;      /* if is_ram: ram_size-1 (power of two) */
    void    *ctx;           /* device context for callbacks */
    uint8_t  (*rd8)(void *ctx, uint32_t addr);
    void     (*wr8)(void *ctx, uint32_t addr, uint8_t v);
    uint32_t (*rd32)(void *ctx, uint32_t addr);   /* optional; composed from rd8 if NULL */
    void     (*wr32)(void *ctx, uint32_t addr, uint32_t v); /* optional; split to wr8 if NULL */
    struct mem_region *next;
} mem_region;

typedef struct mem_bus {
    mem_region  ram_region; /* embedded head region when initialised with RAM */
    mem_region *regions;    /* linked list, head first = highest priority */
    uint64_t    unmapped_rd;
    uint64_t    unmapped_wr;
    /* MEMC1a supervisor-only I/O window: a data access to
     * [io_base, io_base+io_size) while user_mode is set aborts instead of
     * reaching the device (read returns open bus, write is dropped) and
     * latches the dabt flag. Instruction fetches (fetch set) are exempt, as
     * are privileged modes. Only the CPU core drives fetch/user_mode, and
     * only around its own accesses, so host/device bus users never abort. */
    int      io_abort_en;
    uint32_t io_base, io_size;
    int      fetch, user_mode;
    int      dabt;
    uint32_t dabt_addr;
    uint64_t dabt_count;
} mem_bus;

/* Initialise a bus whose head region is flat RAM at [0, ram_size).
 * ram_size must be a power of two. Pass ram=NULL for an empty bus. */
void mem_bus_init(mem_bus *bus, uint8_t *ram, uint32_t ram_size);

/* Append a region to the registry (lowest priority so far). */
void mem_region_add(mem_bus *bus, mem_region *r);

uint8_t  mem_rd8(mem_bus *bus, uint32_t addr);
void     mem_wr8(mem_bus *bus, uint32_t addr, uint8_t v);
uint32_t mem_rd32(mem_bus *bus, uint32_t addr);
void     mem_wr32(mem_bus *bus, uint32_t addr, uint32_t v);

/* Host-side helpers: little-endian word read/write of raw memory (no bus). */
uint32_t mem_get32(const uint8_t *p);
void     mem_put32(uint8_t *p, uint32_t v);

#endif /* SOPHIE_MEM_H */
