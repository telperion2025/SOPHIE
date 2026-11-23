/*
 * vidc.c - VIDC1 model (WP3). See vidc.h for the register contract.
 * Licence: Apache-2.0 (see LICENSE).
 */
#include "vidc.h"
#include "memc.h"
#include "machine.h"

#include <string.h>

void vidc_init(Vidc *vidc, Machine *mach)
{
    memset(vidc, 0, sizeof *vidc);
    vidc->mach = mach;
    vidc->border = 0;
}

static uint32_t pal12_to_rgb(uint32_t v)
{
    uint32_t b = (v >> 8) & 0xF, g = (v >> 4) & 0xF, r = v & 0xF;
    return (r * 0x11u << 16) | (g * 0x11u << 8) | (b * 0x11u);
}

void vidc_write(Vidc *vidc, uint32_t v)
{
    uint32_t reg = (v >> 24) & 0xFFu;
    uint32_t val = v & 0x00FFFFFFu;
    if (reg >= 64)
        return;
    vidc->regs[reg] = val;

    if (reg <= 0x0F) {
        /* display palette: expand into 16 8bpp entries (VIDC1 4bpp mapping) */
        uint32_t rgb = pal12_to_rgb(val);
        vidc->pal4[reg] = rgb;
        for (uint32_t idx = 0; idx < 0x100; idx += 0x10) {
            uint32_t b = ((val & 0x700) >> 8) | ((idx & 0x80) >> 4);
            uint32_t g = ((val & 0x030) >> 4) | ((idx & 0x60) >> 3);
            uint32_t r = ((val & 0x007)) | ((idx & 0x10) >> 1);
            vidc->pal8[reg + idx] = (r * 0x11u << 16) | (g * 0x11u << 8) | (b * 0x11u);
        }
    } else if (reg == VIDC_REG_BORDER) {
        vidc->border = pal12_to_rgb(val);
    } else if (reg == VIDC_REG_CONTROL) {
        vidc->pixel_clock = val & 3;
        vidc->bpp = (val >> 2) & 3;
        vidc->interlace = (val >> 6) & 1;
    }
}

uint8_t vidc_rd8(void *ctx, uint32_t addr)
{
    (void)ctx; (void)addr;
    return 0xFF; /* VIDC registers are write-only */
}

void vidc_wr8(void *ctx, uint32_t addr, uint8_t v)
{
    (void)addr;
    /* STRB replicates the byte on all four data lanes */
    vidc_write(ctx, ((uint32_t)v << 24) | 0x00FFFFFFu);
}

uint32_t vidc_rd32(void *ctx, uint32_t addr)
{
    (void)ctx; (void)addr;
    return 0xFFFFFFFFu;
}

void vidc_wr32(void *ctx, uint32_t addr, uint32_t v)
{
    (void)addr;
    vidc_write(ctx, v);
}

void vidc_render(Vidc *vidc, mem_bus *bus, uint32_t *fb, int fb_w, int fb_h)
{
    const uint32_t *c = vidc->regs + VIDC_REG_CRTC;
    const uint32_t xstep[4] = { 19, 11, 7, 5 };
    int bpp = vidc->bpp;

    /* decode CRTC registers (VIDC1 arithmetic) */
    uint32_t hcr  = ((c[VIDC_CRTC_HCR] >> 14) << 1) + 2;
    uint32_t hbsr = ((c[VIDC_CRTC_HBSR] >> 14) << 1) + 1;
    uint32_t hdsr = ((c[VIDC_CRTC_HDSR] >> 14) * 2) + xstep[bpp];
    uint32_t hder = ((c[VIDC_CRTC_HDER] >> 14) * 2) + xstep[bpp];
    uint32_t hber = ((c[VIDC_CRTC_HBER] >> 14) << 1) + 1;
    uint32_t vcr  = (c[VIDC_CRTC_VCR] >> 14) + 1;
    uint32_t vbsr = (c[VIDC_CRTC_VBSR] >> 14) + 1;
    uint32_t vdsr = (c[VIDC_CRTC_VDSR] >> 14) + 1;
    uint32_t vder = (c[VIDC_CRTC_VDER] >> 14) + 1;
    (void)hcr; (void)vcr; (void)hber;

    for (int i = 0; i < fb_w * fb_h; i++)
        fb[i] = vidc->border;

    int xstart = (int)(hdsr - hbsr);
    int ystart = (int)(vdsr - vbsr);
    int xsize = (int)(hder - hdsr);
    int ysize = (int)(vder - vdsr);
    if (xsize <= 0 || ysize <= 0)
        return;

    int chars_per_line = xsize >> (3 - bpp);
    int xchar_size = 1 << (3 - bpp);
    const uint32_t pen_mask[4] = { 1, 3, 0xF, 0xFF };
    const uint32_t pen_byte_size[4] = { 1, 2, 4, 1 };
    /* video DMA fetches from the physical DRAM window at vidstart */
    uint32_t video_base = MEMC_PHYS_BASE + vidc->mach->memc.vidstart;

    for (int srcy = 0; srcy < ysize; srcy++) {
        int dsty = (srcy + ystart) << vidc->interlace;
        if (dsty < 0 || dsty >= fb_h) continue;
        for (int srcx = 0; srcx < chars_per_line; srcx++) {
            uint32_t pen = mem_rd8(bus, video_base + (uint32_t)(srcx + srcy * chars_per_line));
            int dstx = srcx * xchar_size + xstart;
            for (int xi = 0; xi < xchar_size; xi++) {
                if (dstx + xi < 0 || dstx + xi >= fb_w) continue;
                uint32_t dot = (pen >> (xi * pen_byte_size[bpp])) & pen_mask[bpp];
                uint32_t rgb = (bpp == 3) ? vidc->pal8[dot] : vidc->pal4[dot & 0xF];
                fb[dsty * fb_w + dstx + xi] = rgb;
            }
        }
    }
}
