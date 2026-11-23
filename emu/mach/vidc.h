/*
 * vidc.h - VIDC1 video/sound controller model (WP3).
 *
 * Register region: 0x03400000-0x035FFFFF, write-only. A word store to any
 * address in the region delivers {register, value} as reg = v>>24,
 * val = v & 0xFFFFFF (ARM STRB replicates the byte on all four bus lanes,
 * so reg = byte, val = 0xVVVVVV). Reads return open bus.
 *
 * VIDC1 register file (reg index = byte>>2 within the region):
 *   0x00-0x0F display palette (12-bit BGR: b=bits 11:8, g=7:4, r=3:0)
 *   0x10      border colour
 *   0x11-0x13 cursor palette
 *   0x18-0x1F stereo image
 *   0x20-0x2F CRTC: HCR HSWR HBSR HDSR HDER HBER HCSR HIR
 *               VCR VSWR VBSR VDSR VDER VBER VCSR VCER
 *   0x30      sound frequency latch
 *   0x38      control: bits 1:0 pixel clock, bits 3:2 bpp mode
 *               (0=1bpp,1=2bpp,2=4bpp,3=8bpp), bit 6 interlace
 *
 * vidc_render() decodes the CRTC registers (same arithmetic as the VIDC1
 * datasheet) and rasterises the display from the MEMC video-start address
 * into a host 32bpp framebuffer. Sound DMA is modelled as register
 * capture only (M4).
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

#define VIDC_REG_PALETTE   0x00u /* 0x00-0x0F */
#define VIDC_REG_BORDER    0x10u
#define VIDC_REG_CRTC      0x20u /* 0x20-0x2F */
#define VIDC_REG_SOUNDFREQ 0x30u
#define VIDC_REG_CONTROL   0x38u

/* CRTC register indices within 0x20-0x2F */
enum {
    VIDC_CRTC_HCR = 0, VIDC_CRTC_HSWR, VIDC_CRTC_HBSR, VIDC_CRTC_HDSR,
    VIDC_CRTC_HDER, VIDC_CRTC_HBER, VIDC_CRTC_HCSR, VIDC_CRTC_HIR,
    VIDC_CRTC_VCR, VIDC_CRTC_VSWR, VIDC_CRTC_VBSR, VIDC_CRTC_VDSR,
    VIDC_CRTC_VDER, VIDC_CRTC_VBER, VIDC_CRTC_VCSR, VIDC_CRTC_VCER
};

typedef struct Machine Machine;

typedef struct Vidc {
    uint32_t regs[64];     /* raw register file, indexed by reg byte */
    uint32_t pal4[16];     /* display palette, decoded to 0xRRGGBB */
    uint32_t pal8[256];    /* 8bpp expansion of the display palette */
    uint32_t border;       /* border colour, 0xRRGGBB */
    int      bpp;          /* decoded control: 0..3 */
    int      pixel_clock;  /* decoded control: 0..3 */
    int      interlace;    /* decoded control bit 6 */
    uint64_t frame_acc;    /* CPU cycles accumulated towards the next vsync */
    Machine *mach;
} Vidc;

void vidc_init(Vidc *vidc, Machine *mach);
void vidc_write(Vidc *vidc, uint32_t v);   /* one register write */

/* Advance the frame timer by delta CPU cycles; each frame end raises the
 * flyback (vsync) interrupt through the IOC (IRQ status A bit 3). Frame
 * period = HCR * VCR * (interlace+1) pixel clocks, pixel clock selected by
 * control bits 1:0 ({8,12,16,24} MHz), CPU clock 8 MHz. */
void vidc_tick(Vidc *vidc, uint32_t cycles_delta);

/* region callbacks for [VIDC_BASE, VIDC_BASE+VIDC_SIZE) */
uint8_t  vidc_rd8(void *ctx, uint32_t addr);
void     vidc_wr8(void *ctx, uint32_t addr, uint8_t v);
uint32_t vidc_rd32(void *ctx, uint32_t addr);
void     vidc_wr32(void *ctx, uint32_t addr, uint32_t v);

/* Rasterise the current mode into fb (fb_w x fb_h, 0xRRGGBB pixels). */
void vidc_render(Vidc *vidc, mem_bus *bus, uint32_t *fb, int fb_w, int fb_h);

#endif /* SOPHIE_VIDC_H */
