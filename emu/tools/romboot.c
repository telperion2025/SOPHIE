/*
 * romboot.c - WP4 smoke tool: load a ROM image into the Archimedes base
 * machine and run N instructions, printing where the CPU got to.
 *
 * Not part of `make test` (needs a ROM image, which is local runtime data).
 * Run: ./tools/romboot [rom-image] [instructions] [--cmos file] [--disc N=path] [--ro]
 *   (default roms/ROM300, 2M; --cmos loads/saves the battery-backed CMOS RAM;
 *    --disc N=path attaches a disc image to floppy drive N (0-3), --ro write-protects)
 * Licence: Apache-2.0 (see LICENSE).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "machine.h"

static uint8_t rom[0x400000]; /* 4MB ROM window */
static uint8_t ram[0x400000]; /* 4MB DRAM */

int main(int argc, char **argv)
{
    const char *rompath = "roms/ROM300";
    uint64_t n = 2000000;
    const char *cmos_path = NULL;
    const char *disc_arg[4] = { NULL, NULL, NULL, NULL };
    int ro = 0;
    int pos = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cmos") == 0 && i + 1 < argc)
            cmos_path = argv[++i];
        else if (strcmp(argv[i], "--disc") == 0 && i + 1 < argc) {
            const char *spec = argv[++i];
            int drv = spec[0] - '0';
            if (drv >= 0 && drv < 4 && spec[1] == '=')
                disc_arg[drv] = spec + 2;
            else
                fprintf(stderr, "romboot: bad --disc spec '%s' (want N=path)\n", spec);
        }
        else if (strcmp(argv[i], "--ro") == 0)
            ro = 1;
        else if (pos == 0)
            rompath = argv[i], pos++;
        else if (pos == 1)
            n = strtoull(argv[i], NULL, 0), pos++;
    }

    FILE *f = fopen(rompath, "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s (ROM images are local runtime data; see README)\n", rompath);
        return 2;
    }
    size_t rom_size = fread(rom, 1, sizeof rom, f);
    fclose(f);

    Machine m;
    machine_init(&m, ram, sizeof ram, rom, (uint32_t)rom_size);
    for (int d = 0; d < 4; d++) {
        if (disc_arg[d] != NULL) {
            if (machine_disc_attach(&m, d, disc_arg[d], ro) != 0) {
                fprintf(stderr, "cannot attach disc %d: %s\n", d, disc_arg[d]);
                return 2;
            }
            printf("disc %d = %s (%s)\n", d, disc_arg[d], ro ? "ro" : "rw");
        }
    }
    if (cmos_path != NULL && cmos_load(&m.cmos, cmos_path) == 0)
        printf("cmos ram loaded from %s\n", cmos_path);
    machine_run(&m, n);
    if (cmos_path != NULL && cmos_save(&m.cmos, cmos_path) == 0)
        printf("cmos ram saved to %s\n", cmos_path);

    printf("rom=%s (%zu bytes)\n", rompath, rom_size);
    printf("ran=%" PRIu64 " cycles, halted=%d\n", m.cpu.cycles, m.cpu.halted);
    printf("pc=%08" PRIx32 " mode=%" PRIu32 " cpsr=%08" PRIx32 "\n",
           m.cpu.r[15] & PC_MASK, m.cpu.r[15] & 3, m.cpu.r[15]);
    printf("memc: latchrom=%d pagesize_idx=%d pages[0]=%d ctrl=%08" PRIx32 " vidstart=%08" PRIx32 "\n",
           m.memc.latchrom, m.memc.pagesize_idx, m.memc.pages[0], m.memc.ctrl, m.memc.vidstart);
    printf("bus: unmapped rd=%" PRIu64 " wr=%" PRIu64 " dabts=%" PRIu64 " dabt_addr=%08" PRIx32 "\n",
           m.bus.unmapped_rd, m.bus.unmapped_wr, m.bus.dabt_count, m.bus.dabt_addr);
    printf("fdc: status=%02x track=%u sector=%u curtrack=%d intrq=%d drq=%d fiq_status=%02x\n",
           m.fdc.status, m.fdc.track, m.fdc.sector, m.fdc.curtrack, m.fdc.intrq, m.fdc.drq,
           m.ioc.regs[IOC_FIQ_STATUS]);
    printf("cmos: ctrl=%02x clock=%02x %02x:%02x:%02x date=%02x mon/wd=%02x (regs 0x00-0x06)\n",
           m.cmos.data[0x00], m.cmos.data[0x01], m.cmos.data[0x02], m.cmos.data[0x03],
           m.cmos.data[0x04], m.cmos.data[0x05], m.cmos.data[0x06]);
    return 0;
}
