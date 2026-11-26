/*
 * sophie.c - WP5 host frontend: boot a ROM image in the Archimedes machine
 * and present VIDC frames through the host window layer, pumping host
 * keyboard input into the IOC KART.
 *
 * Usage: tools/sophie <rom-image> [max-frames] [--ppm out.ppm] [--disc N=path.adf] [--ro]
 *                               [--cmos file] [--keys "text"] [--keys-at frame]
 *                               [--stub-desktop]
 *   default max-frames: run until the window is closed / CPU halts
 *   --ppm: headless backend writes each presented frame as a PPM snapshot
 *   --disc N=path: attach a disc image to floppy drive N (0-3), WP7
 *   --ro: attach disc images write-protected
 *   --cmos file: load/save the battery-backed CMOS RAM (0x10-0xFF) so
 *   *Configure settings persist across runs (created on first save)
 *   --keys "text": type text into the guest (one char per 4 frames from
 *   --keys-at frame, default 0; '\\n' = RETURN); drives the CLI/Wimp, M3
 *   --stub-desktop: skip the ROM boot and render a built-in stub desktop
 *   (backdrop + icon bar + window + pointer) through the real VIDC/MEMC
 *   pipeline - placeholder for M3 while the ROM boot stall (issue #3) is
 *   unresolved; max-frames defaults to 100 in this mode
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
#include "vidc.h"
#include "memc.h"

#define FB_W 1024
#define FB_H 768
#define CYCLES_PER_FRAME 160000u /* 8 MHz / 50 Hz */

static uint8_t rom[0x400000];
/* 1MB DRAM (A3000-class); 4MB triggers the ROM300 reset-path zero-page derail */
static uint8_t ram[0x100000];
static uint32_t fb[FB_W * FB_H];

/* M3 stub desktop: 640x256 4bpp (2 px/byte, 320 bytes/line), drawn at the
 * bottom of DRAM (vidstart=0), rendered through the real VIDC pipeline.
 * Clearly a stub until the ROM Wimp boots (issue #3). */
#define STUB_CPL 320u
#define STUB_W 640u
#define STUB_H 256u

static void stub_setpx(Machine *m, uint32_t x, uint32_t y, uint32_t pen)
{
    if (x >= STUB_W || y >= STUB_H)
        return;
    uint32_t off = MACH_PHYS_BASE + (y * STUB_CPL) + (x >> 1);
    uint8_t b = mem_rd8(&m->bus, off);
    if (x & 1)
        b = (uint8_t)((b & 0xF0u) | (pen & 0xFu));
    else
        b = (uint8_t)((b & 0x0Fu) | ((pen & 0xFu) << 4));
    mem_wr8(&m->bus, off, b);
}

static void stub_fill(Machine *m, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t pen)
{
    for (uint32_t y = y0; y < y0 + h && y < STUB_H; y++)
        for (uint32_t x = x0; x < x0 + w && x < STUB_W; x++)
            stub_setpx(m, x, y, pen);
}

static void stub_desktop(Machine *m)
{
    /* palette: 0 backdrop, 1 window fill, 2 outline/text, 3 title bar/icon bar */
    mem_wr32(&m->bus, VIDC_BASE, (0x00u << 24) | 0x77Bu); /* pen 0 */
    mem_wr32(&m->bus, VIDC_BASE, (0x04u << 24) | 0xFFFu); /* pen 1 */
    mem_wr32(&m->bus, VIDC_BASE, (0x08u << 24) | 0x000u); /* pen 2 */
    mem_wr32(&m->bus, VIDC_BASE, (0x0Cu << 24) | 0x00Fu); /* pen 3 red */
    mem_wr32(&m->bus, VIDC_BASE, (0x40u << 24) | 0x000u); /* border black */
    /* control: pixel clock 16 MHz (2), 4bpp (2) */
    mem_wr32(&m->bus, VIDC_BASE, (0xE0u << 24) | (2u | (2u << 2)));
    /* CRTC raws (value << 14): 640x256 display */
    mem_wr32(&m->bus, VIDC_BASE, (VIDC_CRTC_REG(VIDC_CRTC_HCR)  << 24) | (0x190u << 14));
    mem_wr32(&m->bus, VIDC_BASE, (VIDC_CRTC_REG(VIDC_CRTC_HBSR) << 24) | (0x020u << 14));
    mem_wr32(&m->bus, VIDC_BASE, (VIDC_CRTC_REG(VIDC_CRTC_HDSR) << 24) | (0x030u << 14));
    mem_wr32(&m->bus, VIDC_BASE, (VIDC_CRTC_REG(VIDC_CRTC_HDER) << 24) | (0x170u << 14));
    mem_wr32(&m->bus, VIDC_BASE, (VIDC_CRTC_REG(VIDC_CRTC_HBER) << 24) | (0x180u << 14));
    mem_wr32(&m->bus, VIDC_BASE, (VIDC_CRTC_REG(VIDC_CRTC_VCR)  << 24) | (0x130u << 14));
    mem_wr32(&m->bus, VIDC_BASE, (VIDC_CRTC_REG(VIDC_CRTC_VBSR) << 24) | (0x008u << 14));
    mem_wr32(&m->bus, VIDC_BASE, (VIDC_CRTC_REG(VIDC_CRTC_VDSR) << 24) | (0x010u << 14));
    mem_wr32(&m->bus, VIDC_BASE, (VIDC_CRTC_REG(VIDC_CRTC_VDER) << 24) | (0x110u << 14));
    mem_wr32(&m->bus, VIDC_BASE, (VIDC_CRTC_REG(VIDC_CRTC_VBER) << 24) | (0x120u << 14));
    /* MEMC: video start 0, video end = screen size, 32KB pages + video DMA on */
    mem_wr32(&m->bus, MEMC_BASE, 0x03600000u | (1u << 17) | 0u);                       /* vidstart 0 */
    mem_wr32(&m->bus, MEMC_BASE, 0x03600000u | (2u << 17) | ((STUB_CPL * STUB_H / 16) << 2)); /* vidend */
    mem_wr32(&m->bus, MEMC_BASE, 0x03600000u | (7u << 17) | (3u << 2) | (1u << 10));   /* ctrl */

    /* draw the stub desktop */
    stub_fill(m, 0, 0, STUB_W, STUB_H, 0);            /* backdrop */
    stub_fill(m, 0, STUB_H - 32, STUB_W, 32, 3);       /* icon bar */
    for (uint32_t i = 0; i < 8; i++)                   /* icons on the bar */
        stub_fill(m, 16 + i * 64, STUB_H - 24, 40, 24, (uint32_t)(1 + (i & 1)));
    stub_fill(m, 40, 20, 560, 180, 1);                 /* window */
    stub_fill(m, 40, 20, 560, 16, 3);                  /* title bar */
    for (uint32_t x = 40; x < 600; x++) { stub_setpx(m, x, 20, 2); stub_setpx(m, x, 199, 2); }
    for (uint32_t y = 20; y < 200; y++) { stub_setpx(m, 40, y, 2); stub_setpx(m, 599, y, 2); }
    /* mouse pointer arrow at (500,150) */
    static const uint32_t arrow[8] = { 2, 0, 0, 0, 0, 0, 0, 0 };
    for (uint32_t r = 0; r < 8; r++)
        for (uint32_t c = 0; c <= r; c++)
            stub_setpx(m, 500 + c, 150 + r, arrow[0]);
}

int main(int argc, char **argv)
{
    const char *rompath = argc > 1 ? argv[1] : "roms/ROM300";
    long max_frames = -1;
    const char *ppm = NULL;
    const char *disc_arg[4] = { NULL, NULL, NULL, NULL };
    const char *cmos_path = NULL;
    const char *keys = NULL;
    long keys_at = 0;
    int stub = 0;
    int ro = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--ppm") == 0 && i + 1 < argc)
            ppm = argv[++i];
        else if (strcmp(argv[i], "--cmos") == 0 && i + 1 < argc)
            cmos_path = argv[++i];
        else if (strcmp(argv[i], "--keys") == 0 && i + 1 < argc)
            keys = argv[++i];
        else if (strcmp(argv[i], "--keys-at") == 0 && i + 1 < argc)
            keys_at = strtol(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "--stub-desktop") == 0)
            stub = 1;
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

    size_t rom_size = 0;
    if (!stub) {
        FILE *f = fopen(rompath, "rb");
        if (f == NULL) {
            fprintf(stderr, "cannot open %s (ROM images are local runtime data; see README)\n", rompath);
            return 2;
        }
        rom_size = fread(rom, 1, sizeof rom, f);
        fclose(f);
    }

    Machine m;
    machine_init(&m, ram, sizeof ram, rom, (uint32_t)rom_size);
    if (stub && max_frames < 0)
        max_frames = 100;
    if (stub)
        stub_desktop(&m);
    if (cmos_path != NULL && cmos_load(&m.cmos, cmos_path) == 0)
        printf("sophie: cmos ram loaded from %s\n", cmos_path);

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
    size_t typed = 0;
    while (!host_should_quit(win) && !m.cpu.halted && (max_frames < 0 || frame < max_frames)) {
        if (!stub)
            machine_run(&m, CYCLES_PER_FRAME);
        vidc_render(&m.vidc, &m.bus, fb, FB_W, FB_H);
        if (keys != NULL && frame >= keys_at && typed < strlen(keys) && (frame - keys_at) % 4 == 0) {
            int k = (unsigned char)keys[typed++];
            if (k == '\n')
                k = HOST_KEY_RETURN;
            else if (k == '\t')
                k = HOST_KEY_TAB;
            host_headless_push_key(win, k, 1);
            host_headless_push_key(win, k, 0);
        }
        host_present(win, fb, FB_W, FB_H);
        host_pump_input(win, &m);
        frame++;
    }
    long nonblack = 0;
    for (int i = 0; i < FB_W * FB_H; i++)
        if (fb[i] != m.vidc.pal[0x110])
            nonblack++;

    printf("sophie: stopped after %ld frames, pc=%08x mode=%u cycles=%llu nonblack=%ld%s\n",
           frame, m.cpu.r[15] & PC_MASK, m.cpu.r[15] & 3,
           (unsigned long long)m.cpu.cycles, nonblack, stub ? " (stub-desktop)" : "");
    if (cmos_path != NULL && cmos_save(&m.cmos, cmos_path) == 0)
        printf("sophie: cmos ram saved to %s\n", cmos_path);
    host_close(win);
    return 0;
}
