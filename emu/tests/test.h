/*
 * test.h - minimal check() harness shared by the SOPHIE test suites.
 * Licence: Apache-2.0 (see LICENSE).
 */
#ifndef SOPHIE_TEST_H
#define SOPHIE_TEST_H

#include <stdio.h>
#include <inttypes.h>
#include <string.h>

static int g_passed = 0;
static int g_failed = 0;

static inline void check(const char *name, int cond, const char *detail)
{
    if (cond) {
        g_passed++;
    } else {
        g_failed++;
        printf("  FAIL %s: %s\n", name, detail ? detail : "");
    }
}

static inline int test_summary(const char *suite)
{
    printf("%s: %d passed, %d failed\n", suite, g_passed, g_failed);
    return g_failed;
}

/* Rig: a CPU on a bus whose head region is flat RAM (the WP1/WP2 setup). */
#include "arm26.h"

#define RIG_RAM_SIZE 0x40000u

typedef struct Rig {
    Arm26   cpu;
    mem_bus bus;
    uint8_t ram[RIG_RAM_SIZE];
} Rig;

static inline void rig_init(Rig *g)
{
    memset(g->ram, 0, RIG_RAM_SIZE); /* JS reference uses a zeroed Uint8Array */
    mem_bus_init(&g->bus, g->ram, RIG_RAM_SIZE);
    arm26_init(&g->cpu, &g->bus);
}

static inline void rig_load(Rig *g, const uint32_t *words, unsigned n, uint32_t base)
{
    arm26_load_program(&g->cpu, words, n, base);
}

static inline void rig_setpc(Rig *g, uint32_t pc)
{
    g->cpu.r[15] = (g->cpu.r[15] & ~PC_MASK) | (pc & PC_MASK);
}

#endif /* SOPHIE_TEST_H */
