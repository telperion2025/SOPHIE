/*
 * cmos.c - PCF8583 model (WP3/M3): I2C slave state machine + 256-byte
 * register/CMOS file. See cmos.h for the contract. Licence: Apache-2.0.
 */
#include "cmos.h"

#include <string.h>

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
