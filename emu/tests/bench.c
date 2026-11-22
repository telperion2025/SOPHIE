/*
 * bench.c - WP1 acceptance benchmark: interpreter throughput (MIPS).
 *
 * Runs the same hand-encoded loop as emu/ref/bench.mjs so the C core can
 * be compared against the JS reference on the same host (spec WP1: the C
 * benchmark must be within 3x of the JS reference; in practice it is far
 * faster).
 *
 * Run: ./bench [instructions]   (via `make bench`)
 * Licence: Apache-2.0 (see LICENSE).
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "test.h"

int main(int argc, char **argv)
{
    uint64_t n = (argc > 1) ? strtoull(argv[1], NULL, 0) : 1000000ull;

    Rig g; rig_init(&g);
    uint32_t prog[] = {
        0xE3A0000Au,  /* MOV r0,#10 */
        0xE3A01000u,  /* MOV r1,#0 */
        0xE0811000u,  /* ADD r1,r1,r0 */
        0xE2500001u,  /* SUBS r0,r0,#1 */
        0x1AFFFFFCu,  /* BNE 0x08 */
    };
    rig_load(&g, prog, 5, 0);

    arm26_run(&g.cpu, 3); /* warm */

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    arm26_run(&g.cpu, n);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    double mips = (double)g.cpu.cycles / secs / 1e6;
    printf("c bench: %" PRIu64 " instructions in %.1f ms -> %.1f MIPS (r1=%u)\n",
           g.cpu.cycles, secs * 1e3, mips, g.cpu.r[1]);
    return 0;
}
