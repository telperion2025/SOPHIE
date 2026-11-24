/*
 * vidc.c - VIDC1 model (WP3). See vidc.h for the register contract.
 * Licence: Apache-2.0 (see LICENSE).
 */
#include "vidc.h"
#include "memc.h"
#include "machine.h"
#include "ioc.h"

#include <string.h>

#define VIDC_CPU_HZ 8000000u

/* horizontal display decode step per bpp mode (VIDC1) */
static const uint32_t xstep[4] = { 19, 11, 7, 5 };

void vidc_init(Vidc *vidc, Machine *mach)
{
    memset(vidc, 0, sizeof *vidc);
    vidc->mach = mach;
}

static uint32_t pal12_to_rgb(uint32_t v)
{
    uint32_t b = (v >> 8) & 0xF, g = (v >> 4) & 0xF, r = v & 0xF;
    return (r * 0x11u << 16) | (g * 0x11u << 8) | (b * 0x11u);
}

static uint32_t convert_hdisplay(Vidc *vidc, int index)
{
    return vidc->hraw[index] * 2 + xstep[vidc->bpp & 3];
}

static void crtc_w(Vidc *vidc, int idx, uint32_t d)
{
    switch (idx) {
      case VIDC_CRTC_HCR:  vidc->crtc[idx] = ((d >> 14) << 1) + 2;   break;
      case VIDC_CRTC_HSWR: vidc->crtc[idx] = (d >> 14) + 1;         break;
      case VIDC_CRTC_HBSR: vidc->crtc[idx] = ((d >> 14) << 1) + 1;  break;
      case VIDC_CRTC_HDSR: vidc->hraw[0] = d >> 14;
                           vidc->crtc[idx] = convert_hdisplay(vidc, 0); break;
      case VIDC_CRTC_HDER: vidc->hraw[1] = d >> 14;
                           vidc->crtc[idx] = convert_hdisplay(vidc, 1); break;
      case VIDC_CRTC_HBER: vidc->crtc[idx] = ((d >> 14) << 1) + 1;  break;
      case VIDC_CRTC_HCSR: vidc->crtc[idx] = ((d >> 13) & 0x7FFu) + 6; break;
      case VIDC_CRTC_HIR:  vidc->crtc[idx] = d;                     break;
      case VIDC_CRTC_VCR:  vidc->crtc[idx] = (d >> 14) + 1;         break;
      case VIDC_CRTC_VSWR: vidc->crtc[idx] = (d >> 14) + 1;         break;
      case VIDC_CRTC_VBSR: vidc->crtc[idx] = (d >> 14) + 1;         break;
      case VIDC_CRTC_VDSR: vidc->crtc[idx] = (d >> 14) + 1;         break;
      case VIDC_CRTC_VDER: vidc->crtc[idx] = (d >> 14) + 1;         break;
      case VIDC_CRTC_VBER: vidc->crtc[idx] = (d >> 14) + 1;         break;
      case VIDC_CRTC_VCSR: vidc->crtc[idx] = ((d >> 14) & 0x3FFu) + 1; break;
      case VIDC_CRTC_VCER: vidc->crtc[idx] = ((d >> 14) & 0x3FFu) + 1; break;
      default: break;
    }
}

static void control_w(Vidc *vidc, uint32_t d)
{
    vidc->pixel_clock = d & 3;
    vidc->bpp = (d >> 2) & 3;
    vidc->interlace = (d >> 6) & 1;
    /* HDSR/HDER depend on the bpp mode: re-decode from the raw values */
    vidc->crtc[VIDC_CRTC_HDSR] = convert_hdisplay(vidc, 0);
    vidc->crtc[VIDC_CRTC_HDER] = convert_hdisplay(vidc, 1);
}

void vidc_write(Vidc *vidc, uint32_t v)
{
    uint32_t reg = (v >> 24) & 0xFFu;
    uint32_t val = v & 0x00FFFFFFu;
    vidc->regs[reg] = val;

    if (reg < 0x40) {
        /* display palette entry i = reg>>2 */
        uint32_t i = reg >> 2;
        vidc->pal[0x100 + i] = pal12_to_rgb(val);
        /* 8bpp expansion: pens i*0x10 .. i*0x10+0xF0 */
        for (uint32_t idx = 0; idx < 0x100; idx += 0x10) {
            uint32_t b = ((val & 0x700) >> 8) | ((idx & 0x80) >> 4);
            uint32_t g = ((val & 0x030) >> 4) | ((idx & 0x60) >> 3);
            uint32_t r = (val & 0x007) | ((idx & 0x10) >> 1);
            vidc->pal[i + idx] = (r * 0x11u << 16) | (g * 0x11u << 8) | (b * 0x11u);
        }
    } else if (reg < 0x50) {
        /* cursor palette entry (reg-0x40)>>2; entry 0 is the border */
        vidc->pal[0x110 + ((reg - 0x40) >> 2)] = pal12_to_rgb(val);
    } else if (reg >= 0x80 && reg < 0xC0) {
        crtc_w(vidc, (reg - 0x80) >> 2, val);
    } else if (reg >= 0xE0) {
        control_w(vidc, val);
    }
    /* 0x60-0x7F stereo image and 0xC0-0xC3 sound latch: capture only */
}

void vidc_tick(Vidc *vidc, uint32_t cycles_delta)
{
    static const uint32_t pixel_rate[4] = { 8000000u, 12000000u, 16000000u, 24000000u };
    uint32_t hcr  = vidc->crtc[VIDC_CRTC_HCR];
    uint32_t hbsr = vidc->crtc[VIDC_CRTC_HBSR];
    uint32_t hber = vidc->crtc[VIDC_CRTC_HBER];
    uint32_t vcr  = vidc->crtc[VIDC_CRTC_VCR];
    uint32_t vbsr = vidc->crtc[VIDC_CRTC_VBSR];
    uint32_t vber = vidc->crtc[VIDC_CRTC_VBER];
    /* sanity: a valid mode has total >= border end >= border start */
    if (hcr < 2 || vcr < 2 || hber < 2 || vber < 2)
        return; /* no valid mode programmed: no vsync */
    if (hcr < hber || hber < hbsr || vber < vbsr)
        return;
    uint64_t frame_cycles = (uint64_t)hcr * vcr * VIDC_CPU_HZ / pixel_rate[vidc->pixel_clock & 3];
    if (frame_cycles == 0)
        return;
    vidc->frame_acc += cycles_delta;
    while (vidc->frame_acc >= frame_cycles) {
        vidc->frame_acc -= frame_cycles;
        if (vidc->mach != NULL)
            ioc_flyback(&vidc->mach->ioc); /* vsync: IRQ status A bit 3 */
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
    /* ARM STRB replicates the byte on all four data lanes */
    vidc_write(ctx, (uint32_t)v * 0x01010101u);
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
    const uint32_t *c = vidc->crtc;
    int bpp = vidc->bpp;

    for (int i = 0; i < fb_w * fb_h; i++)
        fb[i] = vidc->pal[0x110]; /* border */

    int xstart = (int)(c[VIDC_CRTC_HDSR] - c[VIDC_CRTC_HBSR]);
    int ystart = (int)(c[VIDC_CRTC_VDSR] - c[VIDC_CRTC_VBSR]);
    int xsize = (int)(c[VIDC_CRTC_HDER] - c[VIDC_CRTC_HDSR]);
    int ysize = (int)(c[VIDC_CRTC_VDER] - c[VIDC_CRTC_VDSR]);
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
        for (int srcx = 0; srcx < chars_per_line; srcx++) {
            uint32_t pen = mem_rd8(bus, video_base + (uint32_t)(srcx + srcy * chars_per_line));
            int dstx = srcx * xchar_size + xstart;
            for (int xi = 0; xi < xchar_size; xi++) {
                if (dstx + xi < 0 || dstx + xi >= fb_w) continue;
                uint32_t dot = (pen >> (xi * pen_byte_size[bpp])) & pen_mask[bpp];
                uint32_t idx = (bpp == 3) ? dot : 0x100u + dot;
                for (int line = 0; line <= vidc->interlace; line++) {
                    int row = dsty + line;
                    if (row < 0 || row >= fb_h) continue;
                    fb[row * fb_w + dstx + xi] = vidc->pal[idx];
                }
            }
        }
    }
}
