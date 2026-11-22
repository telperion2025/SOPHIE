/**
 * arm26.test.mjs — semantics tests for the ARMv2/3 26-bit interpreter core.
 * Each test assembles its own words by hand (no external assembler) and
 * asserts registers, flags, memory, and exception-entry state.
 * Run: node arm26.test.mjs
 */
import { Arm26, FLAG_N, FLAG_Z, FLAG_C, FLAG_V, FLAG_I, FLAG_F,
         MODE_USR, MODE_SVC, MODE_IRQ, PC_MASK as PC_M } from './arm26.mjs';

let passed = 0, failed = 0;
const failures = [];

function check(name, cond, detail) {
  if (cond) { passed++; }
  else { failed++; failures.push(`${name}: ${detail}`); }
}

/** runs words from reset state (SVC, I+F set) with optional pre-state */
function boot(words, opts = {}) {
  const cpu = new Arm26(0x40000);
  cpu.loadProgram(words, opts.base ?? 0);
  if (opts.svcMode === false) { // drop to USR (clear I,F,mode)
    cpu.r[15] = (cpu.r[15] & ~0x0C000003) >>> 0;
  }
  for (const [k, v] of Object.entries(opts.set || {})) cpu.r[+k] = v >>> 0;
  for (const [addr, w] of Object.entries(opts.mem || {})) cpu.wr32(+addr, w >>> 0);
  return cpu;
}

// 1. MOV immediate -----------------------------------------------------
{
  const cpu = boot([0xE3A00011]);            // MOV r0, #0x11
  cpu.run(1);
  check('mov-imm', cpu.r[0] === 0x11, `r0=${cpu.r[0].toString(16)}`);
}

// 2. MOV with rotate ----------------------------------------------------
{
  const cpu = boot([0xE3A044FF]);            // MOV r4, #0xFF000000  (imm8 0xFF, rot 8)
  cpu.run(1);
  check('mov-rot-imm', cpu.r[4] === 0xFF000000, `r4=${cpu.r[4].toString(16)}`);
}

// 3. ADD reg + imm ------------------------------------------------------
{
  const cpu = boot([0xE3A00005,            // MOV r0, #5
                    0xE2801005]);          // ADD r1, r0, #5
  cpu.run(2);
  check('add', cpu.r[1] === 10, `r1=${cpu.r[1]}`);
}

// 4. SUB reg & flags ----------------------------------------------------
{
  const cpu = boot([0xE0402000]);           // SUB r2, r0, r0
  cpu.r[0] = 0x12345678;
  cpu.run(1);
  check('sub-neg-flags', cpu.r[2] === 0 && !(cpu.r[15] & FLAG_C),
        `r2=${cpu.r[2]}, C=${!!(cpu.r[15] & FLAG_C)} (expect borrow → C clear)`);
}

// 5. CMP sets Z ----------------------------------------------------------

function t(name, fn) {
  const cpu = boot([]);
  cpu.loadProgram([], 0);
  try { fn(cpu); } catch (e) { failed++; failures.push(`${name}: threw ${e.message}`); }
}

t('branch-taken', (cpu) => {
  cpu.loadProgram([
    0xE3A00000,  // 0x00 MOV r0,#0
    0xE3500000,  // 0x04 CMP r0,#0 → Z
    0x0A000000,  // 0x08 BEQ +0 → target = 0x08+8+0 = 0x10 (skips MOV r1 at 0x0C)
    0xE3A01001,  // 0x0C MOV r1,#1 (skipped)
    0xE3A02002,  // 0x10 MOV r2,#2
  ]);
  cpu.run(4);
  check('branch-eq-taken', cpu.r[1] === 0 && cpu.r[2] === 2,
        `r1=${cpu.r[1]} r2=${cpu.r[2]} (r1 must be skipped)`);
});

t('branch-not-taken', (cpu) => {
  cpu.loadProgram([
    0xE3A00000,  // MOV r0,#0
    0xE3500001,  // CMP r0,#1 → no Z
    0x0A000000,  // BEQ (not taken: no Z) → falls through
    0xE3A02002,  // 0x0C executed on fall-through
  ]);
  cpu.run(4);
  check('beq-fallthrough', cpu.r[2] === 2, `r2=${cpu.r[2]}`);
});

t('mul', (cpu) => {
  cpu.loadProgram([
    0xE3A01007,  // MOV r1,#7
    0xE3A02006,  // MOV r2,#6
    0xE0000921,  // MUL r0,r1,r2
  ]);
  cpu.run(3);
  check('mul-42', cpu.r[0] === 42, `r0=${cpu.r[0]}`);
});

t('movs-lsl-carry', (cpu) => {
  cpu.loadProgram([
    0xE3A01003,  // MOV r1,#3 (bit1 set so LSL#31 shifts it into carry)
    0xE1B07F81,  // MOVS r7,r1,LSL #31
  ]);
  cpu.run(2);
  check('lsl-val', cpu.r[7] === 0x80000000, `r7=${cpu.r[7].toString(16)}`);
  check('lsl-flags', (cpu.r[15] & FLAG_N) && (cpu.r[15] & FLAG_C),
        `N=${!!(cpu.r[15] & FLAG_N)} C=${!!(cpu.r[15] & FLAG_C)} (C = old bit1)`);
});

t('adc-chain', (cpu) => {
  cpu.loadProgram([
    0xE3E05000,  // MVN r5,#0   → 0xFFFFFFFF
    0xE2956001,  // ADDS r6,r5,#1 → 0, Z|C
    0xE2A67000,  // ADC r7,r6,#0 → 0 + 0 + C = 1
  ]);
  cpu.run(3);
  check('adds-zc', (cpu.r[15] & FLAG_Z) && (cpu.r[15] & FLAG_C),
        `Z=${!!(cpu.r[15] & FLAG_Z)} C=${!!(cpu.r[15] & FLAG_C)}`);
  check('adc-val', cpu.r[7] === 1, `r7=${cpu.r[7]}`);
});

t('tst-z', (cpu) => {
  cpu.loadProgram([
    0xE3A080F0,  // MOV r8,#0xF0
    0xE318800F,  // TST r8,#0x0F → 0 → Z
  ]);
  cpu.run(2);
  check('tst-flags', (cpu.r[15] & FLAG_Z) !== 0, 'TST should set Z');
  check('tst-noclobber', cpu.r[8] === 0xF0, `r8=${cpu.r[8].toString(16)}`);
});

t('bic', (cpu) => {
  cpu.r[0] = 0xFF; cpu.r[1] = 0x0F;
  cpu.loadProgram([0xE1C03001]); // BIC r3,r0,r1
  cpu.r[0] = 0xFF; cpu.r[1] = 0x0F;
  cpu.run(1);
  check('bic-val', cpu.r[3] === 0xF0, `r3=${cpu.r[3].toString(16)}`);
});

// memory: single transfers
t('str-ldr-rt', (cpu) => {
  cpu.loadProgram([
    0xE3A02D20,  // MOV r2,#0x80?? see below — replaced: use direct set
    0xE5810000,  // STR r0,[r1]
    0xE5914000,  // LDR r4,[r1]
  ]);
  cpu.r[0] = 0xDEADBEEF; cpu.r[1] = 0x1000;
  cpu.run(2); // STR only
  const stored = cpu.rd32(0x1000);
  check('str-word', stored === 0xDEADBEEF, `mem=${stored.toString(16)}`);
  cpu.run(1);
  check('ldr-word', cpu.r[4] === 0xDEADBEEF, `r4=${cpu.r[4].toString(16)}`);
});

t('ldr-unaligned-rotate', (cpu) => {
  cpu.wr32(0x100, 0x12345678);
  cpu.r[2] = 0x100;
  cpu.loadProgram([0xE5920001], 0x200); // LDR r0,[r2,#1] at 0x200
  cpu.r[15] = (cpu.r[15] & ~PC_M) | 0x200;
  cpu.run(1);
  check('ldr-rot', cpu.r[0] === 0x78123456, `r0=${cpu.r[0].toString(16)}`);
});

t('ldrb-strb', (cpu) => {
  cpu.wr32(0x100, 0xAABBCCDD);
  cpu.r[2] = 0x100;
  cpu.loadProgram([0xE5D20000], 0x200); // LDRB r0,[r2]
  cpu.r[15] = (cpu.r[15] & ~PC_M) | 0x200;
  cpu.run(1);
  check('ldrb', cpu.r[0] === 0xDD, `r0=${cpu.r[0].toString(16)}`);
  cpu.r[1] = 0xEE; cpu.r[3] = 0x200;
  cpu.loadProgram([0xE5C31000], 0x204); // STRB r1,[r3,#0]
  cpu.r[15] = (cpu.r[15] & ~PC_M) | 0x204;
  cpu.run(1);
  check('strb', cpu.rd8(0x200) === 0xEE, `mem=${cpu.rd8(0x200).toString(16)}`);
});

t('stmfd-ldmfd', (cpu) => {
  cpu.loadProgram([
    0xE3A00001,  // MOV r0,#1
    0xE3A01002,  // MOV r1,#2
    0xE3A02003,  // MOV r2,#3
    0xE3A0D801,  // MOV r13,#0x200000?? replaced below
  ]);
  const c = cpu;
  c.r[0] = 1; c.r[1] = 2; c.r[2] = 3; c.r[13] = 0x200;
  c.loadProgram([0xE92D0007], 0x300); // STMFD r13!,{r0-r2}
  c.r[15] = (c.r[15] & ~PC_M) | 0x300;
  c.run(1);
  check('stmfd-base', c.r[13] === 0x1F4, `r13=${c.r[13].toString(16)}`);
  check('stmfd-mem', c.rd32(0x1F4) === 1 && c.rd32(0x1F8) === 2 && c.rd32(0x1FC) === 3,
        'stack contents order');
  c.loadProgram([0xE8BD0070], 0x304); // LDMFD r13!,{r4,r5,r6}
  c.r[15] = (c.r[15] & ~PC_M) | 0x304;
  c.run(1);
  check('ldmfd-vals', c.r[4] === 1 && c.r[5] === 2 && c.r[6] === 3, 'restore regs');
  check('ldmfd-base', c.r[13] === 0x200, `r13=${c.r[13].toString(16)}`);
});

t('stm-stores-cpsr-in-r15', (cpu) => {
  cpu.r[1] = 0x100; cpu.r[0] = 0x77;
  cpu.loadProgram([0xE8818000], 0x200); // STM r1,{r15}
  cpu.r[15] = (cpu.r[15] & ~PC_M) | 0x200;
  cpu.run(1);
  const stored = cpu.rd32(0x100);
  // stored = flags | pc(=0x208) | mode(SVC=3)
  const expect = (FLAG_I | FLAG_F | 0x208 | MODE_SVC) >>> 0;
  check('stm-r15-cpsr', stored === expect,
        `stored=${stored.toString(16)} expect=${expect.toString(16)}`);
});

t('swp', (cpu) => {
  cpu.wr32(0x1000, 0xAA551212);
  cpu.r[1] = 0x99; cpu.r[2] = 0x1000;
  cpu.loadProgram([0xE1020091], 0x200); // SWP r0,r1,[r2]
  cpu.r[15] = (cpu.r[15] & ~PC_M) | 0x200;
  cpu.run(1);
  check('swp-old', cpu.r[0] === 0xAA551212, `r0=${cpu.r[0].toString(16)}`);
  check('swp-new', cpu.rd32(0x1000) === 0x99, `mem=${cpu.rd32(0x1000).toString(16)}`);
});

// exceptions
t('swi-entry', (cpu) => {
  cpu.loadProgram([
    0xE3A02000,  // 0x00 MOV r2,#0
    0xEF000001,  // 0x04 SWI 1
  ]);
  cpu.run(2);
  check('swi-pc', (cpu.r[15] & PC_M) === 0x08 && (cpu.r[15] & 3) === MODE_SVC,
        `pc=${(cpu.r[15] & PC_M).toString(16)} mode=${cpu.r[15] & 3}`);
  check('swi-I', (cpu.r[15] & FLAG_I) !== 0, 'I set on SVC entry');
  check('swi-lr', cpu.r[14] === ((FLAG_I | FLAG_F | 0x08 | MODE_SVC) >>> 0),
        `lr=${cpu.r[14].toString(16)} (live r14 in SVC: cpsr at SWI, pc=0x08, I|F)`);
});

t('swi-ret-movs-pc', (cpu) => {
  cpu.loadProgram([
    0xEF000001,  // 0x00 SWI 1 → enter SVC → pc=8
    0xE3A04009,  // 0x04 MOV r4,#9 (return address; never executed by run(2))
    0xE1B0F00E,  // 0x08 MOVS pc, r14  → return to 0x04
  ]);
  cpu.run(2);
  check('rti-pc', (cpu.r[15] & PC_M) === 0x04, `pc=${(cpu.r[15] & PC_M).toString(16)}`);
  check('rti-mode', (cpu.r[15] & 3) === MODE_SVC, `mode=${cpu.r[15] & 3}`);
  check('rti-IF-from-lr', (cpu.r[15] & (FLAG_I | FLAG_F)) === (FLAG_I | FLAG_F),
        'MOVS pc,lr is a full CPSR restore: I|F come from lr_svc');
  check('rti-r4-untouched', cpu.r[4] === 0, 'instruction after return not executed');
});

t('undef-mrs', (cpu) => {
  cpu.loadProgram([0xE10F0000], 0); // MRS pattern → undefined trap
  cpu.r[15] = (cpu.r[15] & ~PC_M) | 0;
  cpu.run(1);
  check('undef-entry', (cpu.r[15] & PC_M) === 0x04 && (cpu.r[15] & FLAG_I) !== 0,
        `pc=${(cpu.r[15] & PC_M).toString(16)}`);
  check('undef-lr', cpu.r[14] === ((FLAG_I | FLAG_F | 0x04 | MODE_SVC) >>> 0),
        `lr=${cpu.r[14].toString(16)} (live r14: cpsr at trap, pc=4, I|F, SVC)`);
});

t('irq-entry', (cpu) => {
  cpu.loadProgram([0xEAFFFFFE], 0); // B . (spin)
  cpu.r[15] &= ~FLAG_I;             // enable IRQs (reset leaves I set)
  cpu.irqPending = true;            // raise between instructions
  cpu.run(1);                       // one instruction then the interrupt is serviced
  check('irq-pc', (cpu.r[15] & PC_M) === 0x18 && (cpu.r[15] & FLAG_I) !== 0,
        `pc=${(cpu.r[15] & PC_M).toString(16)}`);
  check('irq-keep-F', (cpu.r[15] & FLAG_F) !== 0, 'F preserved from pre-IRQ cpsr');
  check('irq-mode', (cpu.r[15] & 3) === MODE_IRQ, `mode=${cpu.r[15] & 3}`);
});

t('teqp-mode-switch', (cpu) => {
  // start in SVC (as after reset), switch to USR by TEQP r0,#0
  cpu.r[0] = 0;
  cpu.loadProgram([0xE330F000], 0); // TEQP r0,#0
  cpu.r[15] = (cpu.r[15] & ~PC_M) | 0;
  cpu.run(1);
  check('teqp-usr', (cpu.r[15] & 3) === MODE_USR,
        `mode=${cpu.r[15] & 3} cpsr=${cpu.r[15].toString(16)}`);
});

// loop: sum 1..10
t('sum-loop', (cpu) => {
  cpu.loadProgram([
    0xE3A0000A,  // 0x00 MOV r0,#10
    0xE3A01000,  // 0x04 MOV r1,#0
    0xE0811000,  // 0x08 ADD r1,r1,r0
    0xE2500001,  // 0x0C SUBS r0,r0,#1
    0x1AFFFFFC,  // 0x10 BNE 0x08
  ]);
  cpu.run(2 + 30);
  check('sum-55', cpu.r[1] === 55, `r1=${cpu.r[1]}`);
  check('sum-Z', (cpu.r[15] & FLAG_Z) !== 0, 'loop ended with Z set');
});

t('swi0-halts', (cpu) => {
  cpu.loadProgram([0xEF000000, 0xE3A00001]); // SWI 0 ; MOV r0,#1
  cpu.run(10);
  check('halt', cpu.halted && cpu.r[0] === 0, 'SWI 0 stops the run loop');
});

console.log(`\narm26: ${passed} passed, ${failed} failed`);
if (failures.length) {
  console.log('failures:');
  for (const f of failures) console.log('  ✗ ' + f);
  process.exit(1);
}

