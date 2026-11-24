/*
 * fdc.h - WD1772 floppy disk controller model (WP7, Archimedes A3xx).
 *
 * The WD1772 sits in the IOC address space at 0x03310000-0x0331FFFF
 * (IOC "bank 1"); the register is selected by address bits 3:2:
 *   0x00 status (R) / command (W)   0x04 track   0x08 sector   0x0C data
 * Like the IOC, the chip hangs on data-bus lanes D16-D23: a word store
 * latches (v>>16)&0xFF, a byte store (STRB, replicated on all lanes)
 * latches the byte. Word reads compose four byte lanes (see ioc_rd32).
 *
 * Status bits: 0x01 BUSY, 0x02 DRQ, 0x04 TR00, 0x08 CRC error, 0x10 RNF,
 * 0x20 spin-up (WD1772), 0x40 write protect, 0x80 motor off (MON).
 * INTRQ drives IOC FIQ status bit 0x02 (FH1), DRQ drives FIQ bit 0x01
 * (FH0). Reading status clears INTRQ; writing command clears INTRQ;
 * reading/writing data clears DRQ and the status DRQ bit.
 *
 * Drive select / side / motor come from IOC peripheral-5 latch A
 * (0x03250040: bits 0-3 drive select active-low, bit 4 side, bit 5 motor
 * active-low); density (DDEN) and master reset from latch B (0x03250018:
 * bit 1 DDEN (0 = MFM double density), bit 3 FDC reset active-low).
 *
 * Commands are modelled at sector granularity against a Disc image
 * (disc.h): type I (restore/seek/step in/out), read sector (single and
 * multiple), write sector, read address, read track, force interrupt.
 * Write track (format) streams and discards the data - formatting a
 * data-only image is not supported (see reference manual section 7).
 *
 * Licence: Apache-2.0 (see LICENSE). Register layout per the WD1772
 * datasheet; behaviour reference only from studied emulators.
 */
#ifndef SOPHIE_FDC_H
#define SOPHIE_FDC_H

#include <stdint.h>

#include "disc.h"

#define FDC_BASE 0x03310000u /* block [FDC_BASE, FDC_BASE+0x10000) inside the IOC region */

#define FDC_REG_STATUS 0u
#define FDC_REG_TRACK  1u
#define FDC_REG_SECTOR 2u
#define FDC_REG_DATA   3u

#define FDC_S_BUSY 0x01u
#define FDC_S_DRQ  0x02u
#define FDC_S_TR00 0x04u
#define FDC_S_LOST 0x04u /* type II: lost data shares the bit */
#define FDC_S_CRC  0x08u
#define FDC_S_RNF  0x10u
#define FDC_S_SPIN 0x20u /* WD1772: spin-up in progress */
#define FDC_S_WP   0x40u
#define FDC_S_MON  0x80u /* motor off */

#define FDC_FIQ_DRQ 0x01u /* FH0: data request  -> IOC FIQ status bit 0 */
#define FDC_FIQ_IRQ 0x02u /* FH1: command done  -> IOC FIQ status bit 1 */

typedef struct Machine Machine;

enum {
    FDC_XFER_NONE = 0,
    FDC_XFER_READ,      /* read sector / read track / read address: stream buf -> data reg */
    FDC_XFER_WRITE      /* write sector / write track: data reg -> buf -> disc */
};

enum {
    FDC_EV_NONE = 0,
    FDC_EV_BYTE,        /* deliver next data byte (read modes) */
    FDC_EV_DONE         /* command completes */
};

typedef struct Fdc {
    uint8_t command, status, track, sector, data;
    int     curtrack, stepdir;
    int     drive;      /* selected drive 0-3, -1 = none */
    int     side;       /* 0/1 */
    int     motor;      /* 1 = motor on */
    int     density;    /* 1 = double density (MFM), 0 = single (FM) */
    int     mr;         /* master reset (1 = running) */
    int     intrq, drq;
    int     xfer_mode, multi;
    uint8_t buf[DISC_MAX_TRACK_BYTES];
    int     xfer_len, xfer_pos;
    int     xfer_track, xfer_side, xfer_sector;
    uint64_t now;       /* CPU cycles seen */
    uint64_t ev_time;   /* absolute cycle of the pending event */
    int     ev_kind;    /* FDC_EV_* */
    Machine *mach;
} Fdc;

void fdc_init(Fdc *fdc, Machine *mach);

/* Advance the command engine by delta CPU cycles (call from machine_run). */
void fdc_tick(Fdc *fdc, uint32_t cycles_delta);

/* Master reset (latch B bit 3, active low). */
void fdc_mr(Fdc *fdc, int state);

/* IOC peripheral-5 latches. */
void fdc_latch_a(Fdc *fdc, uint8_t v); /* drive select / side / motor */
void fdc_latch_b(Fdc *fdc, uint8_t v); /* DDEN / reset */

/* Register accessors (addr absolute; reg = (addr & 0xC) >> 2). */
uint8_t fdc_reg_rd(Fdc *fdc, uint32_t addr);
void    fdc_reg_wr(Fdc *fdc, uint32_t addr, uint8_t v);

/* Bus-facing callbacks for the [FDC_BASE, FDC_BASE+0x10000) block. */
uint8_t  fdc_rd8(void *ctx, uint32_t addr);
void     fdc_wr8(void *ctx, uint32_t addr, uint8_t v);
uint32_t fdc_rd32(void *ctx, uint32_t addr);
void     fdc_wr32(void *ctx, uint32_t addr, uint32_t v);

#endif /* SOPHIE_FDC_H */
