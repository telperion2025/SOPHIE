/*
 * fdc.c - WD1772 model (WP7). See fdc.h for the register contract.
 *
 * Timing is in 8 MHz CPU cycles, coarse but ordered like the hardware:
 * spin-up ~15 ms, one step ~3 ms, a sector's first byte after ~10 ms,
 * then one byte every ~6 us (48 cycles). Events are scheduled on an
 * absolute cycle timeline serviced by fdc_tick().
 *
 * Licence: Apache-2.0 (see LICENSE).
 */
#include "fdc.h"
#include "ioc.h"
#include "machine.h"

#include <string.h>

#define T_SPINUP   120000u /* motor spin-up */
#define T_STEP      24000u /* one head step */
#define T_SECTOR    80000u /* seek-settle + first byte of a sector */
#define T_BYTE         48u /* one data byte at double density */
#define T_ADDR_BYTE    64u

static void fdc_set_intrq(Fdc *fdc, int state)
{
    fdc->intrq = state;
    if (fdc->mach != NULL)
        ioc_fiq_line(&fdc->mach->ioc, FDC_FIQ_IRQ, state);
}

static void fdc_set_drq(Fdc *fdc, int state)
{
    fdc->drq = state;
    if (state)
        fdc->status |= FDC_S_DRQ;
    else
        fdc->status &= (uint8_t)~FDC_S_DRQ;
    if (fdc->mach != NULL)
        ioc_fiq_line(&fdc->mach->ioc, FDC_FIQ_DRQ, state);
}

static void fdc_sched(Fdc *fdc, int kind, uint32_t delay)
{
    fdc->ev_kind = kind;
    fdc->ev_time = fdc->now + delay;
}

static int fdc_mon(const Fdc *fdc)
{
    return fdc->motor ? 0 : FDC_S_MON;
}

static int fdc_tr00(const Fdc *fdc)
{
    return fdc->curtrack == 0 ? FDC_S_TR00 : 0;
}

static Disc *fdc_disc(Fdc *fdc)
{
    if (fdc->drive < 0 || fdc->drive >= DISC_MAX_DRIVES)
        return NULL;
    Disc *d = &fdc->mach->disc[fdc->drive];
    return d->present ? d : NULL;
}

/* Finish the current command: type-II/III status, INTRQ. */
static void fdc_complete(Fdc *fdc, uint8_t extra)
{
    fdc->xfer_mode = FDC_XFER_NONE;
    fdc->ev_kind = FDC_EV_NONE;
    fdc->status = (uint8_t)(fdc_mon(fdc) | fdc_tr00(fdc) | extra);
    fdc_set_drq(fdc, 0);
    fdc_set_intrq(fdc, 1);
}

/* Load the current sector (or track) into the transfer buffer.
 * Returns 0 ok, -1 not found. */
static int fdc_load(Fdc *fdc, int whole_track)
{
    Disc *d = fdc_disc(fdc);
    if (d == NULL || fdc->density != 1 || fdc->curtrack != fdc->track)
        return -1;
    if (whole_track)
        return disc_read_track(d, fdc->track, fdc->side, fdc->buf) == 0
               ? (fdc->xfer_len = d->spt * d->sectsize, 0) : -1;
    if (disc_read_sector(d, fdc->track, fdc->side, fdc->sector, fdc->buf) != 0)
        return -1;
    fdc->xfer_len = d->sectsize;
    return 0;
}

static void fdc_read_cmd(Fdc *fdc, int whole_track)
{
    if (fdc_load(fdc, whole_track) != 0) {
        fdc->xfer_mode = FDC_XFER_NONE;
        fdc->status = (uint8_t)(FDC_S_BUSY | fdc_mon(fdc));
        fdc_sched(fdc, FDC_EV_DONE, T_SECTOR);
        fdc->xfer_len = -1; /* marker: complete with RNF */
        return;
    }
    fdc->xfer_pos = 0;
    fdc->status = (uint8_t)(FDC_S_BUSY | fdc_mon(fdc));
    fdc_sched(fdc, FDC_EV_BYTE, T_SECTOR);
}

static void fdc_write_cmd(Fdc *fdc, int whole_track)
{
    Disc *d = fdc_disc(fdc);
    if (d == NULL || fdc->density != 1 || fdc->curtrack != fdc->track || d->wprot) {
        fdc->xfer_mode = FDC_XFER_NONE;
        fdc->status = (uint8_t)(FDC_S_BUSY | fdc_mon(fdc));
        fdc_sched(fdc, FDC_EV_DONE, T_SECTOR);
        fdc->xfer_len = -2; /* marker: complete with WP/RNF */
        return;
    }
    fdc->xfer_mode = FDC_XFER_WRITE;
    fdc->xfer_len = whole_track ? d->spt * d->sectsize : d->sectsize;
    fdc->xfer_pos = 0;
    fdc->status = (uint8_t)(FDC_S_BUSY | fdc_mon(fdc));
    fdc_set_drq(fdc, 1); /* request first byte */
}

static void fdc_cmd(Fdc *fdc, uint8_t cmd)
{
    if ((fdc->status & FDC_S_BUSY) && (cmd >> 4) != 0xD)
        return; /* only force-interrupt is accepted while busy */
    fdc_set_intrq(fdc, 0);
    fdc->command = cmd;
    fdc->xfer_mode = FDC_XFER_NONE;
    fdc->ev_kind = FDC_EV_NONE;

    switch (cmd >> 4) {
      case 0x0: /* restore */
        fdc->status = (uint8_t)(FDC_S_BUSY | FDC_S_SPIN | fdc_mon(fdc) | fdc_tr00(fdc));
        fdc_sched(fdc, FDC_EV_DONE, T_SPINUP + (uint32_t)fdc->curtrack * T_STEP);
        fdc->xfer_len = 0; /* type I: restore */
        break;
      case 0x1: /* seek to data register */
        fdc->status = (uint8_t)(FDC_S_BUSY | FDC_S_SPIN | fdc_mon(fdc) | fdc_tr00(fdc));
        fdc_sched(fdc, FDC_EV_DONE,
                  T_SPINUP + (uint32_t)(fdc->data > fdc->curtrack ? fdc->data - fdc->curtrack
                                                                   : fdc->curtrack - fdc->data) * T_STEP);
        fdc->xfer_len = 1; /* type I: seek */
        break;
      case 0x2: case 0x3: /* step (direction from last step) */
        fdc->status = (uint8_t)(FDC_S_BUSY | FDC_S_SPIN | fdc_mon(fdc) | fdc_tr00(fdc));
        fdc_sched(fdc, FDC_EV_DONE, T_SPINUP + T_STEP);
        fdc->xfer_len = 2;
        break;
      case 0x4: case 0x5: /* step in */
        fdc->stepdir = 1;
        fdc->status = (uint8_t)(FDC_S_BUSY | FDC_S_SPIN | fdc_mon(fdc) | fdc_tr00(fdc));
        fdc_sched(fdc, FDC_EV_DONE, T_SPINUP + T_STEP);
        fdc->xfer_len = 3;
        break;
      case 0x6: case 0x7: /* step out */
        fdc->stepdir = -1;
        fdc->status = (uint8_t)(FDC_S_BUSY | FDC_S_SPIN | fdc_mon(fdc) | fdc_tr00(fdc));
        fdc_sched(fdc, FDC_EV_DONE, T_SPINUP + T_STEP);
        fdc->xfer_len = 4;
        break;
      case 0x8: case 0x9: /* read sector (bit 4 = multiple) */
        fdc->multi = (cmd & 0x10) != 0;
        fdc->xfer_mode = FDC_XFER_READ;
        fdc_read_cmd(fdc, 0);
        break;
      case 0xA: case 0xB: /* write sector (bit 4 = multiple) */
        fdc->multi = (cmd & 0x10) != 0;
        fdc_write_cmd(fdc, 0);
        break;
      case 0xC: /* read address: 6 ID bytes */
        fdc->xfer_mode = FDC_XFER_READ;
        {
            Disc *d = fdc_disc(fdc);
            if (d == NULL || disc_read_address(d, fdc->track, fdc->side, fdc->buf) != 0) {
                fdc->xfer_mode = FDC_XFER_NONE;
                fdc->status = (uint8_t)(FDC_S_BUSY | fdc_mon(fdc));
                fdc_sched(fdc, FDC_EV_DONE, T_SECTOR);
                fdc->xfer_len = -1;
                break;
            }
            fdc->xfer_len = 6;
            fdc->xfer_pos = 0;
            fdc->status = (uint8_t)(FDC_S_BUSY | fdc_mon(fdc));
            fdc_sched(fdc, FDC_EV_BYTE, T_SECTOR);
        }
        break;
      case 0xD: /* force interrupt */
        fdc->xfer_mode = FDC_XFER_NONE;
        fdc->ev_kind = FDC_EV_NONE;
        fdc_set_drq(fdc, 0);
        fdc->status = (uint8_t)(fdc_mon(fdc) | fdc_tr00(fdc));
        if (cmd & 0x08)
            fdc_set_intrq(fdc, 1);
        break;
      case 0xE: /* read track */
        fdc->multi = 0;
        fdc->xfer_mode = FDC_XFER_READ;
        fdc_read_cmd(fdc, 1);
        break;
      case 0xF: /* write track (format): stream and discard (see fdc.h) */
        fdc->multi = 0;
        fdc_write_cmd(fdc, 1);
        break;
    }
}

/* Type I completion. */
static void fdc_type1_done(Fdc *fdc)
{
    switch (fdc->xfer_len) {
      case 0: fdc->curtrack = fdc->track = 0; break;
      case 1: fdc->curtrack = fdc->track = fdc->data; break;
      case 2: fdc->curtrack += fdc->stepdir; break;
      case 3: fdc->curtrack++; break;
      case 4: fdc->curtrack--; break;
    }
    if (fdc->curtrack < 0)
        fdc->curtrack = 0;
    Disc *d = fdc_disc(fdc);
    if (d != NULL && fdc->curtrack >= d->tracks)
        fdc->curtrack = d->tracks - 1;
    if (fdc->xfer_len >= 2 && (fdc->command & 0x04)) /* U flag: update track reg */
        fdc->track = (uint8_t)fdc->curtrack;
    fdc_complete(fdc, 0);
}

static void fdc_ev_byte(Fdc *fdc)
{
    if (fdc->xfer_pos >= fdc->xfer_len) {
        fdc_sched(fdc, FDC_EV_DONE, T_BYTE);
        return;
    }
    fdc->data = fdc->buf[fdc->xfer_pos++];
    fdc_set_drq(fdc, 1);
    if (fdc->xfer_pos >= fdc->xfer_len)
        fdc_sched(fdc, FDC_EV_DONE, T_BYTE);
    else
        fdc_sched(fdc, FDC_EV_BYTE, fdc->xfer_len == 6 ? T_ADDR_BYTE : T_BYTE);
}

static void fdc_ev_done(Fdc *fdc)
{
    if (fdc->xfer_mode == FDC_XFER_READ && (fdc->status & FDC_S_DRQ)) {
        /* last byte not yet collected: hold off completion (hardware holds
         * BUSY/INTRQ until the final DRQ is serviced) */
        fdc_sched(fdc, FDC_EV_DONE, T_BYTE);
        return;
    }
    if (fdc->xfer_len == -1) { /* not found */
        fdc_complete(fdc, FDC_S_RNF);
        return;
    }
    if (fdc->xfer_len == -2) { /* write protect / no disc */
        Disc *d = fdc_disc(fdc);
        fdc_complete(fdc, (d != NULL && d->wprot) ? FDC_S_WP : FDC_S_RNF);
        return;
    }
    if (fdc->xfer_mode == FDC_XFER_READ && fdc->xfer_len != 6) {
        /* read sector: multiple flag chains the next sector */
        if (fdc->multi) {
            Disc *d = fdc_disc(fdc);
            if (d != NULL && fdc->sector + 1 < d->spt + d->sectbase) {
                fdc->sector++;
                if (disc_read_sector(d, fdc->track, fdc->side, fdc->sector, fdc->buf) == 0) {
                    fdc->xfer_len = d->sectsize;
                    fdc->xfer_pos = 0;
                    fdc->status = (uint8_t)(FDC_S_BUSY | fdc_mon(fdc));
                    fdc_sched(fdc, FDC_EV_BYTE, T_SECTOR);
                    return;
                }
            }
        }
        fdc_complete(fdc, 0);
        return;
    }
    if (fdc->xfer_mode == FDC_XFER_READ && fdc->xfer_len == 6) {
        fdc->sector = (uint8_t)fdc->curtrack; /* read address leaves track in sector reg */
        fdc_complete(fdc, 0);
        return;
    }
    if (fdc->xfer_mode == FDC_XFER_WRITE) {
        /* write sector/track finished (write track data is discarded) */
        if (fdc->xfer_len != 6 && fdc->multi) {
            Disc *d = fdc_disc(fdc);
            if (d != NULL && fdc->sector + 1 < d->spt + d->sectbase) {
                if (disc_write_sector(d, fdc->track, fdc->side, fdc->sector, fdc->buf) == 0) {
                    fdc->sector++;
                    fdc->xfer_pos = 0;
                    fdc->status = (uint8_t)(FDC_S_BUSY | fdc_mon(fdc));
                    fdc_set_drq(fdc, 1);
                    return;
                }
            }
        } else if (fdc->xfer_len > 0) {
            disc_write_sector(fdc_disc(fdc), fdc->track, fdc->side, fdc->sector, fdc->buf);
        }
        fdc_complete(fdc, 0);
        return;
    }
    /* type I */
    if (fdc->xfer_mode == FDC_XFER_NONE && fdc->xfer_len >= 0 && fdc->xfer_len <= 4) {
        fdc_type1_done(fdc);
        return;
    }
    fdc_complete(fdc, 0);
}

void fdc_tick(Fdc *fdc, uint32_t cycles_delta)
{
    fdc->now += cycles_delta;
    while (fdc->ev_kind != FDC_EV_NONE && fdc->ev_time <= fdc->now) {
        int kind = fdc->ev_kind;
        fdc->ev_kind = FDC_EV_NONE;
        if (kind == FDC_EV_BYTE)
            fdc_ev_byte(fdc);
        else
            fdc_ev_done(fdc);
    }
}

void fdc_mr(Fdc *fdc, int state)
{
    if (!state) { /* master reset (active low) */
        fdc->command = 0x03;
        fdc->status = 0;
        fdc->sector = 1;
        fdc->xfer_mode = FDC_XFER_NONE;
        fdc->ev_kind = FDC_EV_NONE;
        fdc_set_intrq(fdc, 0);
        fdc_set_drq(fdc, 0);
    }
    fdc->mr = state;
}

void fdc_latch_a(Fdc *fdc, uint8_t v)
{
    fdc->drive = -1;
    if (!(v & 0x01)) fdc->drive = 0;
    if (!(v & 0x02)) fdc->drive = 1;
    if (!(v & 0x04)) fdc->drive = 2;
    if (!(v & 0x08)) fdc->drive = 3;
    fdc->side = (v & 0x10) ? 0 : 1;
    fdc->motor = !(v & 0x20);
}

void fdc_latch_b(Fdc *fdc, uint8_t v)
{
    fdc->density = (v & 0x02) ? 0 : 1; /* DDEN low = MFM double density */
    fdc_mr(fdc, (v & 0x08) ? 1 : 0);
}

uint8_t fdc_reg_rd(Fdc *fdc, uint32_t addr)
{
    switch ((addr & 0xC) >> 2) {
      case FDC_REG_STATUS:
        fdc_set_intrq(fdc, 0);
        return fdc->status;
      case FDC_REG_TRACK:
        return fdc->track;
      case FDC_REG_SECTOR:
        return fdc->sector;
      case FDC_REG_DATA:
        fdc_set_drq(fdc, 0);
        if (fdc->xfer_mode == FDC_XFER_READ && fdc->xfer_pos >= fdc->xfer_len &&
            fdc->ev_kind == FDC_EV_DONE) {
            fdc->ev_kind = FDC_EV_NONE;
            fdc_ev_done(fdc); /* last byte collected: complete now */
        }
        return fdc->data;
    }
    return 0xFF;
}

void fdc_reg_wr(Fdc *fdc, uint32_t addr, uint8_t v)
{
    switch ((addr & 0xC) >> 2) {
      case FDC_REG_STATUS:
        if (fdc->mr)
            fdc_cmd(fdc, v);
        break;
      case FDC_REG_TRACK:
        fdc->track = v;
        break;
      case FDC_REG_SECTOR:
        fdc->sector = v;
        break;
      case FDC_REG_DATA:
        fdc_set_drq(fdc, 0);
        fdc->data = v;
        if (fdc->xfer_mode == FDC_XFER_WRITE) {
            if (!(fdc->status & FDC_S_BUSY)) {
                fdc->xfer_mode = FDC_XFER_NONE;
                break;
            }
            if (fdc->xfer_pos >= fdc->xfer_len) {
                /* overrun: lost data */
                fdc->xfer_mode = FDC_XFER_NONE;
                fdc->ev_kind = FDC_EV_NONE;
                fdc->status = (uint8_t)(fdc_mon(fdc) | fdc_tr00(fdc) | FDC_S_LOST);
                fdc_set_drq(fdc, 0);
                fdc_set_intrq(fdc, 1);
                break;
            }
            fdc->buf[fdc->xfer_pos++] = v;
            if (fdc->xfer_pos >= fdc->xfer_len) {
                if (fdc->multi) {
                    Disc *d = fdc_disc(fdc);
                    if (d != NULL && fdc->sector + 1 < d->spt + d->sectbase) {
                        disc_write_sector(d, fdc->track, fdc->side, fdc->sector, fdc->buf);
                        fdc->sector++;
                        fdc->xfer_pos = 0;
                        fdc_set_drq(fdc, 1); /* request next sector's first byte */
                        break;
                    }
                }
                if (fdc->xfer_len > 0)
                    disc_write_sector(fdc_disc(fdc), fdc->track, fdc->side, fdc->sector, fdc->buf);
                fdc_complete(fdc, 0);
            } else {
                fdc_set_drq(fdc, 1); /* request next byte */
            }
        }
        break;
    }
}

void fdc_init(Fdc *fdc, Machine *mach)
{
    memset(fdc, 0, sizeof *fdc);
    fdc->mach = mach;
    fdc->drive = 0;
    fdc->side = 0;
    fdc->density = 1;
    fdc->mr = 1;
    fdc->stepdir = 1;
    fdc->status = FDC_S_MON; /* motor off at power-on */
}

uint8_t fdc_rd8(void *ctx, uint32_t addr)
{
    return fdc_reg_rd(ctx, addr);
}

void fdc_wr8(void *ctx, uint32_t addr, uint8_t v)
{
    fdc_reg_wr(ctx, addr, v);
}

uint32_t fdc_rd32(void *ctx, uint32_t addr)
{
    uint32_t off = addr & ~3u;
    return (uint32_t)fdc_reg_rd(ctx, off)
         | ((uint32_t)fdc_reg_rd(ctx, off + 1) << 8)
         | ((uint32_t)fdc_reg_rd(ctx, off + 2) << 16)
         | ((uint32_t)fdc_reg_rd(ctx, off + 3) << 24);
}

void fdc_wr32(void *ctx, uint32_t addr, uint32_t v)
{
    /* WD1772 latches D16-D23 on word stores, like the IOC */
    fdc_reg_wr(ctx, addr & ~3u, (uint8_t)((v >> 16) & 0xFF));
}
