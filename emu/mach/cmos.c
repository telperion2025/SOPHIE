/*
 * cmos.c - PCF8583 model (WP3/M3): I2C slave state machine + 256-byte
 * register/CMOS file. See cmos.h for the contract. Licence: Apache-2.0.
 */
#include "cmos.h"

#include <stdio.h>
#include <string.h>

#define CMOS_CS_CYCLES 80000u /* 100 Hz centisecond tick at the 8 MHz CPU clock */

void cmos_init(Cmos *c)
{
    memset(c, 0, sizeof *c);
    c->state = CMOS_ST_IDLE;
    c->sdar = 1;
    c->sdaw = 1;
    c->scl = 1;
    /* *Configure defaults (Autoboot on, one floppy, 256K RamFS, ...) */
    c->data[0x30] = 0x20;
    c->data[0x36] = 0x01;
    c->data[0x45] = 0x17;
    c->data[0x50] = 0x90;
    c->data[0xb4] = 0x24;
    c->data[0xc7] = 0x01;
    c->data[0xd0] = 0x08;
}

static int bcd2bin(uint8_t b)
{
    return ((b >> 4) & 0xFu) * 10 + (b & 0xFu);
}

static uint8_t bin2bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

/* increment a BCD register; returns 1 on carry past max */
static int bcd_inc(uint8_t *r, int max)
{
    int v = bcd2bin(*r) + 1;
    if (v > max) {
        *r = 0;
        return 1;
    }
    *r = bin2bcd(v);
    return 0;
}

static int month_days(int mo, int year2) /* year2 = year & 3; 0 is a leap year */
{
    static const uint8_t dim[13] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (mo < 1 || mo > 12)
        return 31;
    if (mo == 2 && year2 == 0)
        return 29;
    return dim[mo];
}

/* one centisecond: cs -> sec -> min -> hour -> day/month/year/weekday */
static void cmos_advance(Cmos *c)
{
    if (!bcd_inc(&c->data[0x01], 99))
        return;
    if (!bcd_inc(&c->data[0x02], 59))
        return;
    if (!bcd_inc(&c->data[0x03], 59))
        return;
    if (!bcd_inc(&c->data[0x04], 23))
        return;
    int year2 = (c->data[0x05] >> 6) & 3;
    int day = bcd2bin(c->data[0x05] & 0x3Fu) + 1;
    int mo = bcd2bin(c->data[0x06] & 0x1Fu);
    if (mo < 1 || mo > 12)
        mo = 1;
    if (day > month_days(mo, year2)) {
        day = 1;
        if (++mo > 12) {
            mo = 1;
            year2 = (year2 + 1) & 3;
        }
    }
    int wd = ((c->data[0x06] >> 5) + 1) & 7;
    c->data[0x05] = (uint8_t)((year2 << 6) | bin2bcd(day));
    c->data[0x06] = (uint8_t)((wd << 5) | bin2bcd(mo));
}

void cmos_tick(Cmos *c, uint32_t cycles_delta)
{
    if (c->data[0x00] & 0x80u)
        return; /* stop bit: counters held, accumulator not fed */
    c->cs_acc += cycles_delta;
    while (c->cs_acc >= CMOS_CS_CYCLES) {
        c->cs_acc -= CMOS_CS_CYCLES;
        cmos_advance(c);
    }
}

int cmos_save(const Cmos *c, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return -1;
    size_t n = fwrite(c->data + 0x10, 1, CMOS_SIZE - 0x10, f);
    fclose(f);
    return n == CMOS_SIZE - 0x10 ? 0 : -1;
}

int cmos_load(Cmos *c, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    size_t n = fread(c->data + 0x10, 1, CMOS_SIZE - 0x10, f);
    fclose(f);
    return n == CMOS_SIZE - 0x10 ? 0 : -1;
}

void cmos_scl_w(Cmos *c, int state)
{
    state &= 1;
    if (c->scl == state)
        return;
    c->scl = state;

    switch (c->state) {
      case CMOS_ST_DEVSEL:
      case CMOS_ST_REGISTER:
      case CMOS_ST_DATAIN:
        if (c->bits < 8) {
            if (c->scl) {
                c->shift = (uint8_t)((c->shift << 1) | c->sdaw);
                c->bits++;
            }
        } else {
            if (c->scl) {
                c->bits++;
            } else if (c->bits == 8) {
                switch (c->state) {
                  case CMOS_ST_DEVSEL:
                    c->devsel = c->shift;
                    if ((c->devsel & 0xFEu) != CMOS_SLAVE_ADDR)
                        c->state = CMOS_ST_IDLE;
                    else if ((c->devsel & 1) == 0)
                        c->state = CMOS_ST_REGISTER;
                    else
                        c->state = CMOS_ST_READSELACK;
                    break;
                  case CMOS_ST_REGISTER:
                    c->reg = c->shift;
                    c->state = CMOS_ST_DATAIN;
                    break;
                  case CMOS_ST_DATAIN:
                    c->data[c->reg] = c->shift;
                    c->reg = (c->reg + 1) & 0xFF;
                    break;
                }
                if (c->state != CMOS_ST_IDLE)
                    c->sdar = 0; /* ACK: pull SDA low for the 9th clock */
            } else {
                c->bits = 0;
                c->sdar = 1;
            }
        }
        break;

      case CMOS_ST_READSELACK:
        c->bits = 0;
        c->state = CMOS_ST_DATAOUT;
        break;

      case CMOS_ST_DATAOUT:
        if (c->bits < 8) {
            if (c->scl) {
                c->bits++;
            } else {
                if (c->bits == 0) {
                    c->shift = c->data[c->reg];
                    if ((c->data[0x00] & 0x08) != 0) { /* mask flag */
                        if (c->reg == 0x05)
                            c->shift &= 0x3F;
                        else if (c->reg == 0x06)
                            c->shift &= 0x1F;
                    }
                    c->reg = (c->reg + 1) & 0xFF;
                }
                c->sdar = (c->shift >> 7) & 1;
                c->shift = (uint8_t)(c->shift << 1);
            }
        } else {
            if (c->scl) {
                if (c->sdaw)
                    c->state = CMOS_ST_IDLE; /* NACK from host: stop */
                c->bits = 0;
            } else {
                c->sdar = 1;
            }
        }
        break;

      default:
        break;
    }
}

void cmos_sda_w(Cmos *c, int state)
{
    state &= 1;
    if (c->sdaw == state)
        return;
    c->sdaw = state;
    if (c->scl) {
        if (c->sdaw)
            c->state = CMOS_ST_IDLE; /* STOP: SDA rises while SCL high */
        else {
            c->state = CMOS_ST_DEVSEL; /* START: SDA falls while SCL high */
            c->bits = 0;
        }
        c->sdar = 1;
    }
}

int cmos_sda_r(const Cmos *c)
{
    return c->sdar & 1;
}
