/*
 * cmos_test.c - WP3: PCF8583 CMOS/RTC tests. I2C state machine, *Configure
 * defaults, battery-backed save/load, and the ticking BCD clock (100 Hz
 * centisecond chain: carries, month lengths/leap, stop bit, mask flag).
 *
 * Run: ./cmos_test   (via `make test`)
 * Licence: Apache-2.0 (see LICENSE).
 */
#include <stdio.h>
#include <inttypes.h>
#include <string.h>

#include "test.h"
#include "cmos.h"

#define CS_CYC 80000u /* one centisecond at the 8 MHz CPU clock */

/* ---------- I2C bitbang helpers (SDA=bit0, SCL=bit1, as at IOC CONTROL) ---------- */

static void i2c_set(Cmos *c, int sda, int scl)
{
    cmos_sda_w(c, sda);
    cmos_scl_w(c, scl);
}

static void i2c_start(Cmos *c)
{
    i2c_set(c, 1, 1);
    i2c_set(c, 0, 1); /* SDA falls while SCL high: START */
    i2c_set(c, 0, 0);
}

static void i2c_stop(Cmos *c)
{
    i2c_set(c, 0, 0);
    i2c_set(c, 0, 1);
    i2c_set(c, 1, 1); /* SDA rises while SCL high: STOP */
}

/* transmit one byte, return the ACK level sampled on the 9th clock (0=ACK) */
static int i2c_tx(Cmos *c, uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        int bit = (b >> i) & 1;
        i2c_set(c, bit, 0);
        i2c_set(c, bit, 1);
        i2c_set(c, bit, 0);
    }
    i2c_set(c, 1, 0); /* release SDA for the ACK bit */
    i2c_set(c, 1, 1);
    int ack = cmos_sda_r(c);
    i2c_set(c, 1, 0);
    return ack;
}

static uint8_t i2c_rx(Cmos *c, int ack)
{
    uint8_t b = 0;
    i2c_set(c, 1, 0);
    for (int i = 7; i >= 0; i--) {
        i2c_set(c, 1, 1);
        b = (uint8_t)((b << 1) | (uint8_t)cmos_sda_r(c));
        i2c_set(c, 1, 0);
    }
    i2c_set(c, ack ? 0 : 1, 0); /* ack=1: host ACK (SDA low, continue); ack=0: NACK (end) */
    i2c_set(c, ack ? 0 : 1, 1);
    i2c_set(c, ack ? 0 : 1, 0);
    return b;
}

static uint8_t cmos_read_reg(Cmos *c, uint8_t reg)
{
    i2c_start(c);
    i2c_tx(c, CMOS_SLAVE_ADDR);
    i2c_tx(c, reg);
    i2c_start(c); /* repeated START */
    i2c_tx(c, CMOS_SLAVE_ADDR | 1u);
    uint8_t b = i2c_rx(c, 0); /* NACK: end of read */
    i2c_stop(c);
    return b;
}

static void cmos_write_reg(Cmos *c, uint8_t reg, uint8_t v)
{
    i2c_start(c);
    i2c_tx(c, CMOS_SLAVE_ADDR);
    i2c_tx(c, reg);
    i2c_tx(c, v);
    i2c_stop(c);
}

int main(void)
{
    char det[128];

    /* 1: *Configure defaults + power-on clock state (all zero, not stopped) */
    {
        Cmos c; cmos_init(&c);
        snprintf(det, sizeof det, "0x30=%02x 0xc7=%02x 0xd0=%02x ctrl=%02x cs=%02x",
                 c.data[0x30], c.data[0xc7], c.data[0xd0], c.data[0x00], c.data[0x01]);
        check("cmos-defaults",
              c.data[0x30] == 0x20 && c.data[0xc7] == 0x01 && c.data[0xd0] == 0x08 &&
              c.data[0x00] == 0x00 && c.data[0x01] == 0x00 && c.data[0x06] == 0x00,
              det);
    }

    /* 2: address ACK / NACK (wrong slave address is not ACKed) */
    {
        Cmos c; cmos_init(&c);
        i2c_start(&c);
        int ack_bad = i2c_tx(&c, CMOS_SLAVE_ADDR ^ 0x02u);
        i2c_stop(&c);
        i2c_start(&c);
        int ack_ok = i2c_tx(&c, CMOS_SLAVE_ADDR);
        i2c_stop(&c);
        snprintf(det, sizeof det, "ack=%d,%d", ack_ok, ack_bad);
        check("cmos-addr-ack", ack_ok == 0 && ack_bad == 1, det);
    }

    /* 3: one centisecond tick */
    {
        Cmos c; cmos_init(&c);
        cmos_tick(&c, CS_CYC);
        snprintf(det, sizeof det, "cs=%02x sec=%02x", c.data[0x01], c.data[0x02]);
        check("clock-1cs", c.data[0x01] == 0x01 && c.data[0x02] == 0x00, det);
    }

    /* 4: sub-tick accumulation across calls */
    {
        Cmos c; cmos_init(&c);
        cmos_tick(&c, CS_CYC / 2);
        cmos_tick(&c, CS_CYC / 2 - 1);
        int mid = c.data[0x01];
        cmos_tick(&c, 1);
        snprintf(det, sizeof det, "mid=%02x now=%02x", mid, c.data[0x01]);
        check("clock-accumulate", mid == 0x00 && c.data[0x01] == 0x01, det);
    }

    /* 5: centisecond -> second carry (100 cs = 1 s) */
    {
        Cmos c; cmos_init(&c);
        cmos_tick(&c, CS_CYC * 100);
        snprintf(det, sizeof det, "cs=%02x sec=%02x", c.data[0x01], c.data[0x02]);
        check("clock-cs-carry", c.data[0x01] == 0x00 && c.data[0x02] == 0x01, det);
    }

    /* 6: second -> minute carry at 59:xx */
    {
        Cmos c; cmos_init(&c);
        c.data[0x01] = 0x99; c.data[0x02] = 0x59;
        cmos_tick(&c, CS_CYC);
        snprintf(det, sizeof det, "cs=%02x sec=%02x min=%02x",
                 c.data[0x01], c.data[0x02], c.data[0x03]);
        check("clock-sec-carry",
              c.data[0x01] == 0x00 && c.data[0x02] == 0x00 && c.data[0x03] == 0x01, det);
    }

    /* 7: 23:59:59.99 -> midnight, day and weekday advance */
    {
        Cmos c; cmos_init(&c);
        c.data[0x01] = 0x99; c.data[0x02] = 0x59; c.data[0x03] = 0x59; c.data[0x04] = 0x23;
        c.data[0x05] = 0x01; /* day 1, year bits 0 */
        c.data[0x06] = 0x01; /* month 1, weekday 0 */
        cmos_tick(&c, CS_CYC);
        snprintf(det, sizeof det, "h=%02x date=%02x mon/wd=%02x",
                 c.data[0x04], c.data[0x05], c.data[0x06]);
        check("clock-day-carry",
              c.data[0x04] == 0x00 && c.data[0x05] == 0x02 && c.data[0x06] == 0x21, det);
    }

    /* 8: Dec 31 23:59:59.99 -> Jan 1, year field increments (bits 7:6) */
    {
        Cmos c; cmos_init(&c);
        c.data[0x01] = 0x99; c.data[0x02] = 0x59; c.data[0x03] = 0x59; c.data[0x04] = 0x23;
        c.data[0x05] = 0x31; /* day 31, year bits 0 */
        c.data[0x06] = 0x12; /* month 12, weekday 0 */
        cmos_tick(&c, CS_CYC);
        snprintf(det, sizeof det, "date=%02x mon/wd=%02x", c.data[0x05], c.data[0x06]);
        check("clock-month-year-carry",
              c.data[0x05] == 0x41 && c.data[0x06] == 0x21, det);
    }

    /* 9: February length: leap (year&3 == 0) has 29 days, else 28 */
    {
        Cmos c; cmos_init(&c);
        c.data[0x01] = 0x99; c.data[0x02] = 0x59; c.data[0x03] = 0x59; c.data[0x04] = 0x23;
        c.data[0x05] = 0x29; c.data[0x06] = 0x02; /* Feb 29, leap year 0 */
        cmos_tick(&c, CS_CYC);
        int leap = (c.data[0x05] == 0x01 && c.data[0x06] == 0x23);
        c.data[0x01] = 0x99; c.data[0x02] = 0x59; c.data[0x03] = 0x59; c.data[0x04] = 0x23;
        c.data[0x05] = 0x68; c.data[0x06] = 0x02; /* Feb 28, year bits 1 (not leap) */
        cmos_tick(&c, CS_CYC);
        snprintf(det, sizeof det, "leap date=%02x mon=%02x -> nonleap date=%02x mon=%02x",
                 0x01, 0x23, c.data[0x05], c.data[0x06]);
        check("clock-feb-leap",
              leap && c.data[0x05] == 0x41 && c.data[0x06] == 0x23, det);
    }

    /* 10: stop bit holds the counters and does not feed the accumulator */
    {
        Cmos c; cmos_init(&c);
        c.data[0x00] = 0x80;
        cmos_tick(&c, CS_CYC * 500);
        int held = c.data[0x01];
        c.data[0x00] = 0x00;
        cmos_tick(&c, CS_CYC);
        snprintf(det, sizeof det, "held=%02x after=%02x", held, c.data[0x01]);
        check("clock-stop-bit", held == 0x00 && c.data[0x01] == 0x01, det);
    }

    /* 11: guest-visible I2C read of the ticking clock (the reported gap:
     * clock regs 0x01-0x06 used to always read 0) */
    {
        Cmos c; cmos_init(&c);
        cmos_tick(&c, CS_CYC * 150); /* 1.50 s -> cs=50 (0x50), sec=1 */
        uint8_t cs = cmos_read_reg(&c, 0x01);
        uint8_t sec = cmos_read_reg(&c, 0x02);
        snprintf(det, sizeof det, "i2c cs=%02x sec=%02x", cs, sec);
        check("clock-i2c-read", cs == 0x50 && sec == 0x01, det);
    }

    /* 12: date mask flag (control bit 3) masks year/weekday bits on read */
    {
        Cmos c; cmos_init(&c);
        c.data[0x00] = 0x08;
        c.data[0x05] = 0xC5; c.data[0x06] = 0xE4;
        uint8_t d = cmos_read_reg(&c, 0x05);
        uint8_t m = cmos_read_reg(&c, 0x06);
        c.data[0x00] = 0x00;
        uint8_t d_full = cmos_read_reg(&c, 0x05);
        snprintf(det, sizeof det, "masked=%02x,%02x full=%02x", d, m, d_full);
        check("cmos-mask-flag", d == 0x05 && m == 0x04 && d_full == 0xC5, det);
    }

    /* 13: CMOS RAM write/read with register auto-increment */
    {
        Cmos c; cmos_init(&c);
        i2c_start(&c);
        i2c_tx(&c, CMOS_SLAVE_ADDR);
        i2c_tx(&c, 0x40);
        i2c_tx(&c, 0x5A);
        i2c_tx(&c, 0xA5);
        i2c_stop(&c);
        i2c_start(&c);
        i2c_tx(&c, CMOS_SLAVE_ADDR);
        i2c_tx(&c, 0x40);
        i2c_start(&c);
        i2c_tx(&c, CMOS_SLAVE_ADDR | 1u);
        uint8_t b0 = i2c_rx(&c, 1); /* ACK: continue */
        uint8_t b1 = i2c_rx(&c, 0); /* NACK: end */
        i2c_stop(&c);
        snprintf(det, sizeof det, "b0=%02x b1=%02x", b0, b1);
        check("cmos-ram-autoinc", b0 == 0x5A && b1 == 0xA5, det);
    }

    /* 14: battery-backed save/load persists CMOS RAM 0x10-0xFF only */
    {
        const char *path = "cmos_test.tmp";
        Cmos a, b;
        cmos_init(&a);
        cmos_write_reg(&a, 0x40, 0x5A);
        a.data[0x02] = 0x30; /* clock register: must NOT persist */
        int sv = cmos_save(&a, path);
        cmos_init(&b);
        int ld = cmos_load(&b, path);
        int ld_missing = cmos_load(&b, "cmos_test.no-such-file");
        snprintf(det, sizeof det, "sv=%d ld=%d missing=%d b[0x40]=%02x b[0x02]=%02x",
                 sv, ld, ld_missing, b.data[0x40], b.data[0x02]);
        remove(path);
        check("cmos-save-load",
              sv == 0 && ld == 0 && ld_missing == -1 && b.data[0x40] == 0x5A && b.data[0x02] == 0x00,
              det);
    }

    return test_summary("cmos") ? 1 : 0;
}
