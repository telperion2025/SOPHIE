/**
 * bench.mjs - JS reference interpreter benchmark (spec Appendix A).
 * Mirrors emu/tests/bench.c so the C port can be compared on the same host
 * (WP1 acceptance: C benchmark within 3x of the JS reference).
 * Run: node ref/bench.mjs [instructions]
 */
import { Arm26 } from './arm26.mjs';

const n = BigInt(process.argv[2] ?? '1000000');
const cpu = new Arm26(0x40000);
cpu.loadProgram([
  0xE3A0000A,  // MOV r0,#10
  0xE3A01000,  // MOV r1,#0
  0xE0811000,  // ADD r1,r1,r0
  0xE2500001,  // SUBS r0,r0,#1
  0x1AFFFFFC,  // BNE 0x08
]);
cpu.run(3); // warm
const t0 = performance.now();
cpu.run(Number(n));
const t1 = performance.now();
const secs = (t1 - t0) / 1000;
const mips = cpu.cycles / secs / 1e6;
console.log(`js bench: ${cpu.cycles} instructions in ${(secs * 1e3).toFixed(1)} ms -> ${mips.toFixed(1)} MIPS (r1=${cpu.r[1]})`);
