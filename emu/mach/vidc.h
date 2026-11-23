/*
 * vidc.h - VIDC1 video/sound controller model (WP3).
 *
 * Register region: 0x03400000-0x035FFFFF, write-only. A word store to any
 * address in the region delivers {register, value} as reg = v>>24,
 * val = v & 0xFFFFFF (ARM STRB replicates the byte on all four bus lanes,
 * so reg = byte, val = 0xVVVVVV). Reads return open bus.
 *
 * VIDC1 register file (reg byte = v>>24; within each block the low two bits
 * of the reg byte are ignored, index = (reg & 0xFC) >> 2):
 *   0x00-0x3F display palette, 16 entries at reg 4*i, 12-bit BGR colour
 *             (b=bits 11:8, g=7:4, r=3:0); entry i also feeds the 8bpp pen
 *             group i*0x10..i*0x10+0xF0 (the pixel's high nibble supplies
 *             the high nibble of each gun, the register the low nibble)
 *   0x40-0x4F cursor palette, 4 entries at reg 0x40+4*i; entry 0 (reg 0x40)
 *             doubles as the border colour
 *   0x60-0x7F stereo image, 8 channels at reg 0x60+4*c (capture only)
 *   0x80-0xBF CRTC, 16 registers at reg 0x80+4*i (see VIDC_CRTC_*)
 *   0xC0      sound frequency latch (capture only)
 *   0xE0      control: bits 1:0 pixel clock ({8,12,16,24} MHz),
 *             bits 3:2 bpp mode (0=1bpp, 1=2bpp, 2=4bpp, 3=8bpp),
 *             bit 6 interlace
 *
 * CRTC decode (VIDC1 arithmetic): HCR = ((d>>14)<<1)+2,
 * HBSR = HBER = ((d>>14)<<1)+1, HDSR/HDER = (d>>14)*2 + xstep[bpp] with
 * xstep = {19,11,7,5}, VCR/VSWR/VBSR/VDSR/VDER/VBER = (d>>14)+1,
 * HCSR = ((d>>13)&0x7ff)+6, VCSR/VCER = ((d>>14)&0x3ff)+1.
 * HDSR/HDER depend on the bpp mode and are re-decoded when control changes.
 *
 * vidc_render() rasterises the current mode from the MEMC video-start
 * address into a host 32bpp framebuffer. Sound DMA is modelled as
 * register capture only (M4).
 *
 * Licence: Apache-2.0 (see LICENSE). Register layout per Acorn VIDC1
 * datasheet; behaviour reference only from studied emulators.
 */
#ifndef SOPHIE_VIDC_H
#define SOPHIE_VIDC_H

#include <stdint.h>

#include "mem.h"

#define VIDC_BASE 0x03400000u
#define VIDC_SIZE 0x00200000u

#define VIDC_REG_PALETTE_DISPLAY 0x00u /* 0x00-0x3F, 16 entries at 4*i */
#define VIDC_REG_PALETTE_CURSOR  0x40u /* 0x40-0x4F, 4 entries; 0x40 = border */
#define VIDC_REG_STEREO          0x60u /* 0x60-0x7F, 8 channels (capture) */
#define VIDC_REG_CRTC            0x80u /* 0x80-0xBF, 16 regs at 0x80+4*i */
#define VIDC_REG_SOUNDFREQ       0xC0u /* 0xC0-0xC3 (capture) */
#define VIDC_REG_CONTROL         0xE0u /* 0xE0-0xE3 */

#define VIDC_CRTC_REG(i) (VIDC_REG_CRTC + 4u * (uint32_t)(i))

/* CRTC register indices (reg byte = VIDC_CRTC_REG(i)) */
enum {
    VIDC_CRTC_HCR = 0, VIDC_CRTC_HSWR, VIDC_CRTC_HBSR, VIDC_CRTC_HDSR,
    VIDC_CRTC_HDER, VIDC_CRTC_HBER, VIDC_CRTC_HCSR, VIDC_CRTC_HIR,
    VIDC_CRTC_VCR, VIDC_CRTC_VSWR, VIDC_CRTC_VBSR, VIDC_CRTC_VDSR,
    VIDC_CRTC_VDER, VIDC_CRTC_VBER, VIDC_CRTC_VCSR, VIDC_CRTC_VCER,
    VIDC_CRTC_COUNT
};

typedef struct Machine Machine;

typedef struct Vidc {
    uint32_t regs[0x100];          /* raw register file, indexed by reg byte */
    uint32_t crtc[VIDC_CRTC_COUNT];/* decoded CRTC values (VIDC1 arithmetic) */
    uint32_t hraw[2];              /* raw HDSR/HDER (d>>14), re-decoded on bpp change */
    uint32_t pal[0x114];           /* pen table: 0x00-0xFF 8bpp, 0x100-0x10F
                                      display 4bpp, 0x110-0x113 cursor/border */
    int      bpp;                  /* decoded control: 0..3 */
    int      pixel_clock;          /* decoded control: 0..3 */
    int      interlace;            /* decoded control bit 6 */
    uint64_t frame_acc;            /* CPU cycles accumulated towards the next vsync */
    Machine *mach;
} Vidc;

void vidc_init(Vidc *vidc, Machine *mach);
void vidc_write(Vidc *vidc, uint32_t v);   /* one register write */

/* Advance the frame timer by delta CPU cycles; each frame end raises the
 * flyback (vsync) interrupt through the IOC (IRQ status A bit 3). Frame
 * period = HCR * VCR pixel clocks, pixel clock selected by control bits
 * 1:0 ({8,12,16,24} MHz), CPU clock 8 MHz. */
void vidc_tick(Vidc *vidc, uint32_t cycles_delta);

/* region callbacks for [VIDC_BASE, VIDC_BASE+VIDC_SIZE) */
uint8_t  vidc_rd8(void *ctx, uint32_t addr);
void     vidc_wr8(void *ctx, uint32_t addr, uint8_t v);
uint32_t vidc_rd32(void *ctx, uint32_t addr);
void     vidc_wr32(void *ctx, uint32_t addr, uint32_t v);

/* Rasterise the current mode into fb (fb_w x fb_h, 0xRRGGBB pixels). */
void vidc_render(Vidc *vidc, mem_bus *bus, uint32_t *fb, int fb_w, int fb_h);

#endif /* SOPHIE_VIDC_H */
