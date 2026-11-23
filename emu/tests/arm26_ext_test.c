/*
 * arm26_ext_test.c - WP6 core-extension tests (ARMv4 halfword/signed
 * transfers, register-offset LDR/STR via group 011, LDR pc).
 *
 * Extends the transplanted Appendix-B suite (arm26_test.c, kept verbatim);
 * hand-encoded instruction words, no external assembler (spec section 10).
 * The group-011 and LDR-pc fixes are deliberate deviations from the JS
 * reference toward the ARM ARM (needed by real ROM vector tables); see
 * docs/reference-manual.md section 7.
 *
 * Run: ./arm26_ext_test   (via `make test`)
 * Licence: Apache-2.0 (see LICENSE).
 */
#include <stdio.h>
#include <inttypes.h>

#include "test.h"

static char det[128];

/* STRH r0,[r1,#8] / LDRH r2,[r1,#8] */
static void t_strh_ldrh_imm(void)
{
    Rig g; rig_init(&g);
    arm26_wr32(&g.cpu, 0x100, 0x12345678u);
    uint32_t prog[] = {
        0xE3A010F8u,  /* MOV r1,#0xF8 */
        0xE3A000FFu,  /* MOV r0,#0xFF */
        0xE1C100B8u,  /* STRH r0,[r1,#8]  -> 0x100 */
        0xE1D120B8u,  /* LDRH r2,[r1,#8] */
        0xEF000000u,  /* SWI 0 */
    };
    rig_load(&g, prog, 5, 0);
    arm26_run(&g.cpu, 5);
    snprintf(det, sizeof det, "mem[0x100]=%02x %02x", arm26_rd8(&g.cpu, 0x100), arm26_rd8(&g.cpu, 0x101));
    check("strh-imm", arm26_rd8(&g.cpu, 0x100) == 0xFF && arm26_rd8(&g.cpu, 0x101) == 0x00, det);
    uint32_t w = arm26_rd32(&g.cpu, 0x100);
    snprintf(det, sizeof det, "word=%" PRIx32, w);
    check("strh-keep-upper", w == 0x123400FFu, det);
    snprintf(det, sizeof det, "r2=%" PRIx32, g.cpu.r[2]);
    check("ldrh-imm", g.cpu.r[2] == 0xFF, det);
}

/* LDRH zero-extends */
static void t_ldrh(void)
{
    Rig g; rig_init(&g);
    arm26_wr32(&g.cpu, 0x100, 0xABCD1234u);
    g.cpu.r[1] = 0x100;
    uint32_t prog[] = { 0xE1D120B0u, 0xEF000000u }; /* LDRH r2,[r1,#0] ; SWI 0 */
    rig_load(&g, prog, 2, 0);
    arm26_run(&g.cpu, 2);
    snprintf(det, sizeof det, "r2=%" PRIx32, g.cpu.r[2]);
    check("ldrh", g.cpu.r[2] == 0x1234, det);
}

/* LDRSB sign-extends */
static void t_ldrsb(void)
{
    Rig g; rig_init(&g);
    arm26_wr8(&g.cpu, 0x100, 0x80);
    g.cpu.r[1] = 0x100;
    uint32_t prog[] = { 0xE1D120D0u, 0xEF000000u }; /* LDRSB r2,[r1,#0] ; SWI 0 */
    rig_load(&g, prog, 2, 0);
    arm26_run(&g.cpu, 2);
    snprintf(det, sizeof det, "r2=%" PRIx32, g.cpu.r[2]);
    check("ldrsb-neg", g.cpu.r[2] == 0xFFFFFF80u, det);

    Rig h; rig_init(&h);
    arm26_wr8(&h.cpu, 0x100, 0x7F);
    h.cpu.r[1] = 0x100;
    rig_load(&h, prog, 2, 0);
    arm26_run(&h.cpu, 2);
    snprintf(det, sizeof det, "r2=%" PRIx32, h.cpu.r[2]);
    check("ldrsb-pos", h.cpu.r[2] == 0x7F, det);
}

/* LDRSH sign-extends */
static void t_ldrsh(void)
{
    Rig g; rig_init(&g);
    arm26_wr8(&g.cpu, 0x100, 0x00);
    arm26_wr8(&g.cpu, 0x101, 0x80); /* halfword 0x8000 */
    g.cpu.r[1] = 0x100;
    uint32_t prog[] = { 0xE1D120F0u, 0xEF000000u }; /* LDRSH r2,[r1,#0] ; SWI 0 */
    rig_load(&g, prog, 2, 0);
    arm26_run(&g.cpu, 2);
    snprintf(det, sizeof det, "r2=%" PRIx32, g.cpu.r[2]);
    check("ldrsh-neg", g.cpu.r[2] == 0xFFFF8000u, det);
}

/* STRH with register offset */
static void t_strh_reg(void)
{
    Rig g; rig_init(&g);
    g.cpu.r[0] = 0x00FF; g.cpu.r[1] = 0x100; g.cpu.r[2] = 4;
    uint32_t prog[] = { 0xE18100B2u, 0xEF000000u }; /* STRH r0,[r1,r2] ; SWI 0 */
    rig_load(&g, prog, 2, 0);
    arm26_run(&g.cpu, 2);
    snprintf(det, sizeof det, "mem[0x104]=%02x %02x", arm26_rd8(&g.cpu, 0x104), arm26_rd8(&g.cpu, 0x105));
    check("strh-reg-offset", arm26_rd8(&g.cpu, 0x104) == 0xFF && arm26_rd8(&g.cpu, 0x105) == 0x00, det);
}

/* LDRH with register offset and writeback */
static void t_ldrh_writeback(void)
{
    Rig g; rig_init(&g);
    arm26_wr32(&g.cpu, 0x104, 0x0000BEEFu);
    g.cpu.r[1] = 0x100; g.cpu.r[2] = 4;
    uint32_t prog[] = { 0xE1B100B2u, 0xEF000000u }; /* LDRH r0,[r1,r2]! ; SWI 0 */
    rig_load(&g, prog, 2, 0);
    arm26_run(&g.cpu, 2);
    snprintf(det, sizeof det, "r0=%" PRIx32 " r1=%" PRIx32, g.cpu.r[0], g.cpu.r[1]);
    check("ldrh-reg-wb", g.cpu.r[0] == 0xBEEF && g.cpu.r[1] == 0x104, det);
}

/* LDR with register offset (group 011, bit 4 = 0) */
static void t_ldr_reg_offset(void)
{
    Rig g; rig_init(&g);
    arm26_wr32(&g.cpu, 0x104, 0xDEADBEEFu);
    g.cpu.r[1] = 0x100; g.cpu.r[2] = 4;
    uint32_t prog[] = { 0xE7910002u, 0xEF000000u }; /* LDR r0,[r1,r2] ; SWI 0 */
    rig_load(&g, prog, 2, 0);
    arm26_run(&g.cpu, 2);
    snprintf(det, sizeof det, "r0=%" PRIx32, g.cpu.r[0]);
    check("ldr-reg-offset", g.cpu.r[0] == 0xDEADBEEFu, det);
}

/* STR with register offset */
static void t_str_reg_offset(void)
{
    Rig g; rig_init(&g);
    g.cpu.r[0] = 0xCAFEBABEu; g.cpu.r[1] = 0x100; g.cpu.r[2] = 4;
    uint32_t prog[] = { 0xE7810002u, 0xEF000000u }; /* STR r0,[r1,r2] ; SWI 0 */
    rig_load(&g, prog, 2, 0);
    arm26_run(&g.cpu, 2);
    uint32_t w = arm26_rd32(&g.cpu, 0x104);
    snprintf(det, sizeof det, "mem=%" PRIx32, w);
    check("str-reg-offset", w == 0xCAFEBABEu, det);
}

/* STR r0,[r1,-r2]! : negative register offset with writeback */
static void t_str_reg_neg_wb(void)
{
    Rig g; rig_init(&g);
    g.cpu.r[0] = 0x12345678u; g.cpu.r[1] = 0x200; g.cpu.r[2] = 0x10;
    uint32_t prog[] = { 0xE7210002u, 0xEF000000u }; /* STR r0,[r1,-r2]! ; SWI 0 */
    rig_load(&g, prog, 2, 0);
    arm26_run(&g.cpu, 2);
    uint32_t w = arm26_rd32(&g.cpu, 0x1F0);
    snprintf(det, sizeof det, "mem=%" PRIx32 " r1=%" PRIx32, w, g.cpu.r[1]);
    check("str-reg-neg-wb", w == 0x12345678u && g.cpu.r[1] == 0x1F0, det);
}

/* group 011 with bit 4 = 1 stays undefined */
static void t_grp011_bit4_undef(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = { 0xE7910012u }; /* LDR r0,[r1,r2] with bit4=1 */
    rig_load(&g, prog, 1, 0);
    arm26_run(&g.cpu, 1);
    snprintf(det, sizeof det, "pc=%" PRIx32, g.cpu.r[15] & PC_MASK);
    check("grp011-bit4-undef", (g.cpu.r[15] & PC_MASK) == 0x04 && (g.cpu.r[15] & FLAG_I) != 0, det);
}

/* LDR pc,[r0] jumps (flags preserved) */
static void t_ldr_pc(void)
{
    Rig g; rig_init(&g);
    arm26_wr32(&g.cpu, 0x100, 0x200);
    g.cpu.r[0] = 0x100;
    uint32_t prog[] = { 0xE590F000u }; /* 0x000 LDR pc,[r0] */
    rig_load(&g, prog, 1, 0);
    uint32_t prog2[] = { 0xE3A01007u, 0xEF000000u }; /* 0x200 MOV r1,#7 ; SWI 0 */
    rig_load(&g, prog2, 2, 0x200);
    arm26_run(&g.cpu, 2);
    snprintf(det, sizeof det, "pc=%" PRIx32 " r1=%" PRIu32, g.cpu.r[15] & PC_MASK, g.cpu.r[1]);
    check("ldr-pc", (g.cpu.r[15] & PC_MASK) == 0x204 && g.cpu.r[1] == 7, det);
}

/* store with S=1 in the halfword space is undefined (no STRSB/STRSH) */
static void t_signed_store_undef(void)
{
    Rig g; rig_init(&g);
    uint32_t prog[] = { 0xE1C010D0u }; /* "STRSB" r0,[r1,#0]: 1101 in bits 7:4, L=0 */
    rig_load(&g, prog, 1, 0);
    arm26_run(&g.cpu, 1);
    snprintf(det, sizeof det, "pc=%" PRIx32, g.cpu.r[15] & PC_MASK);
    check("signed-store-undef", (g.cpu.r[15] & PC_MASK) == 0x04 && (g.cpu.r[15] & FLAG_I) != 0, det);
}

int main(void)
{
    t_strh_ldrh_imm();
    t_ldrh();
    t_ldrsb();
    t_ldrsh();
    t_strh_reg();
    t_ldrh_writeback();
    t_ldr_reg_offset();
    t_str_reg_offset();
    t_str_reg_neg_wb();
    t_grp011_bit4_undef();
    t_ldr_pc();
    t_signed_store_undef();
    return test_summary("arm26_ext") ? 1 : 0;
}
