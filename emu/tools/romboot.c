/*
 * romboot.c - WP4 smoke tool: load a ROM image into the Archimedes base
 * machine and run N instructions, printing where the CPU got to.
 *
 * Not part of `make test` (needs a ROM image, which is local runtime data).
 * Run: ./tools/romboot [rom-image] [instructions]     (default roms/ROM300, 2M)
 * Licence: Apache-2.0 (see LICENSE).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include "machine.h"

static uint8_t rom[0x400000]; /* 4MB ROM window */
static uint8_t ram[0x400000]; /* 4MB DRAM */

int main(int argc, char **argv)
{
    const char *rompath = argc > 1 ? argv[1] : "roms/ROM300";
    uint64_t n = argc > 2 ? strtoull(argv[2], NULL, 0) : 2000000;

    FILE *f = fopen(rompath, "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s (ROM images are local runtime data; see README)\n", rompath);
        return 2;
    }
    size_t rom_size = fread(rom, 1, sizeof rom, f);
    fclose(f);

    Machine m;
    machine_init(&m, ram, sizeof ram, rom, (uint32_t)rom_size);
    machine_run(&m, n);

    printf("rom=%s (%zu bytes)\n", rompath, rom_size);
    printf("ran=%" PRIu64 " cycles, halted=%d\n", m.cpu.cycles, m.cpu.halted);
    printf("pc=%08" PRIx32 " mode=%" PRIu32 " cpsr=%08" PRIx32 "\n",
           m.cpu.r[15] & PC_MASK, m.cpu.r[15] & 3, m.cpu.r[15]);
    printf("memc: latchrom=%d pagesize_idx=%d pages[0]=%d ctrl=%08" PRIx32 " vidstart=%08" PRIx32 "\n",
           m.memc.latchrom, m.memc.pagesize_idx, m.memc.pages[0], m.memc.ctrl, m.memc.vidstart);
    printf("bus: unmapped rd=%" PRIu64 " wr=%" PRIu64 "\n", m.bus.unmapped_rd, m.bus.unmapped_wr);
    return 0;
}
