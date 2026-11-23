/*
 * ioc.c - IOC model (WP3). See ioc.h for the register contract.
 * Licence: Apache-2.0 (see LICENSE).
 */
#include "ioc.h"
#include "arm26.h"
#include "fdc.h"
#include "machine.h"

#include <string.h>

void ioc_init(Ioc *ioc, Machine *mach)
{
    memset(ioc, 0, sizeof *ioc);
    ioc->mach = mach;
    ioc->regs[IOC_IRQ_STATUS_A] = 0x10 | 0x80; /* POR + force IRQ */
    ioc->regs[IOC_IRQ_STATUS_B] = IOC_IRB_KART_TX_EMPTY;
    ioc->regs[IOC_FIQ_STATUS]   = 0x80;        /* force FIQ */
}

static void ioc_update_lines(Ioc *ioc, Arm26 *cpu)
{
    int irq = ((ioc->regs[IOC_IRQ_STATUS_A] & ioc->regs[IOC_IRQ_MASK_A]) ||
               (ioc->regs[IOC_IRQ_STATUS_B] & ioc->regs[IOC_IRQ_MASK_B])) != 0;
    int fiq = (ioc->regs[IOC_FIQ_STATUS] & ioc->regs[IOC_FIQ_MASK]) != 0;
    cpu->irqPending = irq;
    cpu->fiqPending = fiq;
}

void ioc_tick(Ioc *ioc, Arm26 *cpu, uint32_t cycles_delta)
{
    /* timers run at 2 MHz from the 8 MHz CPU clock: 4 cycles per tick */
    ioc->tick_acc += cycles_delta;
    while (ioc->tick_acc >= 4) {
        ioc->tick_acc -= 4;
        if (ioc->kart_tx_cnt != 0 && --ioc->kart_tx_cnt == 0)
            ioc->regs[IOC_IRQ_STATUS_B] |= IOC_IRB_KART_TX_EMPTY;
        for (int t = 0; t < 4; t++) {
            if (ioc->timer_cnt[t] == 0)
                continue;
            if (--ioc->timer_cnt[t] == 0) {
                if (t == 0) ioc->regs[IOC_IRQ_STATUS_A] |= IOC_IRA_TIMER0;
                if (t == 1) ioc->regs[IOC_IRQ_STATUS_A] |= IOC_IRA_TIMER1;
                ioc->timer_cnt[t] = ioc->timer_latch[t]; /* continuous reload */
            }
        }
    }
    if (cpu != NULL)
        ioc_update_lines(ioc, cpu);
}

void ioc_flyback(Ioc *ioc)
{
    ioc->regs[IOC_IRQ_STATUS_A] |= IOC_IRA_FLYBACK;
    if (ioc->mach != NULL)
        ioc_update_lines(ioc, &ioc->mach->cpu);
}

void ioc_fiq_line(Ioc *ioc, uint8_t mask, int state)
{
    if (state)
        ioc->regs[IOC_FIQ_STATUS] |= mask;
    else
        ioc->regs[IOC_FIQ_STATUS] &= (uint8_t)~mask;
    if (ioc->mach != NULL)
        ioc_update_lines(ioc, &ioc->mach->cpu);
}

static void ioc_key_push(Ioc *ioc, uint8_t byte)
{
    int next = (ioc->key_tail + 1) % 16;
    if (next == ioc->key_head)
        return; /* queue full: drop */
    if (ioc->key_head == ioc->key_tail && !(ioc->regs[IOC_IRQ_STATUS_B] & IOC_IRB_KART_RX_FULL)) {
        ioc->regs[IOC_KART] = byte;
        ioc->regs[IOC_IRQ_STATUS_B] |= IOC_IRB_KART_RX_FULL;
    } else {
        ioc->key_queue[ioc->key_tail] = byte;
        ioc->key_tail = next;
    }
    if (ioc->mach != NULL)
        ioc_update_lines(ioc, &ioc->mach->cpu);
}

void ioc_key_press(Ioc *ioc, uint8_t byte)
{
    ioc_key_push(ioc, byte);
}

void ioc_key_release(Ioc *ioc, uint8_t byte)
{
    ioc_key_push(ioc, (uint8_t)(byte | 0x80u));
}

static uint8_t ioc_reg_read(Ioc *ioc, uint32_t off)
{
    uint8_t r = off & 0x7F;
    if (r == IOC_CONTROL) {
        /* bits 1-7 loop back the output latch; bit 0 is the SDA line:
         * open-drain, host output ANDed with the CMOS device output */
        uint8_t v = (uint8_t)(ioc->regs[IOC_CONTROL] & 0xFEu);
        if (ioc->mach != NULL)
            v |= (uint8_t)((ioc->regs[IOC_CONTROL] & 1) & cmos_sda_r(&ioc->mach->cmos));
        return v;
    }
    if (r == IOC_IRQ_REQUEST_A)
        return (uint8_t)(ioc->regs[IOC_IRQ_STATUS_A] & ioc->regs[IOC_IRQ_MASK_A]);
    if (r == IOC_IRQ_REQUEST_B)
        return (uint8_t)(ioc->regs[IOC_IRQ_STATUS_B] & ioc->regs[IOC_IRQ_MASK_B]);
    if (r == IOC_FIQ_REQUEST)
        return (uint8_t)(ioc->regs[IOC_FIQ_STATUS] & ioc->regs[IOC_FIQ_MASK]);
    if (r == IOC_KART) {
        uint8_t v = ioc->regs[IOC_KART];
        ioc->regs[IOC_IRQ_STATUS_B] &= (uint8_t)~IOC_IRB_KART_RX_FULL;
        if (ioc->key_head != ioc->key_tail) {
            ioc->regs[IOC_KART] = ioc->key_queue[ioc->key_head];
            ioc->key_head = (ioc->key_head + 1) % 16;
            ioc->regs[IOC_IRQ_STATUS_B] |= IOC_IRB_KART_RX_FULL;
        }
        return v;
    }
    return ioc->regs[r];
}

static void ioc_reg_write(Ioc *ioc, uint32_t off, uint8_t v, Arm26 *cpu)
{
    uint8_t r = off & 0x7F;
    switch (r) {
      case IOC_CONTROL:
        /* GPIO C0-C5 outputs; C0=SDA and C1=SCL drive the PCF8583 */
        ioc->regs[r] = v;
        if (ioc->mach != NULL) {
            cmos_sda_w(&ioc->mach->cmos, v & 1);
            cmos_scl_w(&ioc->mach->cmos, (v >> 1) & 1);
        }
        break;
      case IOC_IRQ_REQUEST_A:
        ioc->regs[IOC_IRQ_STATUS_A] &= (uint8_t)~(v & 0x7C); /* write-1-clear */
        break;
      case IOC_IRQ_REQUEST_B:
        ioc->regs[IOC_IRQ_STATUS_B] &= (uint8_t)~v; /* write-1-clear */
        break;
      case IOC_FIQ_REQUEST:
        ioc->regs[IOC_FIQ_STATUS] &= (uint8_t)~v; /* write-1-clear */
        break;
      case IOC_IRQ_MASK_A:
      case IOC_IRQ_MASK_B:
      case IOC_FIQ_MASK:
        ioc->regs[r] = v;
        break;
      case IOC_KART:
        /* start shifting the byte out: Tx-empty clears and re-asserts
         * when the (modelled) shift register has drained */
        ioc->regs[IOC_IRQ_STATUS_B] &= (uint8_t)~IOC_IRB_KART_TX_EMPTY;
        ioc->regs[IOC_KART] = v;
        ioc->kart_tx_cnt = 64; /* ~10 bit times at 2 MHz, in CPU cycles/4 */
        break;
      default:
        if (r >= IOC_T0_LO && r <= IOC_T3_LO + 0x0C) {
            ioc->regs[r] = v;
            int t = (r - IOC_T0_LO) / 0x10;
            uint32_t base = IOC_T0_LO + (uint32_t)t * 0x10;
            if (r == base + 0x08) /* GO: load counter from latch */
                ioc->timer_cnt[t] = (uint32_t)ioc->regs[base + 4] << 8 | ioc->regs[base];
            else if (r == base + 0x0C) {
                /* LATCH write: capture the current counter for readback */
                ioc->regs[base] = (uint8_t)(ioc->timer_cnt[t] & 0xFF);
                ioc->regs[base + 4] = (uint8_t)((ioc->timer_cnt[t] >> 8) & 0xFF);
                break;
            }
            ioc->timer_latch[t] = (uint32_t)ioc->regs[base + 4] << 8 | ioc->regs[base];
            break;
        }
        ioc->regs[r] = v;
        break;
    }
    if (cpu != NULL)
        ioc_update_lines(ioc, cpu);
}

/* 1MHz peripheral bus: peripheral 5 (0x03250000) hosts the floppy latches;
 * the WD1772 block lives at 0x03310000. Other blocks are stubs. */
static uint8_t ioc_periph_rd8(Ioc *ioc, uint32_t off)
{
    if (off >= 0x110000 && off < 0x120000)
        return fdc_rd8(&ioc->mach->fdc, IOC_BASE + off);
    if (off >= 0x50000 && off < 0x60000) {
        if ((off & 0xFFFCu) == 0x70u)
            return 0x0F; /* monitor type */
        return 0xFF;
    }
    return 0xFF;
}

static void ioc_periph_wr8(Ioc *ioc, uint32_t off, uint8_t v)
{
    if (off >= 0x110000 && off < 0x120000) {
        fdc_wr8(&ioc->mach->fdc, IOC_BASE + off, v);
        return;
    }
    if (off >= 0x50000 && off < 0x60000) {
        switch (off & 0xFFFCu) {
          case 0x18u: fdc_latch_b(&ioc->mach->fdc, v); break; /* latch B */
          case 0x40u: fdc_latch_a(&ioc->mach->fdc, v); break; /* latch A */
          default: break;
        }
    }
}

uint8_t ioc_rd8(void *ctx, uint32_t addr)
{
    Ioc *ioc = ctx;
    uint32_t off = addr - IOC_BASE;
    if (off >= 0x10000)
        return ioc_periph_rd8(ioc, off);
    return ioc_reg_read(ioc, off);
}

void ioc_wr8(void *ctx, uint32_t addr, uint8_t v)
{
    Ioc *ioc = ctx;
    uint32_t off = addr - IOC_BASE;
    if (off >= 0x10000) {
        ioc_periph_wr8(ioc, off, v);
        return;
    }
    ioc_reg_write(ioc, off, v, ioc->mach != NULL ? &ioc->mach->cpu : NULL);
}

uint32_t ioc_rd32(void *ctx, uint32_t addr)
{
    Ioc *ioc = ctx;
    uint32_t off = addr - IOC_BASE;
    if (off >= 0x110000 && off < 0x120000)
        return fdc_rd32(&ioc->mach->fdc, addr);
    if (off >= 0x10000)
        return (uint32_t)ioc_periph_rd8(ioc, off & ~3u)
             | ((uint32_t)ioc_periph_rd8(ioc, (off & ~3u) + 1) << 8)
             | ((uint32_t)ioc_periph_rd8(ioc, (off & ~3u) + 2) << 16)
             | ((uint32_t)ioc_periph_rd8(ioc, (off & ~3u) + 3) << 24);
    return (uint32_t)ioc_reg_read(ioc, off & ~3u)
         | ((uint32_t)ioc_reg_read(ioc, (off & ~3u) + 1) << 8)
         | ((uint32_t)ioc_reg_read(ioc, (off & ~3u) + 2) << 16)
         | ((uint32_t)ioc_reg_read(ioc, (off & ~3u) + 3) << 24);
}

void ioc_wr32(void *ctx, uint32_t addr, uint32_t v)
{
    Ioc *ioc = ctx;
    uint32_t off = addr - IOC_BASE;
    if (off >= 0x110000 && off < 0x120000) {
        fdc_wr32(&ioc->mach->fdc, addr, v); /* latches D16-D23 */
        return;
    }
    if (off >= 0x10000) {
        /* peripheral bus: latch D16-D23 on word stores */
        ioc_periph_wr8(ioc, off & ~3u, (uint8_t)((v >> 16) & 0xFF));
        return;
    }
    /* IOC latches D16-D23 on word stores */
    ioc_reg_write(ioc, off & ~3u, (uint8_t)((v >> 16) & 0xFF),
                  ioc->mach != NULL ? &ioc->mach->cpu : NULL);
}
