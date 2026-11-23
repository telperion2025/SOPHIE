/*
 * sophie.c - WP5 host frontend: boot a ROM image in the Archimedes machine
 * and present VIDC frames through the host window layer, pumping host
 * keyboard input into the IOC KART.
 *
 * Usage: tools/sophie <rom-image> [max-frames] [--ppm out.ppm] [--disc N=path.adf] [--ro]
 *   default max-frames: run until the window is closed / CPU halts
 *   --ppm: headless backend writes each presented frame as a PPM snapshot
 *   --disc N=path: attach a disc image to floppy drive N (0-3), WP7
 *   --ro: attach disc images write-protected
 *   (build with `make host-x11` for a real X11 window when DISPLAY is set)
 *
 * Not part of `make test` (needs a ROM image, which is local runtime data).
 * Licence: Apache-2.0 (see LICENSE).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "host.h"

#define FB_W 1024
#define FB_H 768
#define CYCLES_PER_FRAME 160000u /* 8 MHz / 50 Hz */

static uint8_t rom[0x400000];
static uint8_t ram[0x400000];
static uint32_t fb[FB_W * FB_H];

int main(int argc, char **argv)
{
    const char *rompath = argc > 1 ? argv[1] : "roms/ROM300";
    long max_frames = -1;
    const char *ppm = NULL;
    const char *disc_arg[4] = { NULL, NULL, NULL, NULL };
    int ro = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--ppm") == 0 && i + 1 < argc)
            ppm = argv[++i];
        else if (strcmp(argv[i], "--disc") == 0 && i + 1 < argc) {
            const char *spec = argv[++i];
            int drv = spec[0] - '0';
            if (drv >= 0 && drv < 4 && spec[1] == '=')
                disc_arg[drv] = spec + 2;
            else
                fprintf(stderr, "sophie: bad --disc spec '%s' (want N=path)\n", spec);
        } else if (strcmp(argv[i], "--ro") == 0)
            ro = 1;
        else
            max_frames = strtol(argv[i], NULL, 0);
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
            printf("sophie: disc %d = %s (%s)\n", d, disc_arg[d], ro ? "ro" : "rw");
        }
    }

    HostWindow *win = host_open("SOPHIE", FB_W, FB_H);
    if (win == NULL) {
        fprintf(stderr, "host_open failed\n");
        return 2;
    }
    if (ppm != NULL)
        host_headless_set_ppm(win, ppm);

    printf("sophie: rom=%s (%zu bytes), frame=%ux%u, %u cycles/frame\n",
           rompath, rom_size, FB_W, FB_H, CYCLES_PER_FRAME);

    long frame = 0;
    while (!host_should_quit(win) && !m.cpu.halted && (max_frames < 0 || frame < max_frames)) {
        machine_run(&m, CYCLES_PER_FRAME);
        vidc_render(&m.vidc, &m.bus, fb, FB_W, FB_H);
        host_present(win, fb, FB_W, FB_H);
        host_pump_input(win, &m);
        frame++;
    }

    printf("sophie: stopped after %ld frames, pc=%08x mode=%u cycles=%llu\n",
           frame, m.cpu.r[15] & PC_MASK, m.cpu.r[15] & 3,
           (unsigned long long)m.cpu.cycles);
    host_close(win);
    return 0;
}
