/*
 * fdc_test.c - WP7 acceptance: WD1772 floppy controller model + disc image
 * backend on the Archimedes base machine.
 *
 * Covers (spec section 10, WP7): register map at 0x03310000 (status/cmd,
 * track, sector, data), D16-D23 word-store latching, type I commands
 * (restore/seek/step with track update), read sector streaming with DRQ ->
 * IOC FIQ bit FH0 and completion INTRQ -> FH1, write sector into the image,
 * record-not-found, write-protect, read address, force interrupt, master
 * reset via latch B, drive select/side/motor via latch A, and the disc
 * geometry table.
 *
 * Run: ./fdc_test   (via `make test`)
 * Licence: Apache-2.0 (see LICENSE).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test.h"
#include "machine.h"

#define LATCH_B_ADDR 0x03250018u
#define LATCH_A_ADDR 0x03250040u
#define FIQ_STATUS_ADDR (IOC_BASE + IOC_FIQ_STATUS)

static uint8_t ram[0x400000];
static uint8_t rom[0x80000];

static Machine m;

/* ---------- tiny ARM encoders for the guest-program test ---------- */

static uint32_t A_IMM12(uint32_t v)
{
    for (int r = 0; r < 16; r++) {
        uint32_t rot = (uint32_t)r * 2;
        uint32_t imm8 = rot ? ((v << rot) | (v >> (32 - rot))) : v;
        imm8 &= 0xFFu;
        uint32_t check = rot ? ((imm8 >> rot) | (imm8 << (32 - rot))) : imm8;
        if (check == v)
            return ((uint32_t)r << 8) | imm8;
    }
    return v & 0xFFu;
}

static uint32_t A_MOV_IMM(int rd, uint32_t imm)  { return 0xE3A00000u | ((uint32_t)rd << 12) | A_IMM12(imm); }
static uint32_t A_MOV_LSL(int rd, int rm, int n)  { return 0xE1A00000u | ((uint32_t)rd << 12) | ((uint32_t)(n & 31) << 7) | (uint32_t)rm; }
static uint32_t A_ADD_REG(int rd, int rn, int rm) { return 0xE0800000u | ((uint32_t)rn << 16) | ((uint32_t)rd << 12) | (uint32_t)rm; }
static uint32_t A_ORR_IMM(int rd, int rn, uint32_t imm) { return 0xE3800000u | ((uint32_t)rn << 16) | ((uint32_t)rd << 12) | A_IMM12(imm); }
static uint32_t A_ADD_IMM(int rd, int rn, uint32_t imm) { return 0xE2800000u | ((uint32_t)rn << 16) | ((uint32_t)rd << 12) | A_IMM12(imm); }
static uint32_t A_SUBS_IMM(int rd, int rn, uint32_t imm) { return 0xE2500000u | ((uint32_t)rn << 16) | ((uint32_t)rd << 12) | A_IMM12(imm); }
static uint32_t A_B(uint32_t pc, uint32_t t)    { return 0xEA000000u | (((t - (pc + 8)) >> 2) & 0xFFFFFFu); }
static uint32_t A_BNE(uint32_t pc, uint32_t t)  { return 0x1A000000u | (((t - (pc + 8)) >> 2) & 0xFFFFFFu); }
static uint32_t A_BEQ(uint32_t pc, uint32_t t)  { return 0x0A000000u | (((t - (pc + 8)) >> 2) & 0xFFFFFFu); }
static uint32_t A_STR(int rd, int rn, uint32_t o){ return 0xE5800000u | ((uint32_t)rn << 16) | ((uint32_t)rd << 12) | (o & 0xFFFu); }
static uint32_t A_STRB(int rd, int rn, uint32_t o){ return 0xE5C00000u | ((uint32_t)rn << 16) | ((uint32_t)rd << 12) | (o & 0xFFFu); }
static uint32_t A_LDRB(int rd, int rn, uint32_t o){ return 0xE5D00000u | ((uint32_t)rn << 16) | ((uint32_t)rd << 12) | (o & 0xFFFu); }
static uint32_t A_TST_IMM(int rn, uint32_t imm)    { return 0xE3100000u | ((uint32_t)rn << 16) | A_IMM12(imm); }
#define A_SPIN 0xEAFFFFFEu

/* Guest program: boots from the ROM window, drives the WD1772 through the
 * real bus (latches + restore/seek/read sector of track 0 sector 0), sums
 * the 1024 data bytes and stores sum + a magic to physical RAM 0x2000100. */
#define G_ROM_BASE 0x03800000u
#define G_START 0x40u

static void build_guest_rom(uint32_t *g)
{
    memset(g, 0, 0x200 * 4);
    g[0x00] = A_B(0x00000000u, G_ROM_BASE + G_START * 4);
    for (int i = 1; i < 8; i++)
        g[i] = A_SPIN;
    uint32_t *c = g + G_START;
    uint32_t pc = G_ROM_BASE + G_START * 4;
    *c++ = A_MOV_IMM(2, 0x32); pc += 4;  *c++ = A_MOV_LSL(2, 2, 20); pc += 4; /* r2 = 0x03200000 */
    *c++ = A_MOV_IMM(3, 0x33); pc += 4;  *c++ = A_MOV_LSL(3, 3, 20); pc += 4;
    *c++ = A_ADD_IMM(3, 3, 0x10000); pc += 4;                            /* r3 = 0x03310000 (FDC) */
    *c++ = A_ADD_IMM(9, 2, 0x50000); pc += 4;                            /* r9 = 0x03250000 (periph 5) */
    *c++ = A_MOV_IMM(4, 0x08); pc += 4;  *c++ = A_STRB(4, 9, 0x18);   pc += 4; /* latch B: mr=1, DDEN=0 */
    *c++ = A_MOV_IMM(4, 0xBE); pc += 4;  *c++ = A_STRB(4, 9, 0x40);   pc += 4; /* latch A: drive0, side0, motor on */
    *c++ = A_MOV_IMM(4, 0x00); pc += 4;  *c++ = A_STRB(4, 3, 0x00);   pc += 4; /* cmd: restore */
    uint32_t poll1 = pc;
    *c++ = A_LDRB(4, 3, 0x00); pc += 4;
    *c++ = A_TST_IMM(4, 0x01); pc += 4;
    *c++ = A_BNE(pc, poll1); pc += 4;                /* wait !busy */
    *c++ = A_MOV_IMM(4, 0x10); pc += 4;  *c++ = A_STRB(4, 3, 0x00);   pc += 4; /* cmd: seek (data=0) */
    uint32_t poll2 = pc;
    *c++ = A_LDRB(4, 3, 0x00); pc += 4;
    *c++ = A_TST_IMM(4, 0x01); pc += 4;
    *c++ = A_BNE(pc, poll2); pc += 4;
    *c++ = A_MOV_IMM(4, 0x00); pc += 4;  *c++ = A_STRB(4, 3, 0x08);   pc += 4; /* sector = 0 */
    *c++ = A_MOV_IMM(4, 0x80); pc += 4;  *c++ = A_STRB(4, 3, 0x00);   pc += 4; /* cmd: read sector */
    *c++ = A_MOV_IMM(5, 0x00); pc += 4;                               /* r5 = sum */
    *c++ = A_MOV_IMM(6, 0x04); pc += 4;  *c++ = A_MOV_LSL(6, 6, 8); pc += 4;  /* r6 = 1024 */
    uint32_t poll3 = pc;
    *c++ = A_LDRB(4, 3, 0x00); pc += 4;
    *c++ = A_TST_IMM(4, 0x02); pc += 4;
    *c++ = A_BEQ(pc, poll3); pc += 4;                /* wait DRQ */
    *c++ = A_LDRB(4, 3, 0x0C); pc += 4;                               /* data byte */
    *c++ = A_ADD_REG(5, 5, 4); pc += 4;
    *c++ = A_SUBS_IMM(6, 6, 1); pc += 4;
    *c++ = A_BNE(pc, poll3); pc += 4;
    uint32_t poll4 = pc;
    *c++ = A_LDRB(4, 3, 0x00); pc += 4;
    *c++ = A_TST_IMM(4, 0x01); pc += 4;
    *c++ = A_BNE(pc, poll4); pc += 4;                /* wait !busy (done) */
    *c++ = A_MOV_IMM(7, 0x02); pc += 4;  *c++ = A_MOV_LSL(7, 7, 24); pc += 4; /* r7 = 0x02000000 */
    *c++ = A_STR(5, 7, 0x100); pc += 4;                               /* [0x2000100] = sum */
    *c++ = A_MOV_IMM(8, 0x60); pc += 4;
    *c++ = A_MOV_LSL(8, 8, 8); pc += 4;
    *c++ = A_ORR_IMM(8, 8, 0x0D); pc += 4;                            /* r8 = 0x600D */
    *c++ = A_STR(8, 7, 0x104); pc += 4;                               /* [0x2000104] = magic */
    *c++ = A_SPIN;                                                    /* done: spin */
}

static uint8_t fdc_rd(uint32_t a) { return mem_rd8(&m.bus, a); }
static void fdc_wr(uint32_t a, uint8_t v) { mem_wr8(&m.bus, a, v); }
static uint8_t fiq_status(void) { return ioc_rd8(&m.ioc, FIQ_STATUS_ADDR); }
static void tick(uint32_t cycles) { fdc_tick(&m.fdc, cycles); }

static char img800k[] = "/tmp/sophie_fdc_test_XXXXXX";
static char img640k[] = "/tmp/sophie_fdc_test2_XXXXXX";

static int make_image(char *tmpl, uint32_t size, int fill_track5)
{
    int fd = mkstemp(tmpl);
    if (fd < 0)
        return -1;
    uint8_t *buf = calloc(1, size);
    if (fill_track5) {
        /* track 5, side 0, sector 0 (1024B sectors, 0-based): offset 5*2*5*1024 = 51200 */
        for (int i = 0; i < 1024; i++)
            buf[51200 + i] = (uint8_t)(i & 0xFF);
    }
    if (write(fd, buf, size) != (ssize_t)size) {
        close(fd);
        free(buf);
        return -1;
    }
    close(fd);
    free(buf);
    return 0;
}

int main(void)
{
    machine_init(&m, ram, sizeof ram, rom, sizeof rom);

    /* geometry table */
    {
        int tr, si, sp, ss, sb;
        check("geom-800k", disc_geometry_for_size(819200, &tr, &si, &sp, &ss, &sb) == 0 &&
              tr == 80 && si == 2 && sp == 5 && ss == 1024 && sb == 0, "ADFS D/E");
        check("geom-640k", disc_geometry_for_size(655360, &tr, &si, &sp, &ss, &sb) == 0 &&
              tr == 80 && si == 2 && sp == 16 && ss == 256 && sb == 0, "ADFS L");
        check("geom-unknown", disc_geometry_for_size(12345, &tr, &si, &sp, &ss, &sb) == -1, "reject");
    }

    /* power-on status: motor off */
    check("status-mon-at-reset", fdc_rd(FDC_BASE) == FDC_S_MON, "status after init");

    /* latches: B = mr high + DDEN low (MFM); A = drive 0, side 0, motor on */
    fdc_wr(LATCH_B_ADDR, 0x08);
    fdc_wr(LATCH_A_ADDR, 0xDE);
    check("latch-motor-on", m.fdc.motor == 1 && m.fdc.drive == 0 && m.fdc.side == 0 &&
          m.fdc.density == 1, "latch A/B decode");

    /* restore: busy+spin during, TR00 + INTRQ (FH1) after */
    fdc_wr(FDC_BASE + 0x00, 0x00);
    check("restore-busy", (fdc_rd(FDC_BASE) & (FDC_S_BUSY | FDC_S_SPIN)) ==
          (FDC_S_BUSY | FDC_S_SPIN), "busy during restore");
    tick(400000);
    check("restore-intrq", (fiq_status() & FDC_FIQ_IRQ) != 0, "INTRQ -> FIQ FH1");
    check("restore-done-tr00", (fdc_rd(FDC_BASE) & (FDC_S_BUSY | FDC_S_TR00)) == FDC_S_TR00,
          "status after restore");
    check("status-read-clears-intrq", (fiq_status() & FDC_FIQ_IRQ) == 0, "read clears INTRQ");
    check("restore-track0", fdc_rd(FDC_BASE + 0x04) == 0, "track reg 0");

    /* seek to track 5 (data reg holds target) */
    fdc_wr(FDC_BASE + 0x04, 5);      /* track */
    fdc_wr(FDC_BASE + 0x0C, 5);      /* data */
    fdc_wr(FDC_BASE + 0x00, 0x10);   /* seek */
    tick(400000);
    check("seek-track5", fdc_rd(FDC_BASE + 0x04) == 5 && m.fdc.curtrack == 5 &&
          !(fdc_rd(FDC_BASE) & FDC_S_TR00), "seek updates track");

    /* step in with update flag (0x44): track 6 */
    fdc_wr(FDC_BASE + 0x00, 0x44);
    tick(400000);
    check("step-in-update", m.fdc.curtrack == 6 && fdc_rd(FDC_BASE + 0x04) == 6, "step in +U");
    /* step out with update (0x64): back to 5 */
    fdc_wr(FDC_BASE + 0x00, 0x64);
    tick(400000);
    check("step-out-update", m.fdc.curtrack == 5 && fdc_rd(FDC_BASE + 0x04) == 5, "step out +U");

    /* attach an 800K ADFS D/E image with a known pattern in track 5 side 0 sector 0 */
    if (make_image(img800k, 819200, 1) != 0) {
        check("make-800k-image", 0, "mkstemp/write failed");
        return test_summary("fdc");
    }
    check("attach-800k", machine_disc_attach(&m, 0, img800k, 0) == 0, "disc_load");

    /* read sector 0 of track 5: DRQ per byte (FH0), 1024 bytes, then INTRQ */
    fdc_wr(FDC_BASE + 0x08, 0);      /* sector */
    fdc_wr(FDC_BASE + 0x00, 0x80);   /* read sector */
    int ok = 1, pos = 0, guard = 0, saw_drq_fiq = 0;
    while (pos < 1024 && guard++ < 200000) {
        tick(49); /* one data byte per 48-cycle slot */
        if (fdc_rd(FDC_BASE) & FDC_S_DRQ) {
            if (pos == 0 && (fiq_status() & FDC_FIQ_DRQ))
                saw_drq_fiq = 1;
            uint8_t b = fdc_rd(FDC_BASE + 0x0C);
            if (b != (uint8_t)(pos & 0xFF))
                ok = 0;
            pos++;
        }
    }
    check("read-drq-first-byte", saw_drq_fiq, "DRQ -> FIQ FH0");
    check("read-sector-1024-bytes", ok && pos == 1024, "sector data matches image");
    tick(1000);
    check("read-sector-done", (fiq_status() & FDC_FIQ_IRQ) && !(m.fdc.status & FDC_S_BUSY),
          "INTRQ after last byte");

    /* write sector 1 of track 5, then verify through the disc backend */
    fdc_wr(FDC_BASE + 0x08, 1);
    fdc_wr(FDC_BASE + 0x00, 0xA0);   /* write sector */
    check("write-drq-requests-byte", fdc_rd(FDC_BASE) & FDC_S_DRQ, "DRQ at write start");
    for (int i = 0; i < 1024; i++)
        fdc_wr(FDC_BASE + 0x0C, (uint8_t)(0xA5 ^ i));
    tick(1000);
    check("write-sector-done", (fiq_status() & FDC_FIQ_IRQ) && !(m.fdc.status & FDC_S_BUSY),
          "INTRQ after write");
    {
        uint8_t buf[1024];
        int wok = disc_read_sector(&m.disc[0], 5, 0, 1, buf) == 0;
        for (int i = 0; i < 1024 && wok; i++)
            if (buf[i] != (uint8_t)(0xA5 ^ i))
                wok = 0;
        check("write-sector-persisted", wok, "image sector 1 content");
    }

    /* record not found: sector 99 */
    fdc_wr(FDC_BASE + 0x08, 99);
    fdc_wr(FDC_BASE + 0x00, 0x80);
    tick(200000);
    check("read-rnf", (m.fdc.status & FDC_S_RNF) && (fiq_status() & FDC_FIQ_IRQ),
          "RNF + INTRQ");

    /* read address: 6 ID bytes, sector reg <- track afterwards */
    fdc_wr(FDC_BASE + 0x00, 0xC0);
    {
        uint8_t id[6];
        int got = 0, guard2 = 0;
        while (got < 6 && guard2++ < 100000) {
            tick(65); /* one ID byte per 64-cycle slot */
            if (fdc_rd(FDC_BASE) & FDC_S_DRQ) {
                id[got++] = fdc_rd(FDC_BASE + 0x0C);
            }
        }
        tick(1000);
        check("read-address-6-bytes", got == 6 && id[0] == 5 && id[3] == 3, "track, sizecode=1024");
        check("read-address-sector-track", fdc_rd(FDC_BASE + 0x08) == 5, "sector <- track");
    }

    /* force interrupt aborts a busy command and raises INTRQ (bit 3) */
    fdc_wr(FDC_BASE + 0x08, 2);
    fdc_wr(FDC_BASE + 0x00, 0x80);
    tick(1000);
    check("busy-before-force-int", fdc_rd(FDC_BASE) & FDC_S_BUSY, "busy");
    fdc_wr(FDC_BASE + 0x00, 0xD8);
    check("force-int-clears-busy", !(m.fdc.status & FDC_S_BUSY) &&
          (fiq_status() & FDC_FIQ_IRQ), "force interrupt 0xD8");

    /* write-protected drive 1: write sector -> WP status */
    if (make_image(img640k, 655360, 0) != 0) {
        check("make-640k-image", 0, "mkstemp/write failed");
    } else {
        check("attach-640k-ro", machine_disc_attach(&m, 1, img640k, 1) == 0, "wprot attach");
        fdc_wr(LATCH_A_ADDR, 0xDD); /* drive 1, side 0, motor on */
        fdc_wr(FDC_BASE + 0x04, 0);
        fdc_wr(FDC_BASE + 0x0C, 0);
        fdc_wr(FDC_BASE + 0x00, 0x10); /* seek track 0 */
        tick(400000);
        fdc_wr(FDC_BASE + 0x08, 0);
        fdc_wr(FDC_BASE + 0x00, 0xA0); /* write sector */
        tick(200000);
        check("write-protect-status", (m.fdc.status & FDC_S_WP) &&
              (fiq_status() & FDC_FIQ_IRQ), "WP + INTRQ");
        fdc_wr(LATCH_A_ADDR, 0xDE); /* back to drive 0 */
    }

    /* word store latches D16-D23: seek via mem_wr32 */
    fdc_wr(FDC_BASE + 0x0C, 3);
    mem_wr32(&m.bus, FDC_BASE, (uint32_t)0x10 << 16); /* command 0x10 */
    tick(400000);
    check("word-store-d16-23", m.fdc.curtrack == 3 && fdc_rd(FDC_BASE + 0x04) == 3,
          "STR to cmd reg");
    check("word-read-composes-lanes", (mem_rd32(&m.bus, FDC_BASE) & 0xFF) ==
          (fdc_rd(FDC_BASE) & 0xFF), "LDR status low lane");

    /* master reset via latch B bit 3 low */
    fdc_wr(LATCH_B_ADDR, 0x00);
    check("mr-resets", m.fdc.status == 0 && fdc_rd(FDC_BASE + 0x08) == 1 && !m.fdc.intrq &&
          !m.fdc.drq, "mr clears status, sector=1");
    fdc_wr(LATCH_B_ADDR, 0x08);

    machine_disc_detach(&m, 0);
    machine_disc_detach(&m, 1);
    unlink(img800k);
    unlink(img640k);

    /* ---------- end-to-end: guest ARM code drives the FDC via the bus ---------- */
    {
        static uint32_t grom[0x200];
        static uint8_t gram[0x400000];
        build_guest_rom(grom);
        char gimg[] = "/tmp/sophie_fdc_guest_XXXXXX";
        int fd = mkstemp(gimg);
        uint8_t *ib = calloc(1, 819200);
        for (int i = 0; i < 1024; i++)
            ib[i] = (uint8_t)(i & 0xFF); /* track 0 side 0 sector 0 */
        if (fd >= 0 && write(fd, ib, 819200) == 819200) {
            close(fd);
            free(ib);
            Machine g;
            machine_init(&g, gram, sizeof gram, (uint8_t *)grom, sizeof grom);
            if (machine_disc_attach(&g, 0, gimg, 0) == 0) {
                machine_run(&g, 3000000);
                uint32_t sum = mem_get32(gram + 0x100);      /* phys window 0x2000000 -> ram */
                uint32_t magic = mem_get32(gram + 0x104);
                check("guest-fdc-sum", sum == 4u * 255u * 256u / 2u, "1024 sector bytes summed");
                check("guest-fdc-magic", magic == 0x600D, "guest completed");
                check("guest-fdc-intrq-cleared", g.fdc.intrq == 0, "status reads cleared INTRQ");
                machine_disc_detach(&g, 0);
            } else {
                check("guest-attach", 0, "attach failed");
            }
        } else {
            check("guest-image", 0, "mkstemp/write failed");
        }
        unlink(gimg);
    }
    return test_summary("fdc");
}
