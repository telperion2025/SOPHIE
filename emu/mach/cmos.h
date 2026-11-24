/*
 * cmos.h - PCF8583 clock/calendar with 240-byte CMOS RAM, modelled as an
 * I2C slave at address 0xA0 hanging off IOC CONTROL GPIO bits 0 (SDA)
 * and 1 (SCL).
 *
 * Register file: 0x00 control (bit 7 = stop, bit 3 = date mask flag),
 * 0x01 hundredths, 0x02 seconds, 0x03 minutes, 0x04 hours, 0x05
 * year/date (bits 7:6 = year & 3), 0x06 month/weekday (bits 7:5 =
 * weekday), 0x07 timer, 0x08 alarm control, 0x09-0x0F alarm, 0x10-0xFF
 * CMOS RAM. The clock registers are BCD and tick at 100 Hz via
 * cmos_tick() (80000 CPU cycles per centisecond at the 8 MHz CPU
 * clock), held while the stop bit is set; the alarm registers are
 * stored but not driven. The I2C bit-level state machine (START/STOP
 * on SDA while SCL high, address byte, register byte, data bytes with
 * ACK on the 9th clock) follows the PCF8583 datasheet as documented in
 * public references; studied emulators are reference-and-credit only.
 *
 * Licence: Apache-2.0 (see LICENSE).
 */
#ifndef SOPHIE_CMOS_H
#define SOPHIE_CMOS_H

#include <stdint.h>

#define CMOS_SIZE 0x100u
#define CMOS_SLAVE_ADDR 0xA0u

enum {
    CMOS_ST_IDLE = 0,
    CMOS_ST_DEVSEL,
    CMOS_ST_REGISTER,
    CMOS_ST_DATAIN,
    CMOS_ST_READSELACK,
    CMOS_ST_DATAOUT
};

typedef struct Cmos {
    uint8_t  data[CMOS_SIZE];
    int      state;
    int      scl, sdaw, sdar; /* line levels: sdaw = host-driven SDA */
    uint8_t  shift, devsel;
    int      bits, reg;
    uint32_t cs_acc; /* centisecond accumulator for cmos_tick */
} Cmos;

/* Reset with the standard *Configure default contents. */
void cmos_init(Cmos *c);

/* I2C line inputs from the IOC GPIO outputs. */
void cmos_sda_w(Cmos *c, int state);
void cmos_scl_w(Cmos *c, int state);

/* Device SDA output (open-drain; AND with the host output at the IOC). */
int cmos_sda_r(const Cmos *c);

/* Advance the BCD clock by cycles_delta CPU cycles (100 Hz). */
void cmos_tick(Cmos *c, uint32_t cycles_delta);

/* Battery-backed persistence of the CMOS RAM region 0x10-0xFF (240
 * bytes; the live clock registers 0x00-0x0F are not persisted).
 * Return 0 on success, -1 if the file cannot be read/written. */
int cmos_save(const Cmos *c, const char *path);
int cmos_load(Cmos *c, const char *path);

#endif /* SOPHIE_CMOS_H */
