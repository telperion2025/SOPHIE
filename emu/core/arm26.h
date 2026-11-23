/*
 * arm26.h - ARMv2/ARMv3 26-bit interpreter core (WP1 C port).
 *
 * Mechanical C11 port of the JS reference implementation (spec Appendix A),
 * which is the tested groundwork for milestone M2. Target semantics:
 * Acorn ARMv2a/ARMv3 cores (ARM2, ARM250, ARM3, ARM610/710) - the 26-bit
 * PC+flags-in-r15 model used by all Archimedes / early Risc PC RISC OS
 * guests. No 32-bit modes (ARMv3 MRS/MSR are undefined, as on ARMv2a
 * hardware). All coprocessor instructions raise undefined-instruction
 * exceptions.
 *
 * r15 layout (26-bit model): bits 31:28 NZCV, bit 27 I, bit 26 F,
 * bits 25:2 PC, bits 1:0 mode. PC_MASK masks the legal PC field.
 *
 * All memory access goes through the mem_bus (WP2), so the core is
 * ISA-portable: no endianness or alignment assumptions (PowerPC plan,
 * spec section 3).
 *
 * Test-exit convention: SWI 0 halts the run loop without an exception.
 * ROM-side code never sees it.
 *
 * Licence: Apache-2.0 (see LICENSE). Implemented from the ARM ARM and the
 * specification only; no code derived from GPL'd emulators.
 */
#ifndef SOPHIE_ARM26_H
#define SOPHIE_ARM26_H

#include <stddef.h>
#include <stdint.h>

#include "mem.h"

#define FLAG_N 0x80000000u
#define FLAG_Z 0x40000000u
#define FLAG_C 0x20000000u
#define FLAG_V 0x10000000u
#define FLAG_I 0x08000000u
#define FLAG_F 0x04000000u
#define FLAG_NZCV 0xF0000000u

#define MODE_USR 0u
#define MODE_FIQ 1u
#define MODE_IRQ 2u
#define MODE_SVC 3u

#define PC_MASK 0x03FFFFFCu
/* 26-bit CPU: the address bus carries 26 bits, so flag bits read from r15
 * never reach memory decoding. Core accessors mask with ADDR_MASK. */
#define ADDR_MASK 0x03FFFFFFu

#define VEC_RESET 0x00u
#define VEC_UNDEF 0x04u
#define VEC_SWI   0x08u
#define VEC_PABT  0x0Cu
#define VEC_DABT  0x10u
#define VEC_IRQ   0x18u
#define VEC_FIQ   0x1Cu

typedef struct Arm26 {
    uint32_t r[16];        /* r15 doubles as the CPSR (flags|PC|mode) */
    uint32_t bankUsr[7];   /* USR r8-r14 (r8-r12 shared with SVC/IRQ) */
    uint32_t bankSvc[2];   /* SVC r13,r14 */
    uint32_t bankIrq[2];   /* IRQ r13,r14 */
    uint32_t bankFiq[7];   /* FIQ r8-r14 */
    int      irqPending;
    int      fiqPending;
    uint64_t cycles;
    int      halted;       /* set by SWI 0 (test-exit convention) */
    mem_bus *mem;
} Arm26;

void     arm26_init(Arm26 *cpu, mem_bus *mem);   /* constructor + reset */
void     arm26_reset(Arm26 *cpu);
uint32_t arm26_mode(const Arm26 *cpu);
uint32_t arm26_pc_operand(const Arm26 *cpu, uint32_t a);

/* memory access through the bus; a USR-mode data access to the machine's
 * supervisor-only I/O window (bus.io_base..) raises a data abort (VEC_DABT,
 * LR_svc = faulting pc+4, SVC mode) after the instruction instead of
 * reaching the device; instruction fetches are exempt */
uint8_t  arm26_rd8(Arm26 *cpu, uint32_t addr);
void     arm26_wr8(Arm26 *cpu, uint32_t addr, uint8_t v);
uint32_t arm26_rd32(Arm26 *cpu, uint32_t addr);
void     arm26_wr32(Arm26 *cpu, uint32_t addr, uint32_t v);

void     arm26_load_program(Arm26 *cpu, const uint32_t *words, size_t n, uint32_t base);

/* Execute up to n instructions; exceptions/interrupts may consume budget. */
void     arm26_run(Arm26 *cpu, uint64_t n);

#endif /* SOPHIE_ARM26_H */
