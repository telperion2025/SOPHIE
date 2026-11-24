/*
 * ioc.h - IOC input/output controller model (WP3, Archimedes base machine).
 *
 * Register region: 0x03200000-0x033FFFFF. The internal register file is
 * byte-wide and mirrored through the region (register = (addr-0x03200000)
 * & 0x7F). Offsets >= 0x10000 are the 1MHz peripheral bus:
 *   0x050000-0x05FFFF  peripheral 5 (0x03250000): latch B at +0x18 (FDC
 *                      DDEN/reset), latch A at +0x40 (floppy drive select/
 *                      side/motor), monitor type at +0x70 reads 0x0F; other
 *                      offsets read 0xFF (open bus) and ignore writes
 *   0x110000-0x11FFFF  WD1772 floppy controller block (0x03310000), routed
 *                      to the machine's Fdc (see fdc.h)
 *   0x1B0000-0x1B00FF  floppy drive status/control (0x033B0000): +0x04 status
 *                      (bits 5-6 media density, 0x60 = double density; bit 3
 *                      write-protect; bit 4 no-disc/error; 0xFF when no disc
 *                      is present), +0x08/+0x0C control latches (read back)
 *   other peripheral blocks: stub (reads 0xFF, writes ignored)
 * IOC registers sit on data-bus lanes D16-D23: a word store latches
 * (v>>16)&0xFF, a byte store (replicated on all lanes) latches the byte.
 * Word reads are composed little-endian from byte reads (simplification,
 * see reference manual).
 *
 * Register byte offsets:
 *   0x00 CONTROL (GPIO C0-C5, IR/IF lines)
 *   0x04 KART keyboard data (read clears Rx-full)
 *   0x10 IRQ_STATUS_A   0x14 IRQ_REQUEST_A (write-1-clear, bits 0x7C)
 *   0x18 IRQ_MASK_A     0x20 IRQ_STATUS_B   0x24 IRQ_REQUEST_B
 *   0x28 IRQ_MASK_B     0x30 FIQ_STATUS     0x34 FIQ_REQUEST
 *   0x38 FIQ_MASK
 *   0x40.. T0 latch lo/hi, GO, LATCH; 0x50.. T1; 0x60.. T2; 0x70.. T3
 *
 * IRQ A bits: 7 force, 6 timer1, 5 timer0, 4 POR, 3 IR (VIDC flyback/vsync),
 * 2 IF, 1 IL7, 0 IL6. IRQ request reads return status & mask.
 * IRQ B bits: 7 KART Rx full, 6 KART Tx empty, 0-5 IL0-IL5.
 * FIQ bits: 7 force, 1 FH1 (floppy INTRQ), 0 FH0 (floppy DRQ); the FIQ
 * request register reads status & mask and is write-1-clear on status.
 * Timers 0/1 run at 2 MHz and raise IRQ A bits 5/6 on underflow, reloading
 * from their latches (continuous). T2/T3 are baud/KART clocks (no IRQ).
 *
 * Licence: Apache-2.0 (see LICENSE). Register layout per Acorn IOC
 * datasheet; behaviour reference only from studied emulators.
 */
#ifndef SOPHIE_IOC_H
#define SOPHIE_IOC_H

#include <stdint.h>

#define IOC_BASE 0x03200000u
#define IOC_SIZE 0x00200000u

#define IOC_CONTROL       0x00u
#define IOC_KART          0x04u
#define IOC_IRQ_STATUS_A  0x10u
#define IOC_IRQ_REQUEST_A 0x14u
#define IOC_IRQ_MASK_A    0x18u
#define IOC_IRQ_STATUS_B  0x20u
#define IOC_IRQ_REQUEST_B 0x24u
#define IOC_IRQ_MASK_B    0x28u
#define IOC_FIQ_STATUS    0x30u
#define IOC_FIQ_REQUEST   0x34u
#define IOC_FIQ_MASK      0x38u
#define IOC_T0_LO 0x40u
#define IOC_T0_HI 0x44u
#define IOC_T0_GO 0x48u
#define IOC_T0_LATCH 0x4Cu
#define IOC_T1_LO 0x50u
#define IOC_T2_LO 0x60u
#define IOC_T3_LO 0x70u

#define IOC_IRA_FLYBACK  0x08u /* VIDC vsync (IR line) */
#define IOC_IRA_TIMER0   0x20u
#define IOC_IRA_TIMER1   0x40u
#define IOC_IRB_KART_RX_FULL  0x80u
#define IOC_IRB_KART_TX_EMPTY 0x40u

#define IOC_TIMER_HZ 2000000u /* 2 MHz timers */

typedef struct Machine Machine;
struct Arm26;

typedef struct Ioc {
    uint8_t  regs[0x80];
    uint32_t timer_cnt[4];
    uint32_t timer_latch[4];
    uint64_t tick_acc;      /* fractional 2MHz accumulator (CPU cycles/4) */
    uint8_t  key_queue[16];
    int      key_head, key_tail;
    uint32_t kart_tx_cnt;  /* KART transmit shift countdown; 0 = Tx empty */
    uint8_t  flatch[4];    /* 0x033B0000 block control latches (+0x04/+0x08/+0x0C) */
    Machine *mach;
} Ioc;

void ioc_init(Ioc *ioc, Machine *mach);

/* Advance the timers by delta CPU cycles and update the CPU IRQ/FIQ lines. */
void ioc_tick(Ioc *ioc, struct Arm26 *cpu, uint32_t cycles_delta);

/* VIDC flyback (vsync) input: sets IRQ status A bit 3 (IR line). */
void ioc_flyback(Ioc *ioc);

/* Drive an external FIQ source line (e.g. FDC INTRQ/DRQ) into FIQ status. */
void ioc_fiq_line(Ioc *ioc, uint8_t mask, int state);

/* Queue a keyboard byte (KART Rx). ioc_key_press sends a make code,
 * ioc_key_release the matching break code (byte | 0x80). */
void ioc_key_press(Ioc *ioc, uint8_t byte);
void ioc_key_release(Ioc *ioc, uint8_t byte);

/* region callbacks for [IOC_BASE, IOC_BASE+IOC_SIZE) */
uint8_t  ioc_rd8(void *ctx, uint32_t addr);
void     ioc_wr8(void *ctx, uint32_t addr, uint8_t v);
uint32_t ioc_rd32(void *ctx, uint32_t addr);
void     ioc_wr32(void *ctx, uint32_t addr, uint32_t v);

#endif /* SOPHIE_IOC_H */
