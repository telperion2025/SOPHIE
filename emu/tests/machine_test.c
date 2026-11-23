/*
 * machine_test.c - WP3 acceptance: Archimedes base machine with a
 * hand-built dummy ROM.
 *
 * Acceptance (spec section 10, WP3): the dummy ROM boots far enough to
 * read/write emulated memory and receive timers; an IRQ-driven CPU loop
 * runs for 1M cycles without lockup. Also covers VIDC framebuffer render
 * and the IOC keyboard path.
 *
 * Run: ./machine_test   (via `make test`)
 * Licence: Apache-2.0 (see LICENSE).
 */
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>

#include "test.h"
#include "machine.h"

/* ---------- tiny hand-encoder for the dummy ROM ---------- */

/* encode v as an ARM imm12 (imm8 ROR 2*rot); aborts if not encodable */
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
    fprintf(stderr, "A_IMM12: 0x%" PRIx32 " not encodable as imm8:rot\n", v);
    abort();
}

static uint32_t A_MOV_IMM(int rd, uint32_t imm)  { return 0xE3A00000u | ((uint32_t)rd << 12) | A_IMM12(imm); }
static uint32_t A_MOV_LSL(int rd, int rm, int n)  { return 0xE1A00000u | ((uint32_t)rd << 12) | ((uint32_t)(n & 31) << 7) | (uint32_t)rm; }
static uint32_t A_ORR_IMM(int rd, int rn, uint32_t imm) { return 0xE3800000u | ((uint32_t)rn << 16) | ((uint32_t)rd << 12) | A_IMM12(imm); }
static uint32_t A_ORR_REG(int rd, int rn, int rm) { return 0xE1800000u | ((uint32_t)rn << 16) | ((uint32_t)rd << 12) | (uint32_t)rm; }
static uint32_t A_ADD_IMM(int rd, int rn, uint32_t imm) { return 0xE2800000u | ((uint32_t)rn << 16) | ((uint32_t)rd << 12) | A_IMM12(imm); }
static uint32_t A_CMP_R(int rn, int rm)           { return 0xE1500000u | ((uint32_t)rn << 16) | (uint32_t)rm; }
static uint32_t A_B(uint32_t pc, uint32_t target) { return 0xEA000000u | (((target - (pc + 8)) >> 2) & 0xFFFFFFu); }
static uint32_t A_BNE(uint32_t pc, uint32_t target) { return 0x1A000000u | (((target - (pc + 8)) >> 2) & 0xFFFFFFu); }
static uint32_t A_BEQ(uint32_t pc, uint32_t target) { return 0x0A000000u | (((target - (pc + 8)) >> 2) & 0xFFFFFFu); }
static uint32_t A_STR(int rd, int rn, uint32_t off)  { return 0xE5800000u | ((uint32_t)rn << 16) | ((uint32_t)rd << 12) | (off & 0xFFFu); }
static uint32_t A_LDR(int rd, int rn, uint32_t off)  { return 0xE5900000u | ((uint32_t)rn << 16) | ((uint32_t)rd << 12) | (off & 0xFFFu); }
static uint32_t A_STRB(int rd, int rn, uint32_t off) { return 0xE5C00000u | ((uint32_t)rn << 16) | ((uint32_t)rd << 12) | (off & 0xFFFu); }
static uint32_t A_LDRB(int rd, int rn, uint32_t off) { return 0xE5D00000u | ((uint32_t)rn << 16) | ((uint32_t)rd << 12) | (off & 0xFFFu); }
static uint32_t A_TST_IMM(int rn, uint32_t imm)    { return 0xE1100000u | ((uint32_t)rn << 16) | A_IMM12(imm); }
static uint32_t A_LDMIA(int rn, uint32_t list) { return 0xE8900000u | ((uint32_t)rn << 16) | (list & 0xFFFFu); }
static uint32_t A_STMIA(int rn, uint32_t list) { return 0xE8800000u | ((uint32_t)rn << 16) | (list & 0xFFFFu); }
static uint32_t A_TEQP(int rn) { return 0xE330F000u | ((uint32_t)rn << 16); }
#define A_MOVS_PC_R14 0xE1B0F00Eu
#define A_SPIN        0xEAFFFFFEu

#define ROM_BASE_ADDR 0x03800000u
#define ROM_WORDS     0x200u   /* 2KB dummy ROM image */

#define W_START  0x40u         /* byte 0x100: init code */
#define W_IRQ    0x80u         /* byte 0x200: IRQ handler */
#define W_FAIL   0xC0u         /* byte 0x300: fail spin */
#define W_POOL   0xB0u         /* byte 0x2C0: literal pool */

/* MEMC control word: register 7 tag 0x03600000, 32K pages (bits 3:2=3), OS mode (bit 12) */
#define MEMC_CTRL_WORD (0x03600000u | (7u << 17) | (3u << 2) | (1u << 12))

static void build_dummy_rom(uint32_t *rom)
{
    memset(rom, 0, ROM_WORDS * 4);
    /* Vectors live at logical 0x00-0x1C (ROM-mapped at reset), so branch
     * offsets are encoded for those addresses, targeting the absolute ROM
     * window 0x03800000+ where the code runs. */
    rom[0x00] = A_B(0x00000000u, ROM_BASE_ADDR + W_START * 4);
    for (int i = 1; i < 8; i++)
        rom[i] = A_SPIN;
    rom[0x06] = A_B(0x00000018u, ROM_BASE_ADDR + W_IRQ * 4); /* IRQ vector */

    uint32_t *c = rom + W_START;
    uint32_t pc = ROM_BASE_ADDR + W_START * 4;
    /* r13 = 0x00100000 (SVC stack in RAM) */
    *c++ = A_MOV_IMM(13, 1);        pc += 4;
    *c++ = A_MOV_LSL(13, 13, 20);   pc += 4;
    /* MEMC control: 32K pages + OS mode, via literal pool */
    *c++ = A_LDR(0, 15, (ROM_BASE_ADDR + W_POOL * 4) - (pc + 8)); pc += 4; /* r0 = ctrl word */
    *c++ = A_MOV_IMM(2, 0x36);       pc += 4;
    *c++ = A_MOV_LSL(2, 2, 20);     pc += 4;   /* r2 = 0x03600000 */
    *c++ = A_STR(0, 2, 0);          pc += 4;   /* write control register */
    /* CAM: logical page 0 -> physical page 0 (data word 0); clears ROM latch */
    *c++ = A_MOV_IMM(3, 0x38);       pc += 4;
    *c++ = A_MOV_LSL(3, 3, 20);     pc += 4;   /* r3 = 0x03800000 (ROM window = CAM writes) */
    *c++ = A_MOV_IMM(1, 0);         pc += 4;
    *c++ = A_STR(1, 3, 0);          pc += 4;
    /* copy vectors ROM[0..0x1C] -> logical 0x00 (now DRAM via the CAM) */
    *c++ = A_MOV_IMM(0, 0x38);       pc += 4;
    *c++ = A_MOV_LSL(0, 0, 20);     pc += 4;   /* r0 = 0x03800000 */
    *c++ = A_MOV_IMM(1, 0);         pc += 4;   /* r1 = 0 */
    *c++ = A_LDMIA(0, 0x03FCu);      pc += 4;   /* {r2-r9} */
    *c++ = A_STMIA(1, 0x03FCu);      pc += 4;
    rom[W_POOL] = MEMC_CTRL_WORD;
    /* memory test: store r13 at 0x1000, read back, compare */
    *c++ = A_MOV_IMM(3, 1);         pc += 4;
    *c++ = A_MOV_LSL(3, 3, 12);     pc += 4;   /* r3 = 0x1000 */
    *c++ = A_STR(13, 3, 0);         pc += 4;
    *c++ = A_LDR(4, 3, 0);          pc += 4;
    *c++ = A_CMP_R(4, 13);          pc += 4;
    *c++ = A_BNE(pc, ROM_BASE_ADDR + W_FAIL * 4); pc += 4;
    /* success marker: [0x3000] = 2 */
    *c++ = A_MOV_IMM(3, 3);         pc += 4;
    *c++ = A_MOV_LSL(3, 3, 12);     pc += 4;   /* r3 = 0x3000 */
    *c++ = A_MOV_IMM(4, 2);         pc += 4;
    *c++ = A_STR(4, 3, 0);          pc += 4;
    /* IOC: r2 = 0x03200000; timer0 latch 0x00FF, GO; ack; masks: A=timer0|flyback, B=KART Rx */
    *c++ = A_MOV_IMM(2, 0x32);      pc += 4;
    *c++ = A_MOV_LSL(2, 2, 20);     pc += 4;   /* r2 = 0x03200000 */
    *c++ = A_MOV_IMM(5, 0xFF);      pc += 4;
    *c++ = A_STRB(5, 2, 0x40);      pc += 4;   /* T0 latch lo */
    *c++ = A_MOV_IMM(5, 0);         pc += 4;
    *c++ = A_STRB(5, 2, 0x44);      pc += 4;   /* T0 latch hi */
    *c++ = A_STRB(5, 2, 0x48);      pc += 4;   /* T0 GO */
    *c++ = A_MOV_IMM(5, 0x28);      pc += 4;
    *c++ = A_STRB(5, 2, 0x14);      pc += 4;   /* ack IRQ request A (timer0|flyback) */
    *c++ = A_STRB(5, 2, 0x18);      pc += 4;   /* IRQ mask A = timer0|flyback */
    *c++ = A_MOV_IMM(5, 0x80);      pc += 4;
    *c++ = A_STRB(5, 2, 0x28);      pc += 4;   /* IRQ mask B = KART Rx full */
    /* enable IRQs: CPSR := F|SVC|pc (full replace via TEQP, PC preserved).
     * The PC field is patched in after the block layout is known. */
    *c++ = A_MOV_IMM(0, 0x38);      pc += 4;
    *c++ = A_MOV_LSL(0, 0, 20);     pc += 4;   /* r0 = 0x03800000 */
    int i_add1 = (int)(c - rom); *c++ = A_ADD_IMM(0, 0, 0); pc += 4; /* PC field hi */
    int i_add2 = (int)(c - rom); *c++ = A_ADD_IMM(0, 0, 0); pc += 4; /* PC field lo */
    *c++ = A_MOV_IMM(1, 1);         pc += 4;
    *c++ = A_MOV_LSL(1, 1, 26);     pc += 4;   /* r1 = 0x04000000 (F) */
    *c++ = A_ORR_REG(0, 0, 1);      pc += 4;
    *c++ = A_ORR_IMM(0, 0, 3);      pc += 4;   /* mode SVC */
    *c++ = A_TEQP(0);               pc += 4;   /* CPSR := r0, I cleared */
    uint32_t off = pc - ROM_BASE_ADDR;         /* address after TEQP, relative */
    rom[i_add1] = A_ADD_IMM(0, 0, off & 0x300u);
    rom[i_add2] = A_ADD_IMM(0, 0, off & 0xFFu);
    /* main loop: [0x2000]++ forever */
    *c++ = A_MOV_IMM(5, 2);         pc += 4;
    *c++ = A_MOV_LSL(5, 5, 12);     pc += 4;   /* r5 = 0x2000 */
    uint32_t loop_pc = pc;
    *c++ = A_LDR(4, 5, 0);          pc += 4;
    *c++ = A_ADD_IMM(4, 4, 1);      pc += 4;
    *c++ = A_STR(4, 5, 0);          pc += 4;
    *c++ = A_B(pc, loop_pc);        pc += 4;

    /* IRQ handler: [0x2004]++ (all IRQs); if IRQA&0x08 [0x2008]++ (flyback);
     * if IRB&0x80 [0x200C]++ (KART Rx); ack A=0x28, B=0x80; MOVS pc,r14 */
    c = rom + W_IRQ;
    pc = ROM_BASE_ADDR + W_IRQ * 4;
    *c++ = A_MOV_IMM(6, 2);         pc += 4;
    *c++ = A_MOV_LSL(6, 6, 12);     pc += 4;   /* r6 = 0x2000 */
    *c++ = A_LDR(4, 6, 4);          pc += 4;
    *c++ = A_ADD_IMM(4, 4, 1);      pc += 4;
    *c++ = A_STR(4, 6, 4);          pc += 4;   /* [0x2004]++ */
    *c++ = A_MOV_IMM(2, 0x32);      pc += 4;
    *c++ = A_MOV_LSL(2, 2, 20);     pc += 4;   /* r2 = 0x03200000 */
    *c++ = A_LDRB(5, 2, 0x10);      pc += 4;   /* r5 = IRQ_STATUS_A */
    *c++ = A_TST_IMM(5, 0x08);      pc += 4;
    int i_beq1 = (int)(c - rom); *c++ = A_BEQ(pc, pc + 16); pc += 4; /* skip increment if bit clear */
    *c++ = A_LDR(4, 6, 8);          pc += 4;
    *c++ = A_ADD_IMM(4, 4, 1);      pc += 4;
    *c++ = A_STR(4, 6, 8);          pc += 4;   /* [0x2008]++ */
    rom[i_beq1] = A_BEQ(ROM_BASE_ADDR + i_beq1 * 4, pc);
    *c++ = A_LDRB(5, 2, 0x20);      pc += 4;   /* r5 = IRQ_STATUS_B */
    *c++ = A_TST_IMM(5, 0x80);      pc += 4;
    int i_beq2 = (int)(c - rom); *c++ = A_BEQ(pc, pc + 16); pc += 4;
    *c++ = A_LDR(4, 6, 12);         pc += 4;
    *c++ = A_ADD_IMM(4, 4, 1);      pc += 4;
    *c++ = A_STR(4, 6, 12);         pc += 4;   /* [0x200C]++ */
    rom[i_beq2] = A_BEQ(ROM_BASE_ADDR + i_beq2 * 4, pc);
    *c++ = A_MOV_IMM(5, 0x28);      pc += 4;
    *c++ = A_STRB(5, 2, 0x14);      pc += 4;   /* ack A: timer0|flyback */
    *c++ = A_MOV_IMM(5, 0x80);      pc += 4;
    *c++ = A_STRB(5, 2, 0x24);      pc += 4;   /* ack B: KART Rx full */
    *c++ = A_MOVS_PC_R14;           pc += 4;

    /* fail: [0x3000] = 1, spin */
    c = rom + W_FAIL;
    *c++ = A_MOV_IMM(3, 3);
    *c++ = A_MOV_LSL(3, 3, 12);     /* r3 = 0x3000 */
    *c++ = A_MOV_IMM(4, 1);
    *c++ = A_STR(4, 3, 0);
    *c++ = A_SPIN;
}

int main(void)
{
    static uint8_t ram[0x400000];   /* 4MB DRAM */
    static uint32_t rom[ROM_WORDS];
    build_dummy_rom(rom);

    Machine m;
    machine_init(&m, ram, sizeof ram, (uint8_t *)rom, ROM_WORDS * 4);

    char det[160];

    /* WP3 acceptance: boot the dummy ROM, run an IRQ-driven loop 1M cycles */
    machine_run(&m, 1000000);

    snprintf(det, sizeof det, "latchrom=%d pagesize=%d pages[0]=%d ctrl=%08" PRIx32,
             m.memc.latchrom, m.memc.pagesize_idx, m.memc.pages[0], m.memc.ctrl);
    check("memc-cam-programmed",
          m.memc.latchrom == 0 && m.memc.pagesize_idx == 3 && m.memc.pages[0] == 0, det);

    uint32_t vec = mem_rd32(&m.bus, 0x18);
    uint32_t vec_expect = A_B(0x00000018u, ROM_BASE_ADDR + W_IRQ * 4);
    snprintf(det, sizeof det, "vec[0x18]=%" PRIx32 " expect=%" PRIx32, vec, vec_expect);
    check("vectors-copied-to-ram", vec == vec_expect, det);

    uint32_t marker = mem_rd32(&m.bus, 0x3000);
    snprintf(det, sizeof det, "marker=%" PRIu32 " (2=mem test passed, 1=failed)", marker);
    check("memory-read-write", marker == 2, det);

    uint32_t loopctr = mem_rd32(&m.bus, 0x2000);
    snprintf(det, sizeof det, "loop counter=%" PRIu32, loopctr);
    check("main-loop-progress", loopctr > 100, det);

    uint32_t irqctr = mem_rd32(&m.bus, 0x2004);
    snprintf(det, sizeof det, "irq counter=%" PRIu32, irqctr);
    check("timer-irqs-delivered", irqctr >= 100, det);

    snprintf(det, sizeof det, "cycles=%" PRIu64 " halted=%d", m.cpu.cycles, m.cpu.halted);
    check("ran-1M-cycles-no-lockup", m.cpu.cycles == 1000000 && !m.cpu.halted, det);

    uint32_t phys = mem_rd32(&m.bus, 0x02000000u + 0x3000);
    snprintf(det, sizeof det, "phys alias=%" PRIu32, phys);
    check("physical-window-alias", phys == 2, det);

    uint32_t rom0 = mem_rd32(&m.bus, ROM_BASE_ADDR);
    check("rom-window-readable", rom0 == A_B(0x00000000u, ROM_BASE_ADDR + W_START * 4), "reset vector in ROM");

    /* the ROM image mirrors across the full 8MB window (0x03800000-0x03FFFFFF):
     * 0x03C00100 aliases ROM offset 0x100 (2KB dummy ROM) */
    uint32_t mir = mem_rd32(&m.bus, 0x03C00100u);
    snprintf(det, sizeof det, "mirror=%08" PRIx32 " expect=%08" PRIx32, mir, rom[0x40]);
    check("rom-window-mirrored", mir == rom[0x40], det);

    uint8_t irqmask = mem_rd8(&m.bus, 0x03200000u + 0x18);
    snprintf(det, sizeof det, "IRQ_MASK_A=%" PRIx32, irqmask);
    check("ioc-irq-mask", irqmask == 0x28, det);

    /* VIDC vsync: program a CRTC mode (8 MHz pixel clock), then count the
     * flyback IRQs the dummy-ROM handler tallies in [0x2008] */
    mem_wr32(&m.bus, VIDC_BASE, (VIDC_REG_CONTROL << 24) | (2u << 2)); /* 4bpp, pixel clock 0 */
    mem_wr32(&m.bus, VIDC_BASE, (VIDC_CRTC_REG(VIDC_CRTC_HCR)  << 24) | (100u << 14));
    mem_wr32(&m.bus, VIDC_BASE, (VIDC_CRTC_REG(VIDC_CRTC_HBSR) << 24) | (10u << 14));
    mem_wr32(&m.bus, VIDC_BASE, (VIDC_CRTC_REG(VIDC_CRTC_HBER) << 24) | (90u << 14));
    mem_wr32(&m.bus, VIDC_BASE, (VIDC_CRTC_REG(VIDC_CRTC_VCR)  << 24) | (312u << 14));
    mem_wr32(&m.bus, VIDC_BASE, (VIDC_CRTC_REG(VIDC_CRTC_VBSR) << 24) | (5u << 14));
    mem_wr32(&m.bus, VIDC_BASE, (VIDC_CRTC_REG(VIDC_CRTC_VBER) << 24) | (300u << 14));
    uint32_t irq0 = mem_rd32(&m.bus, 0x2004);
    machine_run(&m, 500000);
    uint32_t vsc = mem_rd32(&m.bus, 0x2008);
    uint32_t irq1 = mem_rd32(&m.bus, 0x2004);
    snprintf(det, sizeof det, "vsync-irqs=%" PRIu32 " total %" PRIu32 "->%" PRIu32, vsc, irq0, irq1);
    check("vsync-irqs-delivered", vsc > 0 && irq1 > irq0, det);

    /* forced vsync: raise the flyback line directly, no frame wait - IRQA
     * bit 3 must be set immediately and the handler must tally it */
    uint32_t vsc0 = mem_rd32(&m.bus, 0x2008);
    ioc_flyback(&m.ioc);
    uint8_t sta = mem_rd8(&m.bus, 0x03200000u + IOC_IRQ_STATUS_A);
    snprintf(det, sizeof det, "IRQA=%" PRIx32, sta);
    check("vsync-flyback-bit-set", (sta & IOC_IRA_FLYBACK) != 0, det);
    machine_run(&m, 10000);
    uint32_t vsc1 = mem_rd32(&m.bus, 0x2008);
    snprintf(det, sizeof det, "vsync-irqs=%" PRIu32 "->%" PRIu32, vsc0, vsc1);
    check("vsync-irq-forced-delivered", vsc1 > vsc0, det);

    /* keyboard: queue a byte, IRQ B Rx-full set and delivered to the handler
     * ([0x200C]); reading KART returns the byte and clears Rx-full */
    ioc_key_press(&m.ioc, 'A');
    uint8_t stb = mem_rd8(&m.bus, 0x03200000u + IOC_IRQ_STATUS_B);
    snprintf(det, sizeof det, "IRB=%" PRIx32, stb);
    check("kart-rx-full", (stb & IOC_IRB_KART_RX_FULL) != 0, det);
    machine_run(&m, 100000);
    uint32_t kc = mem_rd32(&m.bus, 0x200C);
    snprintf(det, sizeof det, "kart-irqs=%" PRIu32, kc);
    check("kart-irq-delivered", kc > 0, det);
    uint8_t key = mem_rd8(&m.bus, 0x03200000u + IOC_KART);
    stb = mem_rd8(&m.bus, 0x03200000u + IOC_IRQ_STATUS_B);
    snprintf(det, sizeof det, "key=%c IRB=%" PRIx32, key, stb);
    check("kart-read-clears", key == 'A' && !(stb & IOC_IRB_KART_RX_FULL), det);

    /* VIDC: program a small 4bpp mode and render a frame */
    mem_wr32(&m.bus, VIDC_BASE, (VIDC_REG_CONTROL << 24) | 8u);   /* bpp mode 2 (4bpp) */
    mem_wr32(&m.bus, VIDC_BASE, (0x00u << 24) | 0xF00u);           /* palette 0 = blue */
    mem_wr32(&m.bus, VIDC_BASE, (0x14u << 24) | 0x00Fu);           /* palette 5 = red */
    mem_wr32(&m.bus, VIDC_BASE, (VIDC_CRTC_REG(VIDC_CRTC_HCR)  << 24) | (170u << 14));
    mem_wr32(&m.bus, VIDC_BASE, (VIDC_CRTC_REG(VIDC_CRTC_HBSR) << 24) | (50u << 14));
    mem_wr32(&m.bus, VIDC_BASE, (VIDC_CRTC_REG(VIDC_CRTC_HDSR) << 24) | (60u << 14));
    mem_wr32(&m.bus, VIDC_BASE, (VIDC_CRTC_REG(VIDC_CRTC_HDER) << 24) | (140u << 14));
    mem_wr32(&m.bus, VIDC_BASE, (VIDC_CRTC_REG(VIDC_CRTC_HBER) << 24) | (160u << 14));
    mem_wr32(&m.bus, VIDC_BASE, (VIDC_CRTC_REG(VIDC_CRTC_VCR)  << 24) | (200u << 14));
    mem_wr32(&m.bus, VIDC_BASE, (VIDC_CRTC_REG(VIDC_CRTC_VBSR) << 24) | (20u << 14));
    mem_wr32(&m.bus, VIDC_BASE, (VIDC_CRTC_REG(VIDC_CRTC_VDSR) << 24) | (30u << 14));
    mem_wr32(&m.bus, VIDC_BASE, (VIDC_CRTC_REG(VIDC_CRTC_VDER) << 24) | (100u << 14));
    mem_wr32(&m.bus, VIDC_BASE, (VIDC_CRTC_REG(VIDC_CRTC_VBER) << 24) | (190u << 14));
    for (uint32_t p = 0; p <= 2; p++) memc_map_page(&m.memc, p, p); /* cover logical 0x10000 */
    mem_wr32(&m.bus, MEMC_BASE, 0x03600000u | (1u << 17) | ((0x10000u / 16) << 2)); /* video start reg = 0x10000 */
    mem_wr8(&m.bus, 0x10000u, 0x05u);                              /* first pixel byte */

    static uint32_t fb[400 * 300];
    vidc_render(&m.vidc, &m.bus, fb, 400, 300);
    /* decoded: xstart = HDSR-HBSR = (60*2+7)-(50*2+1) = 26 ; ystart = VDSR-VBSR = 31-21 = 10
     * pen byte 0x05, 4bpp: low nibble 5 -> red at x=26, high nibble 0 -> blue at x=27 */
    uint32_t p0 = fb[10 * 400 + 26];
    uint32_t p1 = fb[10 * 400 + 27];
    snprintf(det, sizeof det, "fb[10,26]=%" PRIx32, p0);
    check("vidc-render-pixel-red", p0 == 0xFF0000u, det);
    snprintf(det, sizeof det, "fb[10,27]=%" PRIx32, p1);
    check("vidc-render-pixel-blue", p1 == 0x0000FFu, det);

    return test_summary("machine") ? 1 : 0;
}
