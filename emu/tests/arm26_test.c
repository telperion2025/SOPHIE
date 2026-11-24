/*
 * arm26_test.c - semantics tests for the ARMv2/3 26-bit interpreter core.
 *
 * WP1: verbatim transplant of the 42-assertion JS suite (spec Appendix B).
 * Each test hand-encodes its own instruction words (no external assembler)
 * and asserts registers, flags, memory, and exception-entry state.
 * Traceability: test names match the JS suite one-to-one; see
 * docs/reference-manual.md for the spec-section mapping.
 *
 * Run: ./arm26_test   (via `make test`)
 * Licence: Apache-2.0 (see LICENSE).
 */
#include <stdio.h>
#include <inttypes.h>

#include "test.h"

#define FLAGS_STR(f) "N%" PRIx32 " Z%" PRIx32 " C%" PRIx32 " V%" PRIx32, \
    !!((f) & FLAG_N), !!((f) & FLAG_Z), !!((f) & FLAG_C), !!((f) & FLAG_V)

static char det[128];

/* 1. MOV immediate */
static void t_mov_imm(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = { 0xE3A00011u };            /* MOV r0, #0x11 */
    rig_load(&g, prog, 1, 0);
    arm26_run(&g.cpu, 1);
    snprintf(det, sizeof det, "r0=%" PRIx32, g.cpu.r[0]);
    check("mov-imm", g.cpu.r[0] == 0x11, det);
}

/* 2. MOV with rotate */
static void t_mov_rot_imm(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = { 0xE3A044FFu };            /* MOV r4, #0xFF000000 (imm8 0xFF, rot 8) */
    rig_load(&g, prog, 1, 0);
    arm26_run(&g.cpu, 1);
    snprintf(det, sizeof det, "r4=%" PRIx32, g.cpu.r[4]);
    check("mov-rot-imm", g.cpu.r[4] == 0xFF000000u, det);
}

/* 3. ADD reg + imm */
static void t_add(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = { 0xE3A00005u, 0xE2801005u }; /* MOV r0,#5 ; ADD r1,r0,#5 */
    rig_load(&g, prog, 2, 0);
    arm26_run(&g.cpu, 2);
    snprintf(det, sizeof det, "r1=%" PRIu32, g.cpu.r[1]);
    check("add", g.cpu.r[1] == 10, det);
}

/* 4. SUB reg & flags */
static void t_sub_neg_flags(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = { 0xE0402000u };            /* SUB r2, r0, r0 */
    rig_load(&g, prog, 1, 0);
    g.cpu.r[0] = 0x12345678u;
    arm26_run(&g.cpu, 1);
    snprintf(det, sizeof det, "r2=%" PRIx32 " C=%d (expect borrow -> C clear)",
             g.cpu.r[2], !!(g.cpu.r[15] & FLAG_C));
    check("sub-neg-flags", g.cpu.r[2] == 0 && !(g.cpu.r[15] & FLAG_C), det);
}

/* 5. branch taken */
static void t_branch_taken(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = {
        0xE3A00000u,  /* 0x00 MOV r0,#0 */
        0xE3500000u,  /* 0x04 CMP r0,#0 -> Z */
        0x0A000000u,  /* 0x08 BEQ +0 -> target = 0x08+8+0 = 0x10 (skips MOV r1 at 0x0C) */
        0xE3A01001u,  /* 0x0C MOV r1,#1 (skipped) */
        0xE3A02002u,  /* 0x10 MOV r2,#2 */
    };
    rig_load(&g, prog, 5, 0);
    arm26_run(&g.cpu, 4);
    snprintf(det, sizeof det, "r1=%" PRIu32 " r2=%" PRIu32 " (r1 must be skipped)", g.cpu.r[1], g.cpu.r[2]);
    check("branch-eq-taken", g.cpu.r[1] == 0 && g.cpu.r[2] == 2, det);
}

/* 6. branch not taken */
static void t_branch_not_taken(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = {
        0xE3A00000u,  /* MOV r0,#0 */
        0xE3500001u,  /* CMP r0,#1 -> no Z */
        0x0A000000u,  /* BEQ (not taken) -> falls through */
        0xE3A02002u,  /* 0x0C executed on fall-through */
    };
    rig_load(&g, prog, 4, 0);
    arm26_run(&g.cpu, 4);
    snprintf(det, sizeof det, "r2=%" PRIu32, g.cpu.r[2]);
    check("beq-fallthrough", g.cpu.r[2] == 2, det);
}

/* 7. MUL */
static void t_mul(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = { 0xE3A01007u, 0xE3A02006u, 0xE0000291u }; /* MOV r1,#7; MOV r2,#6; MUL r0,r1,r2 */
    rig_load(&g, prog, 3, 0);
    arm26_run(&g.cpu, 3);
    snprintf(det, sizeof det, "r0=%" PRIu32, g.cpu.r[0]);
    check("mul-42", g.cpu.r[0] == 42, det);
}

/* 8/9. MOVS with LSL carry */
static void t_movs_lsl_carry(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = { 0xE3A01003u, 0xE1B07F81u }; /* MOV r1,#3 ; MOVS r7,r1,LSL #31 */
    rig_load(&g, prog, 2, 0);
    arm26_run(&g.cpu, 2);
    snprintf(det, sizeof det, "r7=%" PRIx32, g.cpu.r[7]);
    check("lsl-val", g.cpu.r[7] == 0x80000000u, det);
    snprintf(det, sizeof det, FLAGS_STR(g.cpu.r[15]));
    check("lsl-flags", (g.cpu.r[15] & FLAG_N) && (g.cpu.r[15] & FLAG_C), det);
}

/* 10/11. ADC chain */
static void t_adc_chain(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = {
        0xE3E05000u,  /* MVN r5,#0  -> 0xFFFFFFFF */
        0xE2956001u,  /* ADDS r6,r5,#1 -> 0, Z|C */
        0xE2A67000u,  /* ADC r7,r6,#0 -> 0 + 0 + C = 1 */
    };
    rig_load(&g, prog, 3, 0);
    arm26_run(&g.cpu, 3);
    snprintf(det, sizeof det, "Z=%d C=%d", !!(g.cpu.r[15] & FLAG_Z), !!(g.cpu.r[15] & FLAG_C));
    check("adds-zc", (g.cpu.r[15] & FLAG_Z) && (g.cpu.r[15] & FLAG_C), det);
    snprintf(det, sizeof det, "r7=%" PRIu32, g.cpu.r[7]);
    check("adc-val", g.cpu.r[7] == 1, det);
}

/* 12/13. TST */
static void t_tst_z(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = { 0xE3A080F0u, 0xE318800Fu }; /* MOV r8,#0xF0 ; TST r8,#0x0F -> 0 -> Z */
    rig_load(&g, prog, 2, 0);
    arm26_run(&g.cpu, 2);
    check("tst-flags", (g.cpu.r[15] & FLAG_Z) != 0, "TST should set Z");
    snprintf(det, sizeof det, "r8=%" PRIx32, g.cpu.r[8]);
    check("tst-noclobber", g.cpu.r[8] == 0xF0, det);
}

/* 14. BIC */
static void t_bic(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = { 0xE1C03001u }; /* BIC r3,r0,r1 */
    rig_load(&g, prog, 1, 0);
    g.cpu.r[0] = 0xFF; g.cpu.r[1] = 0x0F;
    arm26_run(&g.cpu, 1);
    snprintf(det, sizeof det, "r3=%" PRIx32, g.cpu.r[3]);
    check("bic-val", g.cpu.r[3] == 0xF0, det);
}

/* 15/16. STR/LDR round trip */
static void t_str_ldr_rt(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = { 0xE3A02D20u, 0xE5810000u, 0xE5914000u }; /* MOV r2,..; STR r0,[r1]; LDR r4,[r1] */
    rig_load(&g, prog, 3, 0);
    g.cpu.r[0] = 0xDEADBEEFu; g.cpu.r[1] = 0x1000;
    arm26_run(&g.cpu, 2); /* STR only */
    uint32_t stored = arm26_rd32(&g.cpu, 0x1000);
    snprintf(det, sizeof det, "mem=%" PRIx32, stored);
    check("str-word", stored == 0xDEADBEEFu, det);
    arm26_run(&g.cpu, 1);
    snprintf(det, sizeof det, "r4=%" PRIx32, g.cpu.r[4]);
    check("ldr-word", g.cpu.r[4] == 0xDEADBEEFu, det);
}

/* 17. unaligned LDR rotates the aligned word */
static void t_ldr_unaligned_rotate(void)
{
    Rig g; rig_init(&g);
    arm26_wr32(&g.cpu, 0x100, 0x12345678u);
    g.cpu.r[2] = 0x100;
    uint32_t prog[] = { 0xE5920001u }; /* LDR r0,[r2,#1] */
    rig_load(&g, prog, 1, 0x200);
    rig_setpc(&g, 0x200);
    arm26_run(&g.cpu, 1);
    snprintf(det, sizeof det, "r0=%" PRIx32, g.cpu.r[0]);
    check("ldr-rot", g.cpu.r[0] == 0x78123456u, det);
}

/* 18/19. LDRB/STRB */
static void t_ldrb_strb(void)
{
    Rig g; rig_init(&g);
    arm26_wr32(&g.cpu, 0x100, 0xAABBCCDDu);
    g.cpu.r[2] = 0x100;
    uint32_t p1[] = { 0xE5D20000u }; /* LDRB r0,[r2] */
    rig_load(&g, p1, 1, 0x200);
    rig_setpc(&g, 0x200);
    arm26_run(&g.cpu, 1);
    snprintf(det, sizeof det, "r0=%" PRIx32, g.cpu.r[0]);
    check("ldrb", g.cpu.r[0] == 0xDD, det);
    g.cpu.r[1] = 0xEE; g.cpu.r[3] = 0x200;
    uint32_t p2[] = { 0xE5C31000u }; /* STRB r1,[r3,#0] */
    rig_load(&g, p2, 1, 0x204);
    rig_setpc(&g, 0x204);
    arm26_run(&g.cpu, 1);
    snprintf(det, sizeof det, "mem=%" PRIx32, arm26_rd8(&g.cpu, 0x200));
    check("strb", arm26_rd8(&g.cpu, 0x200) == 0xEE, det);
}

/* 20-23. STMFD/LDMFD round trip */
static void t_stmfd_ldmfd(void)
{
    Rig g; rig_init(&g);
    g.cpu.r[0] = 1; g.cpu.r[1] = 2; g.cpu.r[2] = 3; g.cpu.r[13] = 0x200;
    uint32_t p1[] = { 0xE92D0007u }; /* STMFD r13!,{r0-r2} */
    rig_load(&g, p1, 1, 0x300);
    rig_setpc(&g, 0x300);
    arm26_run(&g.cpu, 1);
    snprintf(det, sizeof det, "r13=%" PRIx32, g.cpu.r[13]);
    check("stmfd-base", g.cpu.r[13] == 0x1F4, det);
    int memok = arm26_rd32(&g.cpu, 0x1F4) == 1 && arm26_rd32(&g.cpu, 0x1F8) == 2 && arm26_rd32(&g.cpu, 0x1FC) == 3;
    check("stmfd-mem", memok, "stack contents order");
    uint32_t p2[] = { 0xE8BD0070u }; /* LDMFD r13!,{r4,r5,r6} */
    rig_load(&g, p2, 1, 0x304);
    rig_setpc(&g, 0x304);
    arm26_run(&g.cpu, 1);
    check("ldmfd-vals", g.cpu.r[4] == 1 && g.cpu.r[5] == 2 && g.cpu.r[6] == 3, "restore regs");
    snprintf(det, sizeof det, "r13=%" PRIx32, g.cpu.r[13]);
    check("ldmfd-base", g.cpu.r[13] == 0x200, det);
}

/* 24. STM stores CPSR in r15 slot */
static void t_stm_stores_cpsr_in_r15(void)
{
    Rig g; rig_init(&g);
    g.cpu.r[1] = 0x100; g.cpu.r[0] = 0x77;
    uint32_t prog[] = { 0xE8818000u }; /* STM r1,{r15} */
    rig_load(&g, prog, 1, 0x200);
    rig_setpc(&g, 0x200);
    arm26_run(&g.cpu, 1);
    uint32_t stored = arm26_rd32(&g.cpu, 0x100);
    uint32_t expect = FLAG_I | FLAG_F | 0x208 | MODE_SVC;
    snprintf(det, sizeof det, "stored=%" PRIx32 " expect=%" PRIx32, stored, expect);
    check("stm-r15-cpsr", stored == expect, det);
}

/* 25/26. SWP */
static void t_swp(void)
{
    Rig g; rig_init(&g);
    arm26_wr32(&g.cpu, 0x1000, 0xAA551212u);
    g.cpu.r[1] = 0x99; g.cpu.r[2] = 0x1000;
    uint32_t prog[] = { 0xE1020091u }; /* SWP r0,r1,[r2] */
    rig_load(&g, prog, 1, 0x200);
    rig_setpc(&g, 0x200);
    arm26_run(&g.cpu, 1);
    snprintf(det, sizeof det, "r0=%" PRIx32, g.cpu.r[0]);
    check("swp-old", g.cpu.r[0] == 0xAA551212u, det);
    snprintf(det, sizeof det, "mem=%" PRIx32, arm26_rd32(&g.cpu, 0x1000));
    check("swp-new", arm26_rd32(&g.cpu, 0x1000) == 0x99, det);
}

/* 27-29. SWI entry */
static void t_swi_entry(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = { 0xE3A02000u, 0xEF000001u }; /* MOV r2,#0 ; SWI 1 */
    rig_load(&g, prog, 2, 0);
    arm26_run(&g.cpu, 2);
    snprintf(det, sizeof det, "pc=%" PRIx32 " mode=%" PRIu32, g.cpu.r[15] & PC_MASK, g.cpu.r[15] & 3);
    check("swi-pc", (g.cpu.r[15] & PC_MASK) == 0x08 && (g.cpu.r[15] & 3) == MODE_SVC, det);
    check("swi-I", (g.cpu.r[15] & FLAG_I) != 0, "I set on SVC entry");
    uint32_t expect = FLAG_I | FLAG_F | 0x08 | MODE_SVC;
    snprintf(det, sizeof det, "lr=%" PRIx32 " (live r14 in SVC: cpsr at SWI, pc=0x08, I|F)", g.cpu.r[14]);
    check("swi-lr", g.cpu.r[14] == expect, det);
}

/* 30-33. SWI return via MOVS pc,lr */
static void t_swi_ret_movs_pc(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = {
        0xEF000001u,  /* 0x00 SWI 1 -> enter SVC -> pc=8 */
        0xE3A04009u,  /* 0x04 MOV r4,#9 (return address; never executed by run(2)) */
        0xE1B0F00Eu,  /* 0x08 MOVS pc, r14 -> return to 0x04 */
    };
    rig_load(&g, prog, 3, 0);
    arm26_run(&g.cpu, 2);
    snprintf(det, sizeof det, "pc=%" PRIx32, g.cpu.r[15] & PC_MASK);
    check("rti-pc", (g.cpu.r[15] & PC_MASK) == 0x04, det);
    snprintf(det, sizeof det, "mode=%" PRIu32, g.cpu.r[15] & 3);
    check("rti-mode", (g.cpu.r[15] & 3) == MODE_SVC, det);
    check("rti-IF-from-lr", (g.cpu.r[15] & (FLAG_I | FLAG_F)) == (FLAG_I | FLAG_F),
          "MOVS pc,lr is a full CPSR restore: I|F come from lr_svc");
    check("rti-r4-untouched", g.cpu.r[4] == 0, "instruction after return not executed");
}

/* 34/35. undefined-instruction entry (MRS pattern) */
static void t_undef_mrs(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = { 0xE10F0000u }; /* MRS pattern -> undefined trap */
    rig_load(&g, prog, 1, 0);
    rig_setpc(&g, 0);
    arm26_run(&g.cpu, 1);
    snprintf(det, sizeof det, "pc=%" PRIx32, g.cpu.r[15] & PC_MASK);
    check("undef-entry", (g.cpu.r[15] & PC_MASK) == 0x04 && (g.cpu.r[15] & FLAG_I) != 0, det);
    uint32_t expect = FLAG_I | FLAG_F | 0x04 | MODE_SVC;
    snprintf(det, sizeof det, "lr=%" PRIx32 " (live r14: cpsr at trap, pc=4, I|F, SVC)", g.cpu.r[14]);
    check("undef-lr", g.cpu.r[14] == expect, det);
}

/* 36-38. IRQ entry */
static void t_irq_entry(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = { 0xEAFFFFFEu }; /* B . (spin) */
    rig_load(&g, prog, 1, 0);
    g.cpu.r[15] &= ~FLAG_I;      /* enable IRQs (reset leaves I set) */
    g.cpu.irqPending = 1;        /* raise between instructions */
    arm26_run(&g.cpu, 1);
    snprintf(det, sizeof det, "pc=%" PRIx32, g.cpu.r[15] & PC_MASK);
    check("irq-pc", (g.cpu.r[15] & PC_MASK) == 0x18 && (g.cpu.r[15] & FLAG_I) != 0, det);
    check("irq-keep-F", (g.cpu.r[15] & FLAG_F) != 0, "F preserved from pre-IRQ cpsr");
    snprintf(det, sizeof det, "mode=%" PRIu32, g.cpu.r[15] & 3);
    check("irq-mode", (g.cpu.r[15] & 3) == MODE_IRQ, det);
}

/* 39. TEQP mode switch; TEQP/TSTP with rd=15 update CPSR only, never PC */
static void t_teqp_mode_switch(void)
{
    Rig g; rig_init(&g);
    g.cpu.r[0] = 0;
    uint32_t prog[] = {
        0xE330F000u,  /* 0x00 TEQP r0,#0  -> CPSR <- r0, PC must not jump */
        0xE3A01001u,  /* 0x04 MOV r1,#1 */
    };
    rig_load(&g, prog, 2, 0);
    rig_setpc(&g, 0);
    arm26_run(&g.cpu, 1);
    snprintf(det, sizeof det, "mode=%" PRIu32 " cpsr=%" PRIx32, g.cpu.r[15] & 3, g.cpu.r[15]);
    check("teqp-usr", (g.cpu.r[15] & 3) == MODE_USR, det);
    snprintf(det, sizeof det, "pc=%08" PRIx32, g.cpu.r[15] & PC_MASK);
    check("teqp-pc-preserved", (g.cpu.r[15] & PC_MASK) == 0x04, det);
    arm26_run(&g.cpu, 1);
    snprintf(det, sizeof det, "pc=%08" PRIx32 " r1=%" PRIx32, g.cpu.r[15] & PC_MASK, g.cpu.r[1]);
    check("teqp-continues", (g.cpu.r[15] & PC_MASK) == 0x08 && g.cpu.r[1] == 1, det);

    /* ROM idiom: teq lr,#0 with lr holding a CPSR image (bit27=I) */
    Rig g2; rig_init(&g2);
    g2.cpu.r[14] = 0x0B802AECu;
    uint32_t prog2[] = { 0xE33EF000u, 0xE3A01001u }; /* TEQP lr,#0 ; MOV r1,#1 */
    rig_load(&g2, prog2, 2, 0);
    rig_setpc(&g2, 0);
    arm26_run(&g2.cpu, 1);
    snprintf(det, sizeof det, "pc=%08" PRIx32 " cpsr=%08" PRIx32, g2.cpu.r[15] & PC_MASK, g2.cpu.r[15]);
    check("teqp-lr-image", (g2.cpu.r[15] & PC_MASK) == 0x04 && (g2.cpu.r[15] & FLAG_I) != 0, det);
}

/* 40/41. loop: sum 1..10 */
static void t_sum_loop(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = {
        0xE3A0000Au,  /* 0x00 MOV r0,#10 */
        0xE3A01000u,  /* 0x04 MOV r1,#0 */
        0xE0811000u,  /* 0x08 ADD r1,r1,r0 */
        0xE2500001u,  /* 0x0C SUBS r0,r0,#1 */
        0x1AFFFFFCu,  /* 0x10 BNE 0x08 */
    };
    rig_load(&g, prog, 5, 0);
    arm26_run(&g.cpu, 2 + 30);
    snprintf(det, sizeof det, "r1=%" PRIu32, g.cpu.r[1]);
    check("sum-55", g.cpu.r[1] == 55, det);
    check("sum-Z", (g.cpu.r[15] & FLAG_Z) != 0, "loop ended with Z set");
}

/* 42. SWI 0 halts */
static void t_swi0_halts(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = { 0xEF000000u, 0xE3A00001u }; /* SWI 0 ; MOV r0,#1 */
    rig_load(&g, prog, 2, 0);
    arm26_run(&g.cpu, 10);
    check("halt", g.cpu.halted && g.cpu.r[0] == 0, "SWI 0 stops the run loop");
}

/* 43. r15 as first operand (Rn) of data processing: PSR bits supplied as zero */
static void t_rn15_psr_zero(void)
{
    Rig g; rig_init(&g);
    g.cpu.r[15] |= FLAG_N; /* N set in CPSR, mode SVC from reset */
    uint32_t prog[] = { 0xE28F0000u, 0xE1A0100Fu }; /* ADD r0,pc,#0 ; MOV r1,pc */
    rig_load(&g, prog, 2, 0);
    arm26_run(&g.cpu, 1);
    check("rn15-psr-zero", g.cpu.r[0] == 8, "Rn=r15 reads PC+8 with PSR zeroed");
    arm26_run(&g.cpu, 1);
    snprintf(det, sizeof det, "r1=%08" PRIx32, g.cpu.r[1]);
    check("rm15-keeps-psr", g.cpu.r[1] == (0x80000000u | 0x0C000000u | 0x0Cu | MODE_SVC),
          "Rm=r15 keeps flags|PC+8|mode");
}

/* 44. LDR with Rd==Rn and writeback: the loaded value wins (ARM2 silicon) */
static void t_ldr_rd_rn_load_wins(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = { 0xE3A00040u, 0xE4900000u }; /* MOV r0,#0x40 ; LDR r0,[r0],#0 */
    rig_load(&g, prog, 2, 0);
    arm26_wr32(&g.cpu, 0x40, 0x12345678u);
    arm26_run(&g.cpu, 2);
    snprintf(det, sizeof det, "r0=%08" PRIx32, g.cpu.r[0]);
    check("ldr-rd-rn-post-load-wins", g.cpu.r[0] == 0x12345678u,
          "post-indexed LDR Rd==Rn: loaded value wins over base writeback");

    Rig h; rig_init(&h);
    uint32_t prog2[] = { 0xE3A00040u, 0xE5B00004u }; /* MOV r0,#0x40 ; LDR r0,[r0,#4]! */
    rig_load(&h, prog2, 2, 0);
    arm26_wr32(&h.cpu, 0x44, 0xCAFEBABEu);
    arm26_run(&h.cpu, 2);
    snprintf(det, sizeof det, "r0=%08" PRIx32, h.cpu.r[0]);
    check("ldr-rd-rn-prewb-load-wins", h.cpu.r[0] == 0xCAFEBABEu,
          "pre-indexed writeback LDR Rd==Rn: loaded value wins");
}

/* 45. SWI 0 takes the SWI vector when swi0_halt is cleared (machine/ROM mode) */
static void t_swi0_vectors_when_not_halting(void)
{
    Rig g; rig_init(&g);
    g.cpu.swi0_halt = 0;
    uint32_t prog[] = { 0xEF000000u, 0xE3A00001u }; /* SWI 0 ; MOV r0,#1 */
    rig_load(&g, prog, 2, 0);
    uint32_t vec[] = { 0xEA00000Cu };               /* 0x08: B 0x40 */
    rig_load(&g, vec, 1, 0x08);
    uint32_t handler[] = { 0xE3A00002u };           /* 0x40: MOV r0,#2 */
    rig_load(&g, handler, 1, 0x40);
    arm26_run(&g.cpu, 3);
    snprintf(det, sizeof det, "halted=%d r0=%" PRIx32 " mode=%u",
             g.cpu.halted, g.cpu.r[0], g.cpu.r[15] & 3);
    check("swi0-vectors", !g.cpu.halted && g.cpu.r[0] == 2 && (g.cpu.r[15] & 3) == MODE_SVC,
          "SWI 0 vectors to 0x08 in SVC mode when swi0_halt=0");
}

int main(void)
{
    t_mov_imm();
    t_mov_rot_imm();
    t_add();
    t_sub_neg_flags();
    t_branch_taken();
    t_branch_not_taken();
    t_mul();
    t_movs_lsl_carry();
    t_adc_chain();
    t_tst_z();
    t_bic();
    t_str_ldr_rt();
    t_ldr_unaligned_rotate();
    t_ldrb_strb();
    t_stmfd_ldmfd();
    t_stm_stores_cpsr_in_r15();
    t_swp();
    t_swi_entry();
    t_swi_ret_movs_pc();
    t_undef_mrs();
    t_irq_entry();
    t_teqp_mode_switch();
    t_sum_loop();
    t_swi0_halts();
    t_rn15_psr_zero();
    t_ldr_rd_rn_load_wins();
    t_swi0_vectors_when_not_halting();
    return test_summary("arm26") ? 1 : 0;
}
