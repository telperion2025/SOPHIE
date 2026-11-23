/*
 * arm26.c - ARMv2/ARMv3 26-bit interpreter core (WP1 C port of Appendix A).
 *
 * Transliterated statement-for-statement from the verified JS reference
 * (42-assertion suite, spec Appendix B / section 9). Behaviour is
 * bug-compatible with that reference, including its documented quirks
 * (see docs/reference-manual.md, "Known limitations").
 *
 * Licence: Apache-2.0 (see LICENSE).
 */
#include "arm26.h"

/* ---------- memory ---------- */

uint8_t arm26_rd8(Arm26 *cpu, uint32_t a)  { return mem_rd8(cpu->mem, a & ADDR_MASK); }
void    arm26_wr8(Arm26 *cpu, uint32_t a, uint8_t v)  { mem_wr8(cpu->mem, a & ADDR_MASK, v); }
uint32_t arm26_rd32(Arm26 *cpu, uint32_t a) { return mem_rd32(cpu->mem, a & ADDR_MASK); }
void    arm26_wr32(Arm26 *cpu, uint32_t a, uint32_t v) { mem_wr32(cpu->mem, a & ADDR_MASK, v); }

void arm26_load_program(Arm26 *cpu, const uint32_t *words, size_t n, uint32_t base)
{
    for (size_t i = 0; i < n; i++)
        arm26_wr32(cpu, base + (uint32_t)i * 4, words[i]);
}

/* ---------- model identity ---------- */

uint32_t arm26_mode(const Arm26 *cpu) { return cpu->r[15] & 3; }

/* r15 as it appears when read as an operand during the instruction at a */
uint32_t arm26_pc_operand(const Arm26 *cpu, uint32_t a)
{
    return (cpu->r[15] & 0xFC000000u) | (((a + 8) & PC_MASK)) | (cpu->r[15] & 3);
}

/* ---------- construction / reset ---------- */

void arm26_init(Arm26 *cpu, mem_bus *mem)
{
    cpu->mem = mem;
    arm26_reset(cpu);
}

void arm26_reset(Arm26 *cpu)
{
    for (int i = 0; i < 16; i++) cpu->r[i] = 0;
    for (int i = 0; i < 7; i++) { cpu->bankUsr[i] = 0; cpu->bankFiq[i] = 0; }
    cpu->bankSvc[0] = cpu->bankSvc[1] = 0;
    cpu->bankIrq[0] = cpu->bankIrq[1] = 0;
    /* r15 = I,F set, SVC mode, pc 0 (ARMv3 26-bit reset state) */
    cpu->r[15] = FLAG_I | FLAG_F | MODE_SVC;
    cpu->irqPending = 0;
    cpu->fiqPending = 0;
    cpu->cycles = 0;
    cpu->halted = 0;
}

/* ---------- register banking ---------- */

static uint32_t *bank_cell(Arm26 *cpu, uint32_t mode, int i) /* i in 8..14 */
{
    if (mode == MODE_FIQ) return &cpu->bankFiq[i - 8];
    if (i <= 12) return &cpu->bankUsr[i - 8];               /* shared r8-r12 */
    if (i == 13)
        return mode == MODE_SVC ? &cpu->bankSvc[0]
             : mode == MODE_IRQ ? &cpu->bankIrq[0]
             : &cpu->bankUsr[5];
    return mode == MODE_SVC ? &cpu->bankSvc[1]
         : mode == MODE_IRQ ? &cpu->bankIrq[1]
         : &cpu->bankUsr[6];
}

static void set_mode(Arm26 *cpu, uint32_t m)
{
    uint32_t old = arm26_mode(cpu);
    if (old == m) return;
    for (int i = 8; i < 15; i++) *bank_cell(cpu, old, i) = cpu->r[i];
    for (int i = 8; i < 15; i++) cpu->r[i] = *bank_cell(cpu, m, i);
}

/* ---------- exceptions ---------- */

/*
 * 26-bit exception entry.
 * banked r14_mode := current CPSR (flags | pc | old mode)
 * CPSR := (NZCV preserved) | (I set; F set for FIQ) | vector | new mode
 */
static void exception(Arm26 *cpu, uint32_t vector, uint32_t newMode, int setF)
{
    uint32_t oldCPSR = cpu->r[15];
    set_mode(cpu, newMode);
    cpu->r[14] = oldCPSR;
    uint32_t nzcv = oldCPSR & FLAG_NZCV;
    uint32_t fiqBit = setF ? FLAG_F : (oldCPSR & FLAG_F);
    cpu->r[15] = nzcv | FLAG_I | fiqBit | vector | newMode;
}

/* ---------- register access ---------- */

/* operand read; bankSel forces USR-bank view for LDM/STM user-mode transfers */
static uint32_t get_reg(Arm26 *cpu, int i, uint32_t a, int bankSel)
{
    if (i == 15) return arm26_pc_operand(cpu, a);
    if (!bankSel) return cpu->r[i];
    if (i >= 8 && i <= 14) return *bank_cell(cpu, MODE_USR, i);
    return cpu->r[i];
}

static void set_reg(Arm26 *cpu, int i, uint32_t v, int bankSel)
{
    if (i == 15) return; /* r15 writes go through the _writePC paths */
    if (bankSel && i >= 8 && i <= 14) { *bank_cell(cpu, MODE_USR, i) = v; return; }
    cpu->r[i] = v;
}

/* r15 as a memory base (first operand of LDR/STR/LDM/STM/SWP): on 26-bit
 * ARM the PSR bits are supplied as zero when r15 is the first operand, so
 * the base address is PC+8, word aligned. (Deviation from the JS reference,
 * which supplies the full r15 here; see docs/reference-manual.md section 7.) */
static uint32_t base_reg(Arm26 *cpu, int rn, uint32_t a)
{
    if (rn == 15) return (a + 8) & PC_MASK;
    return cpu->r[rn];
}

/* MOVS-style r15 write from a logical ALU result: full CPSR replace (if priv) */
static void write_pc_logical(Arm26 *cpu, uint32_t v)
{
    if (arm26_mode(cpu) != MODE_USR) {
        cpu->r[15] = (v & (FLAG_NZCV | FLAG_I | FLAG_F)) | (v & PC_MASK) | (v & 3);
    } else {
        /* unprivileged: NZCV change; I,F,mode protected */
        cpu->r[15] = (v & FLAG_NZCV) | (cpu->r[15] & (FLAG_I | FLAG_F)) | (v & PC_MASK) | MODE_USR;
    }
}

/* MOVS-style r15 write from an arithmetic ALU or LDM-with-S load:
 * kind 'arith'   : NZCV from computed flags, IF+mode kept
 * kind 'restore' : full CPSR from value (if priv) */
static void write_pc_restore(Arm26 *cpu, uint32_t v, int restore)
{
    if (restore && arm26_mode(cpu) != MODE_USR) {
        cpu->r[15] = (v & (FLAG_NZCV | FLAG_I | FLAG_F)) | (v & PC_MASK) | (v & 3);
        return;
    }
    cpu->r[15] = (v & FLAG_NZCV) | (cpu->r[15] & (FLAG_I | FLAG_F | 3)) | (v & PC_MASK);
}

/* ---------- condition codes ---------- */

static int cond_ok(Arm26 *cpu, uint32_t ins)
{
    uint32_t f = cpu->r[15];
    switch ((ins >> 28) & 0xF) {
      case 0x0: return (f & FLAG_Z) != 0;
      case 0x1: return (f & FLAG_Z) == 0;
      case 0x2: return (f & FLAG_C) != 0;
      case 0x3: return (f & FLAG_C) == 0;
      case 0x4: return (f & FLAG_N) != 0;
      case 0x5: return (f & FLAG_N) == 0;
      case 0x6: return (f & FLAG_V) != 0;
      case 0x7: return (f & FLAG_V) == 0;
      case 0x8: return (f & FLAG_C) != 0 && (f & FLAG_Z) == 0;
      case 0x9: return (f & FLAG_C) == 0 || (f & FLAG_Z) != 0;
      case 0xA: return !!(f & FLAG_N) == !!(f & FLAG_V);
      case 0xB: return !!(f & FLAG_N) != !!(f & FLAG_V);
      case 0xC: return (f & FLAG_Z) == 0 && !!(f & FLAG_N) == !!(f & FLAG_V);
      case 0xD: return (f & FLAG_Z) != 0 || !!(f & FLAG_N) != !!(f & FLAG_V);
      case 0xE: return 1;
      default:  return 0; /* NV: never in 26-bit ARM */
    }
}

/* ---------- barrel shifter ---------- */

typedef struct { uint32_t v; int has_carry; uint32_t carry; } shift_result;

static shift_result apply_shift(uint32_t val, int typ, uint32_t n)
{
    shift_result r = { 0, 0, 0 };
    switch (typ) {
      case 0: /* LSL */
        if (n == 0) { r.v = val; return r; }
        if (n < 32) { r.carry = (val >> (32 - n)) & 1; r.v = val << n; r.has_carry = 1; return r; }
        if (n == 32) { r.v = 0; r.carry = val & 1; r.has_carry = 1; return r; }
        r.v = 0; r.carry = 0; r.has_carry = 1; return r;
      case 1: /* LSR */
        if (n == 0) { r.v = val; return r; }
        if (n < 32) { r.carry = (val >> (n - 1)) & 1; r.v = val >> n; r.has_carry = 1; return r; }
        if (n == 32) { r.v = 0; r.carry = (val >> 31) & 1; r.has_carry = 1; return r; }
        r.v = 0; r.carry = 0; r.has_carry = 1; return r;
      case 2: { /* ASR */
        uint32_t s = (val & 0x80000000u) ? 0xFFFFFFFFu : 0;
        if (n == 0) { r.v = val; return r; }
        if (n < 32) { r.carry = (val >> (n - 1)) & 1; r.v = (val >> n) | (s << (32 - n)); r.has_carry = 1; return r; }
        r.v = s; r.carry = (val >> 31) & 1; r.has_carry = 1; return r;
      }
      default: { /* ROR */
        if (n == 0) n = 32;
        uint32_t k = n & 31;
        if (k == 0) { r.v = val; r.carry = (val >> 31) & 1; r.has_carry = 1; return r; }
        r.v = (val >> k) | (val << (32 - k));
        r.carry = (val >> (k - 1)) & 1;
        r.has_carry = 1;
        return r;
      }
    }
}

/* carry_out === has_carry==0 means "leave C alone" */
static shift_result shift(Arm26 *cpu, uint32_t ins, uint32_t a)
{
    shift_result r = { 0, 0, 0 };
    if ((ins >> 25) & 1) { /* immediate */
        uint32_t rot = (ins >> 7) & 0x1Eu;
        uint32_t imm = ins & 0xFFu;
        if (rot == 0) { r.v = imm; return r; }
        r.v = (imm >> rot) | (imm << (32 - rot));
        r.carry = (imm >> (rot - 1)) & 1;
        r.has_carry = 1;
        return r;
    }
    int rm = ins & 0xF;
    uint32_t val = get_reg(cpu, rm, a, 0);
    int typ = (ins >> 5) & 3;
    if (((ins >> 4) & 1) == 0) { /* shift by immediate amount (bits 11-7) */
        uint32_t n = (ins >> 7) & 0x1Fu;
        if (n == 0) {
            if (typ == 0) { r.v = val; return r; }         /* LSL #0 */
            if (typ == 3) {                                 /* ROR #0 === RRX */
                uint32_t c = (cpu->r[15] & FLAG_C) ? 1 : 0;
                r.v = (val >> 1) | (c << 31);
                r.carry = val & 1;
                r.has_carry = 1;
                return r;
            }
            n = 32; /* LSR/ASR #0 behave as #32 */
        }
        return apply_shift(val, typ, n);
    }
    /* shift by register */
    int rs = (ins >> 8) & 0xF;
    uint32_t n = get_reg(cpu, rs, a, 0) & 0xFFu;
    if (typ == 0 && n == 0) { r.v = val; return r; }       /* LSL #0 */
    return apply_shift(val, typ, n);
}

/* ---------- main loop ---------- */

static void execute(Arm26 *cpu, uint32_t a, uint32_t ins);

static void service_interrupts(Arm26 *cpu)
{
    if (cpu->halted) return;
    uint32_t f = cpu->r[15];
    if (cpu->fiqPending && !(f & FLAG_F)) {
        cpu->fiqPending = 0;
        exception(cpu, VEC_FIQ, MODE_FIQ, 1);
        return;
    }
    if (cpu->irqPending && !(f & FLAG_I)) {
        cpu->irqPending = 0;
        exception(cpu, VEC_IRQ, MODE_IRQ, 0);
    }
}

void arm26_run(Arm26 *cpu, uint64_t n)
{
    while (n-- > 0 && !cpu->halted) {
        uint32_t a = cpu->r[15] & PC_MASK;
        uint32_t ins = arm26_rd32(cpu, a);
        cpu->r[15] = (cpu->r[15] & ~PC_MASK) | ((a + 4) & PC_MASK);
        cpu->cycles++;
        execute(cpu, a, ins);
        service_interrupts(cpu);
    }
}

/* ---------- instruction groups ---------- */

static void branch(Arm26 *cpu, uint32_t a, uint32_t ins)
{
    uint32_t off = (ins & 0x00FFFFFFu) << 2;
    /* sign-extend the 26-bit offset: PC bits are 25:2 of CPSR */
    uint32_t sign = (off & 0x02000000u) ? 0xFC000000u : 0;
    uint32_t target = (a + 8 + off + sign) & PC_MASK;
    if ((ins >> 24) & 1) { /* BL */
        cpu->r[14] = (cpu->r[15] & 0xFC000000u) | ((a + 4) & PC_MASK) | (cpu->r[15] & 3);
    }
    cpu->r[15] = (cpu->r[15] & ~PC_MASK) | target;
}

static void swi(Arm26 *cpu, uint32_t a, uint32_t ins)
{
    (void)a;
    /* Test-exit convention: SWI 0 halts the run loop (no exception). */
    if ((ins & 0x00FFFFFFu) == 0) { cpu->halted = 1; return; }
    exception(cpu, VEC_SWI, MODE_SVC, 0);
}

static void multiply(Arm26 *cpu, uint32_t a, uint32_t ins)
{
    int acc = (ins >> 21) & 1, S = (ins >> 20) & 1;
    int rd = (ins >> 16) & 0xF, rn = (ins >> 12) & 0xF;
    int rs = (ins >> 8) & 0xF, rm = ins & 0xF;
    uint32_t result = get_reg(cpu, rm, a, 0) * get_reg(cpu, rs, a, 0); /* low 32 bits */
    if (acc) result += get_reg(cpu, rn, a, 0);
    set_reg(cpu, rd, result, 0);
    if (S && rd != 15) {
        uint32_t f = cpu->r[15] & ~FLAG_NZCV;
        if (result & 0x80000000u) f |= FLAG_N;
        if (result == 0) f |= FLAG_Z;
        cpu->r[15] = f;
    }
}

static void swp(Arm26 *cpu, uint32_t a, uint32_t ins)
{
    int B = (ins >> 22) & 1;
    int rn = (ins >> 16) & 0xF, rd = (ins >> 12) & 0xF, rm = ins & 0xF;
    uint32_t addr = base_reg(cpu, rn, a);
    uint32_t old;
    if (B) {
        old = arm26_rd8(cpu, addr);
        arm26_wr8(cpu, addr, (uint8_t)(get_reg(cpu, rm, a, 0) & 0xFF));
    } else {
        old = arm26_rd32(cpu, addr);
        arm26_wr32(cpu, addr, get_reg(cpu, rm, a, 0));
    }
    set_reg(cpu, rd, old, 0);
}

static void data_processing(Arm26 *cpu, uint32_t a, uint32_t ins)
{
    int opcode = (ins >> 21) & 0xF;
    int S = (ins >> 20) & 1;
    int rn = (ins >> 16) & 0xF;
    int rd = (ins >> 12) & 0xF;
    uint32_t op1 = get_reg(cpu, rn, a, 0);
    shift_result sh = shift(cpu, ins, a);
    uint32_t op2 = sh.v;
    uint32_t result = 0;
    int flagsC = (cpu->r[15] & FLAG_C) != 0; /* current C if shifter leaves it alone */
    int flagsV = (cpu->r[15] & FLAG_V) != 0;
    int isArith = 0;
    uint32_t u1, u2;
    int32_t s1, s2;

    switch (opcode) {
      case 0x0: result = op1 & op2; break;
      case 0x1: result = op1 ^ op2; break;
      case 0x2: u1 = op1; u2 = op2; result = u1 - u2;
                flagsC = u1 >= u2; s1 = (int32_t)op1; s2 = (int32_t)op2;
                flagsV = (s1 < 0) != (s2 < 0) && ((int32_t)result < 0) != (s1 < 0); isArith = 1; break;
      case 0x3: u1 = op1; u2 = op2; result = u2 - u1;
                flagsC = u2 >= u1; s1 = (int32_t)op1; s2 = (int32_t)op2;
                flagsV = (s1 < 0) != (s2 < 0) && ((int32_t)result < 0) != (s2 < 0); isArith = 1; break;
      case 0x4: { uint64_t sum = (uint64_t)op1 + (uint64_t)op2; result = (uint32_t)sum;
                flagsC = sum > 0xFFFFFFFFu; s1 = (int32_t)op1; s2 = (int32_t)op2;
                flagsV = (s1 < 0) == (s2 < 0) && ((int32_t)result < 0) != (s1 < 0); isArith = 1; break; }
      case 0x5: { uint32_t cin = (cpu->r[15] & FLAG_C) ? 1 : 0;
                uint64_t sum = (uint64_t)op1 + (uint64_t)op2 + cin; result = (uint32_t)sum;
                flagsC = sum > 0xFFFFFFFFu; s1 = (int32_t)op1; s2 = (int32_t)op2;
                flagsV = (s1 < 0) == (s2 < 0) && ((int32_t)result < 0) != (s1 < 0); isArith = 1; break; }
      case 0x6: { uint32_t cin = (cpu->r[15] & FLAG_C) ? 1 : 0;
                uint64_t total = (uint64_t)op1 + (uint64_t)(~op2) + cin; result = (uint32_t)total;
                flagsC = total > 0xFFFFFFFFu; s1 = (int32_t)op1; s2 = (int32_t)op2;
                flagsV = (s1 < 0) != (s2 < 0) && ((int32_t)result < 0) != (s1 < 0); isArith = 1; break; }
      case 0x7: { uint32_t cin = (cpu->r[15] & FLAG_C) ? 1 : 0;
                uint64_t total = (uint64_t)op2 + (uint64_t)(~op1) + cin; result = (uint32_t)total;
                flagsC = total > 0xFFFFFFFFu; s1 = (int32_t)op1; s2 = (int32_t)op2;
                flagsV = (s1 < 0) != (s2 < 0) && ((int32_t)result < 0) != (s2 < 0); isArith = 1; break; }
      case 0x8: result = op1 & op2; break;  /* TST */
      case 0x9: result = op1 ^ op2; break;  /* TEQ */
      case 0xA: u1 = op1; u2 = op2; result = u1 - u2;
                flagsC = u1 >= u2; s1 = (int32_t)op1; s2 = (int32_t)op2;
                flagsV = (s1 < 0) != (s2 < 0) && ((int32_t)result < 0) != (s1 < 0); isArith = 1; break; /* CMP */
      case 0xB: { uint64_t sum = (uint64_t)op1 + (uint64_t)op2; result = (uint32_t)sum;
                flagsC = sum > 0xFFFFFFFFu; s1 = (int32_t)op1; s2 = (int32_t)op2;
                flagsV = (s1 < 0) == (s2 < 0) && ((int32_t)result < 0) != (s1 < 0); isArith = 1; break; } /* CMN */
      case 0xC: result = op1 | op2; break;
      case 0xD: result = op2; break;        /* MOV */
      case 0xE: result = op1 & ~op2; break; /* BIC */
      default:  result = ~op2; break;       /* MVN */
    }

    int isCompare = opcode >= 0x8 && opcode <= 0xB;
    int target = isCompare ? -1 : rd;

    if (S) {
        /* shifter carry-out feeds C only for logical ops; arithmetic
         * ops (ADD/SUB/CMP/...) compute C/V from the operation itself */
        if (sh.has_carry && !isArith) flagsC = sh.carry != 0;
        uint32_t f = cpu->r[15] & ~FLAG_NZCV;
        if (result & 0x80000000u) f |= FLAG_N;
        if (result == 0) f |= FLAG_Z;
        if (flagsC) f |= FLAG_C;
        if (flagsV) f |= FLAG_V;
        cpu->r[15] = f;
    }

    if (rd == 15 && S) {
        /* r15 as ALU destination with S: 26-bit CPSR semantics.
         * TSTP/TEQP/CMPP/CMNP (rd=15 on the compare ops): CPSR <- ALU
         * result, PC is NOT affected (unlike MOVS pc,.. / ADDS pc,..). */
        if (isCompare) {
            if (arm26_mode(cpu) != MODE_USR) {
                uint32_t pc = cpu->r[15] & PC_MASK;
                set_mode(cpu, result & 3);
                cpu->r[15] = (result & (FLAG_NZCV | FLAG_I | FLAG_F | 3)) | pc;
            } else {
                /* unprivileged: NZCV change only */
                cpu->r[15] = (result & FLAG_NZCV) | (cpu->r[15] & (FLAG_I | FLAG_F | 3 | PC_MASK));
            }
        } else if (isArith) {
            /* ADDS/SUBS pc,...: NZCV already updated from result above;
             * PC <- result, I/F/mode preserved. (Exception returns go via
             * LDM^/MOVS pc,lr, which restore the full CPSR.) */
            cpu->r[15] = (cpu->r[15] & (FLAG_NZCV | FLAG_I | FLAG_F | 3)) | (result & PC_MASK);
        } else {
            write_pc_logical(cpu, result);
        }
        return;
    }
    if (target == 15) {
        /* plain data write to r15 = jump, all flags preserved */
        cpu->r[15] = (cpu->r[15] & ~PC_MASK) | (result & PC_MASK);
        return;
    }
    if (!isCompare) set_reg(cpu, target, result, 0);
}

static void single_transfer(Arm26 *cpu, uint32_t a, uint32_t ins)
{
    int P = (ins >> 24) & 1, U = (ins >> 23) & 1, B = (ins >> 22) & 1;
    int W = (ins >> 21) & 1, L = (ins >> 20) & 1;
    int rn = (ins >> 16) & 0xF, rd = (ins >> 12) & 0xF;
    uint32_t base = base_reg(cpu, rn, a);

    int32_t offset;
    if ((ins >> 25) & 1) { /* register offset, through the shifter (imm field = shift) */
        shift_result sh = shift(cpu, ins & 0x0FFFFFFu, a); /* I bit cleared: register form */
        offset = (int32_t)sh.v;
    } else {
        offset = (int32_t)(ins & 0xFFFu);
    }
    if (!U) offset = -offset;

    uint32_t addr = base;
    if (P) addr = addr + (uint32_t)offset;

    if (L) {
        uint32_t v;
        if (B) {
            v = arm26_rd8(cpu, addr);
        } else {
            uint32_t word = arm26_rd32(cpu, addr & ~3u); /* aligned read, then rotate */
            uint32_t r = (addr & 3) * 8;
            v = r ? ((word >> r) | (word << (32 - r))) : word; /* unaligned load rotates so byte@addr is LSB */
        }
        if (rd == 15) cpu->r[15] = (cpu->r[15] & ~PC_MASK) | (v & PC_MASK); /* LDR pc: jump, flags preserved */
        else set_reg(cpu, rd, v, 0);
    } else {
        uint32_t v = get_reg(cpu, rd, a, 0);
        if (B) arm26_wr8(cpu, addr, (uint8_t)(v & 0xFF));
        else   arm26_wr32(cpu, addr, v);
    }

    if (!P || W) set_reg(cpu, rn, base + (uint32_t)offset, 0); /* writeback (post) or P with W */
}

static void block_transfer(Arm26 *cpu, uint32_t a, uint32_t ins)
{
    int P = (ins >> 24) & 1, U = (ins >> 23) & 1, S = (ins >> 22) & 1;
    int W = (ins >> 21) & 1, L = (ins >> 20) & 1;
    int rn = (ins >> 16) & 0xF;
    uint32_t list = ins & 0xFFFFu;
    int priv = arm26_mode(cpu) != MODE_USR;
    if (list == 0) list = 0x8000; /* empty list codes r15 (rare; treated literally) */

    int count = 0;
    for (int i = 0; i < 16; i++) if (list & (1u << i)) count++;

    uint32_t base = base_reg(cpu, rn, a);
    /* ARM rule for all forms: lowest-numbered register <-> lowest address. */
    uint32_t delta;
    if (U) delta = P ? 4u : 0u;
    else   delta = (uint32_t)(-(int64_t)(4u * (uint32_t)(count - 1 + (P ? 1 : 0))));
    uint32_t addr = base + delta;
    uint32_t finalBase = base + (U ? (uint32_t)(count * 4) : (uint32_t)(-(int32_t)(count * 4)));

    int userBank = S && priv; /* S without r15 in list = user-bank transfer */

    for (int i = 0; i < 16; i++) {
        if ((list & (1u << i)) == 0) continue;
        if (L) {
            uint32_t v = arm26_rd32(cpu, addr);
            if (i == 15) {
                if (S && priv) write_pc_restore(cpu, v, 1); /* LDM ^ with PC: full CPSR restore */
                else cpu->r[15] = (cpu->r[15] & ~PC_MASK) | (v & PC_MASK);
            } else {
                set_reg(cpu, i, v, userBank);
            }
        } else {
            uint32_t v;
            if (i == 15) v = (cpu->r[15] & 0xFC000000u) | ((a + 8) & PC_MASK) | (cpu->r[15] & 3);
            else v = get_reg(cpu, i, a, userBank);
            arm26_wr32(cpu, addr, v);
        }
        addr = addr + 4;
    }
    if (W) set_reg(cpu, rn, finalBase, 0);
}

/* ARMv4 halfword/signed transfers: cond 000 P U I W L rn rd immHi 1 S H 1 immLo/rm */
static void halfword_transfer(Arm26 *cpu, uint32_t a, uint32_t ins)
{
    int P = (ins >> 24) & 1, U = (ins >> 23) & 1, I = (ins >> 22) & 1;
    int W = (ins >> 21) & 1, L = (ins >> 20) & 1;
    int S = (ins >> 6) & 1, H = (ins >> 5) & 1;
    int rn = (ins >> 16) & 0xF, rd = (ins >> 12) & 0xF;
    uint32_t base = base_reg(cpu, rn, a);

    int32_t offset;
    if (I) offset = (int32_t)(((ins >> 4) & 0xF0u) | (ins & 0xFu)); /* immHi:immLo */
    else   offset = (int32_t)get_reg(cpu, ins & 0xF, a, 0);          /* rm */
    if (!U) offset = -offset;

    uint32_t addr = base;
    if (P) addr = addr + (uint32_t)offset;

    if (L) {
        uint32_t v;
        if (S == 0 && H == 1) {        /* LDRH: zero-extended halfword */
            v = (uint32_t)arm26_rd8(cpu, addr) | ((uint32_t)arm26_rd8(cpu, addr + 1) << 8);
        } else if (S == 1 && H == 0) { /* LDRSB: sign-extended byte */
            v = (uint32_t)(int32_t)(int8_t)arm26_rd8(cpu, addr);
        } else if (S == 1 && H == 1) { /* LDRSH: sign-extended halfword */
            uint32_t h = (uint32_t)arm26_rd8(cpu, addr) | ((uint32_t)arm26_rd8(cpu, addr + 1) << 8);
            v = (uint32_t)(int32_t)(int16_t)h;
        } else {                      /* S=0,H=0,L=1: undefined */
            exception(cpu, VEC_UNDEF, MODE_SVC, 0);
            return;
        }
        if (rd == 15) cpu->r[15] = (cpu->r[15] & ~PC_MASK) | (v & PC_MASK);
        else set_reg(cpu, rd, v, 0);
    } else {
        if (!(S == 0 && H == 1)) {     /* only STRH exists (no STRSB/STRSH) */
            exception(cpu, VEC_UNDEF, MODE_SVC, 0);
            return;
        }
        uint32_t v = get_reg(cpu, rd, a, 0);
        arm26_wr8(cpu, addr, (uint8_t)(v & 0xFF));
        arm26_wr8(cpu, addr + 1, (uint8_t)((v >> 8) & 0xFF));
    }
    if (!P || W) set_reg(cpu, rn, base + (uint32_t)offset, 0);
}

static void execute(Arm26 *cpu, uint32_t a, uint32_t ins)
{
    if (!cond_ok(cpu, ins)) return;

    uint32_t grp = (ins >> 25) & 7;
    if ((ins & 0x0F000000u) == 0x0F000000u) { swi(cpu, a, ins); return; } /* 1111: SWI (before coproc catch!) */
    if (grp == 2) { single_transfer(cpu, a, ins); return; }  /* 010: LDR/STR */
    if (grp == 4) { block_transfer(cpu, a, ins); return; }   /* 100: LDM/STM */
    if (grp == 5) { branch(cpu, a, ins); return; }           /* 101: B/BL */
    if (grp == 3) {                                           /* 011: LDR/STR, register offset */
        if (((ins >> 4) & 1) == 0) { single_transfer(cpu, a, ins); return; }
        exception(cpu, VEC_UNDEF, MODE_SVC, 0); return;      /* bit 4 = 1: undefined before ARMv5 */
    }
    if (grp == 6 || grp == 7) {                               /* 110/111: coprocessor & ARMv3 extras */
        exception(cpu, VEC_UNDEF, MODE_SVC, 0);
        return;
    }

    /* grp === 0 / 1: data-processing space */
    if ((ins & 0x0FC000F0u) == 0x00000090u) { multiply(cpu, a, ins); return; } /* MUL/MLA: bits 27:22=0, bits 7:4=1001 */
    if ((ins & 0x0FB000F0u) == 0x01000090u) { swp(cpu, a, ins); return; }
    /* MRS pattern (ARMv3+; undefined on ARMv2a hardware) */
    if ((ins & 0x0FFFFFF0u) == 0x010F0000u) { exception(cpu, VEC_UNDEF, MODE_SVC, 0); return; }
    if (grp == 0 && (ins & 0x90u) == 0x90u) { halfword_transfer(cpu, a, ins); return; } /* ARMv4 halfword/signed */
    data_processing(cpu, a, ins);
}
