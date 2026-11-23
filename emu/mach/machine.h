/*
 * machine.h - Archimedes base machine (WP3): memory map + device wiring.
 *
 * Memory map (32MB logical space, Acorn Archimedes A3xx/A4xx layout):
 *   0x00000000-0x01FFFFFF  logical window via MEMC (boot ROM latched at
 *                          reset; CAM-walked to DRAM after the CAM is set up)
 *   0x02000000-0x02FFFFFF  physical DRAM window (direct alias)
 *   0x03000000-0x031FFFFF  podule/expansion space (stub)
 *   0x03200000-0x033FFFFF  IOC (registers at low 0x80, 1MHz bus above 0x10000)
 *   0x03400000-0x035FFFFF  VIDC (write-only registers)
 *   0x03600000-0x037FFFFF  MEMC registers
 *   0x03800000-0x03BFFFFF  ROM (reads return ROM; writes program the MEMC CAM)
 *
 * machine_run() executes instructions and advances the IOC timers, so
 * IRQ-driven guest loops work. Host video/input frontends (WP5) attach to
 * vidc_render() and ioc_key_press().
 *
 * Licence: Apache-2.0 (see LICENSE).
 */
#ifndef SOPHIE_MACHINE_H
#define SOPHIE_MACHINE_H

#include <stdint.h>

#include "mem.h"
#include "arm26.h"
#include "memc.h"
#include "vidc.h"
#include "ioc.h"

#define MACH_LOGICAL_BASE 0x00000000u
#define MACH_LOGICAL_SIZE 0x02000000u
#define MACH_PHYS_BASE    0x02000000u
#define MACH_PHYS_SIZE    0x01000000u
#define MACH_PODULE_BASE  0x03000000u
#define MACH_PODULE_SIZE  0x00200000u
#define MACH_ROM_BASE     0x03800000u
#define MACH_ROM_SIZE     0x00400000u

typedef struct Machine {
    mem_bus  bus;
    Arm26    cpu;
    uint8_t *ram;
    uint32_t ram_size;
    uint32_t ram_mask;
    uint8_t *rom;
    uint32_t rom_size;
    Memc     memc;
    Vidc     vidc;
    Ioc      ioc;
    mem_region r_logical, r_phys, r_podule, r_ioc, r_vidc, r_memc, r_rom;
} Machine;

/* ram/rom buffers are owned by the caller and must outlive the machine. */
void machine_init(Machine *m, uint8_t *ram, uint32_t ram_size,
                  uint8_t *rom, uint32_t rom_size);

/* Run up to n instructions, ticking the IOC timers between batches. */
void machine_run(Machine *m, uint64_t n);

#endif /* SOPHIE_MACHINE_H */
