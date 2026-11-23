/*
 * edge_test.c - wide battery of edge-case tests across the core, bus and
 * machine models (review follow-up). Each test hand-encodes its instruction
 * words (no external assembler) or drives device registers directly.
 *
 * Run: ./edge_test   (via `make test`)
 * Licence: Apache-2.0 (see LICENSE).
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "test.h"
#include "machine.h"
#include "memc.h"
#include "ioc.h"
#include "vidc.h"

static char det[160];

/* ---------- instruction encoders ---------- */
#define MOV_IMM(rd, imm, rotf) (0xE3A00000u | ((rd) << 12) | ((rotf) << 8) | (imm))
#define MOVS_IMM(rd, imm, rotf) (0xE3B00000u | ((rd) << 12) | ((rotf) << 8) | (imm))
/* data processing, register op2: op2field = rs<<8 | 1<<4 | shift<<5 | rm */
#define DP_REG(op, S, rn, rd, rs, shift, rm) \
    (0xE0000000u | ((op) << 21) | ((S) << 20) | ((rn) << 16) | ((rd) << 12) | \
     ((rs) << 8) | (1u << 4) | ((shift) << 5) | (rm))

/* ---------- CPU core edges (flat-RAM rig) ---------- */

/* MULS sets N/Z only: V is unaffected, C is meaningless (left alone) */
static void t_muls_v_preserved(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = {
        0xE3A00440u, /* MOV r0,#0x40 ror 8 = 0x40000000 */
        0xE3A01440u, /* MOV r1,#0x40000000 */
        0xE1700001u, /* CMNS r0,r1 -> 0x80000000, V=1 */
        0xE0120190u, /* MULS r2,r0,r1 -> 0, Z=1 */
    };
    rig_load(&g, prog, 4, 0);
    arm26_run(&g.cpu, 4);
    snprintf(det, sizeof det, "r2=%08" PRIx32 " N=%d Z=%d V=%d", g.cpu.r[2],
             !!(g.cpu.r[15] & FLAG_N), !!(g.cpu.r[15] & FLAG_Z), !!(g.cpu.r[15] & FLAG_V));
    check("muls-v-preserved",
          g.cpu.r[2] == 0 && (g.cpu.r[15] & FLAG_V) && (g.cpu.r[15] & FLAG_Z) &&
          !(g.cpu.r[15] & FLAG_N), det);
}

/* shift-by-register with Rs=15: PSR bits zeroed (Rn/Rs rule), so n = (pc+8)&0xFF */
static void t_shift_rs15_psr_zero(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = {
        0xE3A02001u, /* MOV r2,#1 */
        0xE1A00F12u, /* MOV r0,r2,LSL r15  (at a=4: n=(4+8)&0xFF=12) */
    };
    rig_load(&g, prog, 2, 0);
    arm26_run(&g.cpu, 2);
    snprintf(det, sizeof det, "r0=%08" PRIx32, g.cpu.r[0]);
    check("shift-rs15-psr-zero", g.cpu.r[0] == 0x1000u, det);
}

/* register-shift edge amounts: LSL by 0 keeps C; ROR by 32 = unchanged, C=bit31;
 * LSR #0 (imm) means LSR #32; ROR #0 (imm) is RRX through C */
static void t_shift_reg_edges(void)
{
    {
        Rig g; rig_init(&g);
        uint32_t prog[] = {
            0xE3B004C0u, /* MOVS r0,#0xC0 ror 8 -> C=1 */
            0xE3A01055u, /* MOV r1,#0x55 */
            0xE3A02000u, /* MOV r2,#0 */
            0xE1B00211u, /* MOVS r0,r1,LSL r2 (n=0) */
        };
        rig_load(&g, prog, 4, 0);
        arm26_run(&g.cpu, 4);
        snprintf(det, sizeof det, "r0=%02" PRIx32 " C=%d", g.cpu.r[0], !!(g.cpu.r[15] & FLAG_C));
        check("lsl-reg0-keeps-C", g.cpu.r[0] == 0x55 && (g.cpu.r[15] & FLAG_C), det);
    }
    {
        Rig g; rig_init(&g);
        uint32_t prog[] = {
            0xE3A01480u, /* MOV r1,#0x80 ror 8 = 0x80000000 */
            0xE3A02020u, /* MOV r2,#32 */
            0xE1B00271u, /* MOVS r0,r1,ROR r2 (n=32) */
        };
        rig_load(&g, prog, 3, 0);
        arm26_run(&g.cpu, 3);
        snprintf(det, sizeof det, "r0=%08" PRIx32 " C=%d", g.cpu.r[0], !!(g.cpu.r[15] & FLAG_C));
        check("ror-reg32-unchanged", g.cpu.r[0] == 0x80000000u && (g.cpu.r[15] & FLAG_C), det);
    }
    {
        Rig g; rig_init(&g);
        uint32_t prog[] = {
            0xE3A01480u, /* MOV r1,#0x80000000 */
            0xE1B00021u, /* MOVS r0,r1,LSR #0 (=LSR #32) */
        };
        rig_load(&g, prog, 2, 0);
        arm26_run(&g.cpu, 2);
        snprintf(det, sizeof det, "r0=%08" PRIx32 " C=%d", g.cpu.r[0], !!(g.cpu.r[15] & FLAG_C));
        check("lsr-imm0-is-32", g.cpu.r[0] == 0 && (g.cpu.r[15] & FLAG_C), det);
    }
    {
        Rig g; rig_init(&g);
        uint32_t prog[] = {
            0xE3B004C0u, /* MOVS r0,#0xC0000000, C=1 */
            0xE3A01480u, /* MOV r1,#0x80000000 */
            0xE1B00061u, /* MOVS r0,r1,ROR #0 = RRX */
        };
        rig_load(&g, prog, 3, 0);
        arm26_run(&g.cpu, 3);
        snprintf(det, sizeof det, "r0=%08" PRIx32 " C=%d", g.cpu.r[0], !!(g.cpu.r[15] & FLAG_C));
        check("rrx-uses-C", g.cpu.r[0] == 0xC0000000u && !(g.cpu.r[15] & FLAG_C), det);
    }
}

/* STM with writeback and the base in the list: base stored unchanged when it
 * is the first (lowest-numbered) register in the list, modified (final) value
 * when stored second or later (Acorn PRM App. B) */
static void t_stm_base_position(void)
{
    {
        Rig g; rig_init(&g);
        uint32_t prog[] = {
            0xE3A00A01u, /* MOV r0,#0x1000 */
            0xE3A01A02u, /* MOV r1,#0x2000 */
            0xE8A00003u, /* STMIA r0!,{r0,r1} - r0 is lowest in list */
        };
        rig_load(&g, prog, 3, 0);
        arm26_run(&g.cpu, 3);
        uint32_t w0 = mem_rd32(&g.bus, 0x1000), w1 = mem_rd32(&g.bus, 0x1004);
        snprintf(det, sizeof det, "[1000]=%08" PRIx32 " [1004]=%08" PRIx32 " r0=%08" PRIx32,
                 w0, w1, g.cpu.r[0]);
        check("stm-base-first-stores-old",
              w0 == 0x1000u && w1 == 0x2000u && g.cpu.r[0] == 0x1008u, det);
    }
    {
        Rig g; rig_init(&g);
        uint32_t prog[] = {
            0xE3A00C11u, /* MOV r0,#0x1100 */
            0xE3A01A02u, /* MOV r1,#0x2000 */
            0xE8A10003u, /* STMIA r1!,{r0,r1} - base r1 is second in list */
        };
        rig_load(&g, prog, 3, 0);
        arm26_run(&g.cpu, 3);
        uint32_t w2 = mem_rd32(&g.bus, 0x2000), w3 = mem_rd32(&g.bus, 0x2004);
        snprintf(det, sizeof det, "[2000]=%08" PRIx32 " [2004]=%08" PRIx32 " r1=%08" PRIx32,
                 w2, w3, g.cpu.r[1]);
        check("stm-base-later-stores-final",
              w2 == 0x1100u && w3 == 0x2008u && g.cpu.r[1] == 0x2008u, det);
    }
}

/* LDRH with writeback and Rd==Rn: loaded value wins (same ARM2 rule as LDR) */
static void t_ldrh_rd_rn_load_wins(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = {
        0xE3A00040u, /* MOV r0,#0x40 */
        0xE1F000B2u, /* LDRH r0,[r0,#2]! */
    };
    rig_load(&g, prog, 2, 0);
    arm26_wr8(&g.cpu, 0x42, 0xEF);
    arm26_wr8(&g.cpu, 0x43, 0xBE);
    arm26_run(&g.cpu, 2);
    snprintf(det, sizeof det, "r0=%08" PRIx32, g.cpu.r[0]);
    check("ldrh-rd-rn-load-wins", g.cpu.r[0] == 0xBEEFu, det);
}

/* TST/TEQ/CMP/CMN with S=0 are undefined instructions on ARMv2 */
static void t_compare_s0_undef(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = { 0xE1000001u }; /* TST r0,r1 with S=0 */
    rig_load(&g, prog, 1, 0);
    arm26_run(&g.cpu, 1);
    snprintf(det, sizeof det, "pc=%08" PRIx32 " mode=%u I=%d lr=%08" PRIx32,
             g.cpu.r[15] & PC_MASK, g.cpu.r[15] & 3, !!(g.cpu.r[15] & FLAG_I), g.cpu.r[14]);
    check("tst-s0-undef",
          (g.cpu.r[15] & PC_MASK) == VEC_UNDEF && (g.cpu.r[15] & 3) == MODE_SVC &&
          (g.cpu.r[15] & FLAG_I) && (g.cpu.r[14] & PC_MASK) == 4, det);
}

/* coprocessor space (grp 6/7) traps as undefined on ARM2 */
static void t_cdp_undef(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = { 0xEE000010u }; /* CDP */
    rig_load(&g, prog, 1, 0);
    arm26_run(&g.cpu, 1);
    check("cdp-undef", (g.cpu.r[15] & PC_MASK) == VEC_UNDEF && (g.cpu.r[15] & 3) == MODE_SVC,
          "coprocessor instruction -> UNDEF vector");
}

/* NV condition never executes (26-bit ARM) */
static void t_nv_cond_nop(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = { 0xF3A00001u, 0xE3A00002u }; /* MOVNV r0,#1 ; MOV r0,#2 */
    rig_load(&g, prog, 2, 0);
    arm26_run(&g.cpu, 2);
    check("nv-cond-nop", g.cpu.r[0] == 2, "NV-conditioned instruction skipped");
}

/* BL sets lr = (bl+4) | old flags | old mode */
static void t_bl_lr(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = { 0xEB000000u }; /* BL +0 -> 0x08 */
    rig_load(&g, prog, 1, 0);
    arm26_run(&g.cpu, 1);
    snprintf(det, sizeof det, "lr=%08" PRIx32 " pc=%08" PRIx32, g.cpu.r[14], g.cpu.r[15] & PC_MASK);
    check("bl-lr", g.cpu.r[14] == (0x0C000000u | 4u | MODE_SVC) && (g.cpu.r[15] & PC_MASK) == 8, det);
}

/* STR pc stores the full CPSR (flags | pc+8 | mode) */
static void t_str_pc_cpsr(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = { 0xE3A00C01u, 0xE580F000u }; /* MOV r0,#0x100 ; STR pc,[r0] */
    rig_load(&g, prog, 2, 0);
    arm26_run(&g.cpu, 2);
    uint32_t w = mem_rd32(&g.bus, 0x100);
    snprintf(det, sizeof det, "[100]=%08" PRIx32, w);
    check("str-pc-stores-cpsr", w == (0x0C000000u | 0x0Cu | MODE_SVC), det);
}

/* LDM with pc and S=0 loads PC only; flags are preserved */
static void t_ldm_pc_no_flags(void)
{
    Rig g; rig_init(&g);
    g.cpu.r[15] |= FLAG_N;
    uint32_t prog[] = { 0xE3A0DC02u, 0xE8BD8000u }; /* MOV r13,#0x200 ; LDMIA r13!,{pc} */
    rig_load(&g, prog, 2, 0);
    arm26_wr32(&g.cpu, 0x200, 0x00000040u);
    arm26_run(&g.cpu, 2);
    snprintf(det, sizeof det, "pc=%08" PRIx32 " N=%d", g.cpu.r[15] & PC_MASK, !!(g.cpu.r[15] & FLAG_N));
    check("ldm-pc-keeps-flags", (g.cpu.r[15] & PC_MASK) == 0x40 && (g.cpu.r[15] & FLAG_N), det);
}

/* LDM^ with pc in a privileged mode restores the full CPSR (incl. mode) */
static void t_ldm_pc_caret_restores_cpsr(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = { 0xE3A0DC02u, 0xE8FD8000u }; /* MOV r13,#0x200 ; LDMIA r13!,{pc}^ */
    rig_load(&g, prog, 2, 0);
    arm26_wr32(&g.cpu, 0x200, 0x0C000000u | 0x40u | MODE_USR);
    arm26_run(&g.cpu, 2);
    snprintf(det, sizeof det, "pc=%08" PRIx32 " mode=%u", g.cpu.r[15] & PC_MASK, g.cpu.r[15] & 3);
    check("ldm-pc-caret-cpsr", (g.cpu.r[15] & PC_MASK) == 0x40 && (g.cpu.r[15] & 3) == MODE_USR, det);
}

/* LDR pc jumps, flags preserved */
static void t_ldr_pc_jump(void)
{
    Rig g; rig_init(&g);
    g.cpu.r[15] |= FLAG_N;
    uint32_t prog[] = { 0xE3A00C01u, 0xE590F000u }; /* MOV r0,#0x100 ; LDR pc,[r0] */
    rig_load(&g, prog, 2, 0);
    arm26_wr32(&g.cpu, 0x100, 0x00000080u);
    arm26_run(&g.cpu, 2);
    snprintf(det, sizeof det, "pc=%08" PRIx32 " N=%d", g.cpu.r[15] & PC_MASK, !!(g.cpu.r[15] & FLAG_N));
    check("ldr-pc-jump-keeps-flags", (g.cpu.r[15] & PC_MASK) == 0x80 && (g.cpu.r[15] & FLAG_N), det);
}

/* IRQ entry: lr_irq = interrupted pc+8 (SUBS PC,lr,#4 returns to next insn) */
static void t_irq_lr_offset(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = { 0xE3A00000u }; /* MOV r0,#0 at 0 */
    rig_load(&g, prog, 1, 0);
    g.cpu.r[15] &= ~FLAG_I;
    g.cpu.irqPending = 1;
    arm26_run(&g.cpu, 1);
    snprintf(det, sizeof det, "pc=%08" PRIx32 " mode=%u lr_irq=%08" PRIx32,
             g.cpu.r[15] & PC_MASK, g.cpu.r[15] & 3, g.cpu.r[14]);
    check("irq-lr-pc-plus-8",
          (g.cpu.r[15] & PC_MASK) == VEC_IRQ && (g.cpu.r[15] & 3) == MODE_IRQ &&
          g.cpu.r[14] == (FLAG_F | 8u | MODE_SVC), det);
}

/* ---------- machine / bus / MEMC edges ---------- */

static uint8_t edge_ram[0x400000];
static uint8_t edge_rom[0x1000];

/* unmapped logical page reads open bus; a CAM write maps it to DRAM */
static void t_unmapped_logical_then_cam(void)
{
    Machine m;
    machine_init(&m, edge_ram, sizeof edge_ram, edge_rom, sizeof edge_rom);
    memc_map_page(&m.memc, 0, 0); /* unlatch boot ROM, map logical page 0 -> phys 0 */
    uint32_t v = memc_logical_rd32(&m.memc, 0x00400000u); /* logical page 1024, unmapped */
    snprintf(det, sizeof det, "v=%08" PRIx32, v);
    check("unmapped-logical-open-bus", v == 0xFFFFFFFFu, det);
    memc_map_page(&m.memc, 1, 1); /* logical page 1 -> phys page 1 (0x02001000) */
    memc_logical_wr32(&m.memc, 0x1000, 0xDEADBEEFu);
    uint32_t p = mem_rd32(&m.bus, 0x02001000u);
    snprintf(det, sizeof det, "phys=%08" PRIx32, p);
    check("cam-map-routes-to-dram", p == 0xDEADBEEFu, det);
}

/* mapping a second logical page onto an occupied physical page unmaps the
 * previous holder (MEMC1a CAM aliasing rule) */
static void t_cam_remap_unmaps_holder(void)
{
    Machine m;
    machine_init(&m, edge_ram, sizeof edge_ram, edge_rom, sizeof edge_rom);
    memc_map_page(&m.memc, 0, 5);
    memc_map_page(&m.memc, 1, 5);
    snprintf(det, sizeof det, "pages[0]=%d pages[1]=%d", m.memc.pages[0], m.memc.pages[1]);
    check("cam-remap-unmaps-holder", m.memc.pages[0] == -1 && m.memc.pages[1] == 5, det);
}

/* the ROM image mirrors across the full 8MB window; a write to any mirror
 * programs the CAM and drops the boot-ROM latch */
static void t_rom_mirror_and_latch(void)
{
    Machine m;
    edge_rom[0] = 0x11;
    edge_rom[0xFFF] = 0x22;
    machine_init(&m, edge_ram, sizeof edge_ram, edge_rom, sizeof edge_rom);
    uint8_t a = mem_rd8(&m.bus, 0x03800000u + 0x1000u);      /* mirror of rom[0] */
    uint8_t b = mem_rd8(&m.bus, 0x03800000u + 0x1FFFu);      /* -> rom[0xFFF] */
    int latch_before = m.memc.latchrom;
    mem_wr8(&m.bus, 0x03801000u, 0x00u);                     /* write via mirror */
    snprintf(det, sizeof det, "a=%02x b=%02x latch %d->%d", a, b, latch_before, m.memc.latchrom);
    check("rom-mirror-reads", a == 0x11 && b == 0x22, det);
    check("rom-mirror-write-unlatches", latch_before == 1 && m.memc.latchrom == 0, det);
}

/* physical DRAM window beyond the installed RAM is open bus */
static void t_phys_open_bus_beyond_ram(void)
{
    Machine m;
    machine_init(&m, edge_ram, sizeof edge_ram, edge_rom, sizeof edge_rom);
    uint64_t ur0 = m.bus.unmapped_rd, uw0 = m.bus.unmapped_wr;
    uint8_t v = mem_rd8(&m.bus, 0x02400000u); /* 4MB installed: beyond -> open bus */
    mem_wr8(&m.bus, 0x02400000u, 0x55u);
    uint8_t v2 = mem_rd8(&m.bus, 0x02400000u);
    snprintf(det, sizeof det, "v=%02x v2=%02x +rd=%" PRIu64 " +wr=%" PRIu64,
             v, v2, m.bus.unmapped_rd - ur0, m.bus.unmapped_wr - uw0);
    check("phys-open-bus-beyond-ram",
          v == 0xFF && v2 == 0xFF && m.bus.unmapped_rd == ur0 + 2 && m.bus.unmapped_wr == uw0 + 1, det);
}

/* USR-mode instruction fetch from the podule window does NOT data-abort
 * (fetches are exempt); the open-bus word 0xFFFFFFFF decodes as NV = nop */
static void t_usr_fetch_podule_no_abort(void)
{
    Machine m;
    machine_init(&m, edge_ram, sizeof edge_ram, edge_rom, sizeof edge_rom);
    m.cpu.r[15] = FLAG_I | FLAG_F | MODE_USR | 0x03000000u;
    arm26_run(&m.cpu, 1);
    snprintf(det, sizeof det, "pc=%08" PRIx32 " dabts=%" PRIu64, m.cpu.r[15] & PC_MASK, m.bus.dabt_count);
    check("usr-fetch-podule-exempt",
          (m.cpu.r[15] & PC_MASK) == 0x03000004u && m.bus.dabt_count == 0, det);
}

/* SVC-mode data read of the podule window: no abort, open bus */
static void t_svc_podule_read_no_abort(void)
{
    Machine m;
    machine_init(&m, edge_ram, sizeof edge_ram, edge_rom, sizeof edge_rom);
    uint32_t v = arm26_rd32(&m.cpu, 0x03000000u);
    snprintf(det, sizeof det, "v=%08" PRIx32 " dabts=%" PRIu64, v, m.bus.dabt_count);
    check("svc-podule-read-open-bus", v == 0xFFFFFFFFu && m.bus.dabt_count == 0, det);
}

/* IOC timer0 fires into IRQ status A; the IRQ line follows status & mask */
static void t_ioc_timer_mask(void)
{
    Machine m;
    machine_init(&m, edge_ram, sizeof edge_ram, edge_rom, sizeof edge_rom);
    mem_wr8(&m.bus, IOC_BASE + IOC_IRQ_MASK_A, 0x00u);   /* mask all A IRQs */
    mem_wr8(&m.bus, IOC_BASE + IOC_T0_LO, 0x10u);        /* latch = 0x0010 */
    mem_wr8(&m.bus, IOC_BASE + IOC_T0_HI, 0x00u);
    mem_wr8(&m.bus, IOC_BASE + IOC_T0_GO, 0x00u);        /* start: cnt = 0x10 */
    ioc_tick(&m.ioc, &m.cpu, 4 * 0x20);                  /* 32 2MHz ticks */
    int fired_masked = (m.ioc.regs[IOC_IRQ_STATUS_A] & IOC_IRA_TIMER0) != 0;
    int line_masked = m.cpu.irqPending;
    mem_wr8(&m.bus, IOC_BASE + IOC_IRQ_MASK_A, 0xFFu);   /* unmask */
    ioc_tick(&m.ioc, &m.cpu, 4);
    snprintf(det, sizeof det, "statusA=%02x line(masked)=%d line(unmasked)=%d",
             m.ioc.regs[IOC_IRQ_STATUS_A], line_masked, m.cpu.irqPending);
    check("ioc-timer-fires-masked",
          fired_masked && !line_masked && m.cpu.irqPending, det);
}

/* VIDC: no valid CRTC mode -> no vsync flyback; valid mode -> flyback fires */
static void t_vidc_flyback_modes(void)
{
    Machine m;
    machine_init(&m, edge_ram, sizeof edge_ram, edge_rom, sizeof edge_rom);
    vidc_tick(&m.vidc, 10000000u);
    int fb0 = (m.ioc.regs[IOC_IRQ_STATUS_A] & IOC_IRA_FLYBACK) != 0;
    for (int r = 0; r < 16; r++)
        if (r == VIDC_CRTC_HCR || r == VIDC_CRTC_HBSR || r == VIDC_CRTC_HBER ||
            r == VIDC_CRTC_VCR || r == VIDC_CRTC_VBSR || r == VIDC_CRTC_VBER)
            vidc_write(&m.vidc, (VIDC_CRTC_REG(r) << 24) | (100u << 14));
    vidc_tick(&m.vidc, 1000000u);
    int fb1 = (m.ioc.regs[IOC_IRQ_STATUS_A] & IOC_IRA_FLYBACK) != 0;
    snprintf(det, sizeof det, "fb(no mode)=%d fb(valid mode)=%d", fb0, fb1);
    check("vidc-flyback-only-with-valid-mode", !fb0 && fb1, det);
}

/* LDM^ with pc switching mode: the W writeback must land in the OLD mode's
 * base register before the pc load re-banks r8-r14 (ROM300 boot derail). */
static void t_ldm_pc_caret_writeback_old_bank(void)
{
    Rig g; rig_init(&g); /* SVC mode after reset */
    uint32_t prog[] = { 0xE8FD8007u }; /* LDMIA sp!, {r0,r1,r2,pc}^ */
    rig_load(&g, prog, 1, 0);
    g.cpu.r[13] = 0x1000;          /* SVC sp (live) */
    g.cpu.bankUsr[5] = 0x5000;     /* USR sp waiting in the bank */
    arm26_wr32(&g.cpu, 0x1000, 0x111u);
    arm26_wr32(&g.cpu, 0x1004, 0x222u);
    arm26_wr32(&g.cpu, 0x1008, 0x333u);
    arm26_wr32(&g.cpu, 0x100c, 0x2000u | MODE_USR);
    arm26_run(&g.cpu, 1);
    snprintf(det, sizeof det, "mode=%u sp=%08" PRIx32 " svc_sp=%08" PRIx32 " pc=%08" PRIx32,
             g.cpu.r[15] & 3, g.cpu.r[13], g.cpu.bankSvc[0], g.cpu.r[15] & PC_MASK);
    check("ldm-pc-caret-writeback-old-bank",
          (g.cpu.r[15] & 3) == MODE_USR && g.cpu.r[13] == 0x5000 &&
          g.cpu.bankSvc[0] == 0x1010 && (g.cpu.r[15] & PC_MASK) == 0x2000, det);
}

/* SUBS PC,lr,#4 (IRQ return): the mode bits in the result must re-bank
 * r8-r14, so SVC sp/lr come back (ROM300 boot derail). */
static void t_subs_pc_rebanks(void)
{
    Rig g; rig_init(&g); /* SVC */
    uint32_t prog[] = { 0xE1A00000u, 0xE25EF004u }; /* NOP ; SUBS pc,lr,#4 */
    rig_load(&g, prog, 2, 0);
    g.cpu.r[13] = 0x8000;           /* SVC sp */
    g.cpu.r[14] = 0x1234;           /* SVC lr */
    g.cpu.r[15] &= ~FLAG_I;
    g.cpu.irqPending = 1;
    arm26_run(&g.cpu, 1);           /* NOP, then IRQ exception -> IRQ mode */
    snprintf(det, sizeof det, "pre mode=%u", g.cpu.r[15] & 3);
    check("subs-pc-rebank-enter-irq", (g.cpu.r[15] & 3) == MODE_IRQ, det);
    g.cpu.r[13] = 0x9999;           /* IRQ sp (banked, live) */
    g.cpu.r[14] = 0x100u | MODE_SVC; /* return to SVC at 0x100, I clear */
    rig_setpc(&g, 4);
    arm26_run(&g.cpu, 1);           /* SUBS pc,lr,#4 */
    snprintf(det, sizeof det, "mode=%u sp=%08" PRIx32 " lr=%08" PRIx32 " pc=%08" PRIx32,
             g.cpu.r[15] & 3, g.cpu.r[13], g.cpu.r[14], g.cpu.r[15] & PC_MASK);
    check("subs-pc-rebank",
          (g.cpu.r[15] & 3) == MODE_SVC && g.cpu.r[13] == 0x8000 &&
          g.cpu.r[14] == 0x1234 && (g.cpu.r[15] & PC_MASK) == 0xFC, det);
}

int main(void)
{
    t_muls_v_preserved();
    t_shift_rs15_psr_zero();
    t_shift_reg_edges();
    t_stm_base_position();
    t_ldrh_rd_rn_load_wins();
    t_compare_s0_undef();
    t_cdp_undef();
    t_nv_cond_nop();
    t_bl_lr();
    t_str_pc_cpsr();
    t_ldm_pc_no_flags();
    t_ldm_pc_caret_restores_cpsr();
    t_ldr_pc_jump();
    t_irq_lr_offset();
    t_unmapped_logical_then_cam();
    t_cam_remap_unmaps_holder();
    t_rom_mirror_and_latch();
    t_phys_open_bus_beyond_ram();
    t_usr_fetch_podule_no_abort();
    t_svc_podule_read_no_abort();
    t_ioc_timer_mask();
    t_vidc_flyback_modes();
    t_ldm_pc_caret_writeback_old_bank();
    t_subs_pc_rebanks();
    return test_summary("edge") ? 1 : 0;
}
