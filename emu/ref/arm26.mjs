/**
 * arm26 — ARMv2/ARMv3 26-bit interpreter core (reference implementation)
 *
 * Project: "Developing a small OS" — emulator tier, milestones M1/M2.
 * Target semantics: Acorn ARMv2a/ARMv3 cores (ARM2, ARM250, ARM3, ARM610/710),
 * i.e. the 26-bit PC+flags-in-r15 model used by all Archimedes / early Risc PC
 * RISC OS guests. No 32-bit modes (ARMv3 MRS/MSR handled as undefined-opcode,
 * exactly how ARMv2a hardware treats them).
 *
 * Reference for a future C port: this file is deliberately C-shaped
 * (no closures over loop state, explicit masks, single flat memory array).
 *
 * Not implemented yet (device models are separate): IOMD/IOMD1, VIDC, FPA.
 * All coprocessor instructions raise undefined-instruction exceptions.
 */

export const FLAG_N = 0x80000000;
export const FLAG_Z = 0x40000000;
export const FLAG_C = 0x20000000;
export const FLAG_V = 0x10000000;
export const FLAG_I = 0x08000000;
export const FLAG_F = 0x04000000;
export const FLAG_NZCV = 0xF0000000;

export const MODE_USR = 0;
export const MODE_FIQ = 1;
export const MODE_IRQ = 2;
export const MODE_SVC = 3;

export const PC_MASK = 0x03FFFFFC;

const VEC_RESET = 0x00;
const VEC_UNDEF = 0x04;
const VEC_SWI   = 0x08;
const VEC_PABT  = 0x0C;
const VEC_DABT  = 0x10;
const VEC_IRQ   = 0x18;
const VEC_FIQ   = 0x1C;

const OPCODES = ['AND','EOR','SUB','RSB','ADD','ADC','SBC','RSC',
                 'TST','TEQ','CMP','CMN','ORR','MOV','BIC','MVN'];

export class Arm26 {
  constructor(ramSize = 0x100000) {
    if ((ramSize & (ramSize - 1)) !== 0) throw new Error('ramSize must be a power of two');
    this.ramSize = ramSize;
    this.ramMask = ramSize - 1;
    this.ram = new Uint8Array(ramSize);

    this.r = new Uint32Array(16);          // r15 doubles as the CPSR (flags|PC|mode)
    this.bankUsr = new Uint32Array(7);     // USR r8-r14 (also SVC/IRQ r8-r12)
    this.bankSvc = new Uint32Array(2);    // SVC r13,r14
    this.bankIrq = new Uint32Array(2);    // IRQ r13,r14
    this.bankFiq = new Uint32Array(7);    // FIQ r8-r14
    this.irqPending = false;
    this.fiqPending = false;
    this.cycles = 0;
    this.halted = false;                   // set by SWI 0 (test-exit convention)
    this.reset();
  }

  /* ---------- model identity ---------- */

  get mode() { return this.r[15] & 3; }

  /** r15 as it appears when read as an operand during the instruction at a */
  pcOperand(a) {
    return (this.r[15] & 0xFC000000) | (((a + 8) & PC_MASK)) | (this.r[15] & 3);
  }

  /* ---------- reset / exceptions ---------- */

  reset() {
    this.r.fill(0);
    this.bankUsr.fill(0); this.bankSvc.fill(0);
    this.bankIrq.fill(0); this.bankFiq.fill(0);
    // r15 = I,F set, SVC mode, pc 0 (ARMv3 26-bit reset state)
    this.r[15] = (FLAG_I | FLAG_F | MODE_SVC) >>> 0;
    this.irqPending = false;
    this.fiqPending = false;
    this.cycles = 0;
    this.halted = false;
  }

  _bankFor(mode, i) { // i in 8..14
    if (mode === MODE_FIQ) return [this.bankFiq, i - 8];
    if (i <= 12) return [this.bankUsr, i - 8];              // shared r8-r12
    if (i === 13) return mode === MODE_SVC ? [this.bankSvc, 0]
                      : mode === MODE_IRQ ? [this.bankIrq, 0]
                      : [this.bankUsr, 5];
    return mode === MODE_SVC ? [this.bankSvc, 1]
         : mode === MODE_IRQ ? [this.bankIrq, 1]
         : [this.bankUsr, 6];
  }

  _setMode(m) {
    const old = this.mode;
    if (old === m) return;
    for (let i = 8; i < 15; i++) { const b = this._bankFor(old, i); b[0][b[1]] = this.r[i]; }
    for (let i = 8; i < 15; i++) { const b = this._bankFor(m, i); this.r[i] = b[0][b[1]]; }
  }

  /**
   * 26-bit exception entry.
   * banked r14_mode := current CPSR (flags | pc | old mode)
   * CPSR := (NZCV preserved) | (I set; F set for FIQ) | vector | new mode
   */
  _exception(vector, newMode, setF) {
    const oldCPSR = this.r[15];
    this._setMode(newMode);
    this.r[14] = oldCPSR >>> 0;
    const nzcv = oldCPSR & FLAG_NZCV;
    const fiqBit = setF ? FLAG_F : (oldCPSR & FLAG_F);
    this.r[15] = (nzcv | FLAG_I | fiqBit | vector | newMode) >>> 0;
  }

  /* ---------- memory (flat RAM; MMIO is a later milestone) ---------- */

  rd8(a) { return this.ram[a & this.ramMask]; }
  wr8(a, v) { this.ram[a & this.ramMask] = v & 0xFF; }

  rd32(a) {
    a &= this.ramMask;
    return (this.ram[a] | (this.ram[a + 1] << 8) | (this.ram[a + 2] << 16) | (this.ram[a + 3] << 24)) >>> 0;
  }
  wr32(a, v) {
    a &= this.ramMask;
    this.ram[a] = v & 0xFF; this.ram[a + 1] = (v >>> 8) & 0xFF;
    this.ram[a + 2] = (v >>> 16) & 0xFF; this.ram[a + 3] = (v >>> 24) & 0xFF;
  }

  loadProgram(words, base = 0) {
    for (let i = 0; i < words.length; i++) this.wr32(base + i * 4, words[i] >>> 0);
  }

  /* ---------- register access ---------- */

  /** operand read; bankSel forces USR-bank view for LDM/STM user-mode transfers */
  _getReg(i, a, bankSel) {
    if (i === 15) return this.pcOperand(a);
    if (!bankSel) return this.r[i];
    if (i >= 8 && i <= 14) { const b = this._bankFor(MODE_USR, i); return b[0][b[1]]; }
    return this.r[i];
  }
  _setReg(i, v, bankSel) {
    if (i === 15) return; // r15 writes go through _writePC paths
    if (bankSel && i >= 8 && i <= 14) { const b = this._bankFor(MODE_USR, i); b[0][b[1]] = v >>> 0; return; }
    this.r[i] = v >>> 0;
  }

  /** MOVS-style r15 write from a logical ALU result: full CPSR replace (if priv) */
  _writePCLogical(v) {
    if (this.mode !== MODE_USR) {
      this.r[15] = ((v & (FLAG_NZCV | FLAG_I | FLAG_F)) | (v & PC_MASK) | (v & 3)) >>> 0;
    } else {
      // unprivileged: NZCV change; I,F,mode protected
      this.r[15] = ((v & FLAG_NZCV) | (this.r[15] & (FLAG_I | FLAG_F)) | (v & PC_MASK) | MODE_USR) >>> 0;
    }
  }

  /** MOVS-style r15 write from an arithmetic ALU or LDM-with-S load:
   *  NZCV from value, I,F and mode preserved (unless LDM^ which restores all)
   *  kind: 'arith' (NZCV from computed flags, IF+mode kept)
   *        'restore' (full CPSR from value, if priv) */
  _writePCRestore(v, kind) {
    if (kind === 'restore' && this.mode !== MODE_USR) {
      this.r[15] = ((v & (FLAG_NZCV | FLAG_I | FLAG_F)) | (v & PC_MASK) | (v & 3)) >>> 0;
      return;
    }
    this.r[15] = ((v & FLAG_NZCV) | (this.r[15] & (FLAG_I | FLAG_F | 3)) | (v & PC_MASK)) >>> 0;
  }

  /* ---------- condition codes ---------- */

  _condOK(ins) {
    const f = this.r[15];
    switch ((ins >>> 28) & 0xF) {
      case 0x0: return (f & FLAG_Z) !== 0;
      case 0x1: return (f & FLAG_Z) === 0;
      case 0x2: return (f & FLAG_C) !== 0;
      case 0x3: return (f & FLAG_C) === 0;
      case 0x4: return (f & FLAG_N) !== 0;
      case 0x5: return (f & FLAG_N) === 0;
      case 0x6: return (f & FLAG_V) !== 0;
      case 0x7: return (f & FLAG_V) === 0;
      case 0x8: return (f & FLAG_C) !== 0 && (f & FLAG_Z) === 0;
      case 0x9: return (f & FLAG_C) === 0 || (f & FLAG_Z) !== 0;
      case 0xa: return !!(f & FLAG_N) === !!(f & FLAG_V);
      case 0xb: return !!(f & FLAG_N) !== !!(f & FLAG_V);
      case 0xc: return (f & FLAG_Z) === 0 && !!(f & FLAG_N) === !!(f & FLAG_V);
      case 0xd: return (f & FLAG_Z) !== 0 || !!(f & FLAG_N) !== !!(f & FLAG_V);
      case 0xe: return true;
      default:  return false; // NV: never in 26-bit ARM
    }
  }

  /* ---------- barrel shifter ---------- */

  /** returns [value, carryOut] ; carryOut === undefined means "leave C alone" */
  _shift(ins, a) {
    if ((ins >>> 25) & 1) { // immediate
      const rot = (ins >>> 7) & 0x1E;
      const imm = ins & 0xFF;
      if (rot === 0) return [imm, undefined];
      const v = ((imm >>> rot) | (imm << (32 - rot))) >>> 0;
      return [v, (imm >>> (rot - 1)) & 1];
    }
    const rm = ins & 0xF;
    const val = this._getReg(rm, a, false);
    const typ = (ins >>> 5) & 3;
    if (((ins >>> 4) & 1) === 0) { // shift by immediate amount (bits 11-7)
      let n = (ins >>> 7) & 0x1F;
      if (n === 0) {
        if (typ === 0) return [val, undefined];        // LSL #0
        if (typ === 3) {                                // ROR #0 === RRX
          const c = (this.r[15] & FLAG_C) ? 1 : 0;
          const v = ((val >>> 1) | (c << 31)) >>> 0;
          return [v, val & 1];
        }
        n = 32; // LSR/ASR #0 behave as #32
      }
      return this._applyShift(val, typ, n);
    }
    // shift by register
    const rs = (ins >>> 8) & 0xF;
    const n = this._getReg(rs, a, false) & 0xFF;
    if (typ === 0 && n === 0) return [val, undefined];   // LSL #0
    return this._applyShift(val, typ, n);
  }

  _applyShift(val, typ, n) {
    switch (typ) {
      case 0: { // LSL
        if (n === 0) return [val, undefined];
        if (n < 32) { const c = (val >>> (32 - n)) & 1; const v = (val << n) >>> 0; return [v, c]; }
        if (n === 32) return [0, val & 1];
        return [0, 0];
      }
      case 1: { // LSR
        if (n === 0) return [val, undefined];
        if (n < 32) { const c = (val >>> (n - 1)) & 1; const v = val >>> n; return [v, c]; }
        if (n === 32) return [0, (val >>> 31) & 1];
        return [0, 0];
      }
      case 2: { // ASR
        if (n === 0) return [val, undefined];
        const s = (val & 0x80000000) ? 0xFFFFFFFF : 0;
        if (n < 32) { const c = (val >>> (n - 1)) & 1; const v = (val >>> n) | (s << (32 - n)); return [v >>> 0, c]; }
        return [(s >>> 0), (val >>> 31) & 1];
      }
      default: { // ROR
        if (n === 0) n = 32;
        const k = n & 31;
        if (k === 0) return [val, (val >>> 31) & 1];
        const v = ((val >>> k) | (val << (32 - k))) >>> 0;
        return [v, (val >>> (k - 1)) & 1];
      }
    }
  }

  /* ---------- main loop ---------- */

  /** Execute up to n instructions (exceptions/interrupts may consumed budget) */
  run(n) {
    while (n-- > 0 && !this.halted) {
      const a = this.r[15] & PC_MASK;
      const ins = this.rd32(a);
      this.r[15] = (this.r[15] & ~PC_MASK) | ((a + 4) & PC_MASK);
      this.cycles++;
      this._execute(a, ins);
      this._serviceInterrupts();
    }
  }

  _serviceInterrupts() {
    if (this.halted) return;
    const f = this.r[15];
    if (this.fiqPending && !(f & FLAG_F)) { this.fiqPending = false; this._exception(VEC_FIQ, MODE_FIQ, true); return; }
    if (this.irqPending && !(f & FLAG_I)) { this.irqPending = false; this._exception(VEC_IRQ, MODE_IRQ, false); }
  }

  _execute(a, ins) {
    if (!this._condOK(ins)) return;

    const grp = (ins >> 25) & 7;
    if ((ins & 0x0F000000) === 0x0F000000) return this._swi(a, ins); // 1111: SWI (before coproc catch!)
    if (grp === 2) return this._singleTransfer(a, ins);      // 010: LDR/STR
    if (grp === 4) return this._blockTransfer(a, ins);       // 100: LDM/STM
    if (grp === 5) return this._branch(a, ins);              // 101: B/BL
    if (grp === 3 || grp === 6 || grp === 7)                 // 011/110/111: coproc & ARMv3 extras
      return this._exception(VEC_UNDEF, MODE_SVC, false);

    // grp === 0 / 1: data-processing space
    if ((ins & 0x0F000F00) === 0x00000900) return this._multiply(a, ins);
    if ((ins & 0x0FB000F0) === 0x01000090) return this._swp(a, ins);
    // MRS pattern (ARMv3+; undefined on ARMv2a hardware)
    if ((ins & 0x0FFFFFF0) === 0x010F0000) return this._exception(VEC_UNDEF, MODE_SVC, false);
    return this._dataProcessing(a, ins);
  }

  /* ---------- instruction groups ---------- */

  _branch(a, ins) {
    const off = (ins & 0x00FFFFFF) << 2; // note: top bits from imm24, no sign issue past 26-bit PC
    // sign-extend the 26-bit offset: PC bits are 25:2 of CPSR
    const sign = (off & 0x02000000) ? 0xFC000000 : 0;
    const target = ((a + 8 + off + sign) >>> 0) & PC_MASK;          // wraps within 26-bit space? keep PC field legal
    if ((ins >>> 24) & 1) { // BL
      this.r[14] = ((this.r[15] & 0xFC000000) | (((a + 4) & PC_MASK)) | (this.r[15] & 3)) >>> 0;
    }
    this.r[15] = (this.r[15] & ~PC_MASK) | ((target + 0) & PC_MASK) >>> 0;
  }

  _swi(a, ins) {
    // Test-exit convention: SWI 0 halts the run loop (no exception).
    if ((ins & 0x00FFFFFF) === 0) { this.halted = true; return; }
    this._exception(VEC_SWI, MODE_SVC, false);
  }

  _multiply(a, ins) {
    const acc = (ins >>> 21) & 1, S = (ins >>> 20) & 1;
    const rd = (ins >>> 16) & 0xF, rn = (ins >>> 12) & 0xF;
    const rs = (ins >>> 4) & 0xF, rm = ins & 0xF;
    let result = Math.imul(this._getReg(rm, a, false) | 0, this._getReg(rs, a, false) | 0) >>> 0;
    if (acc) result = (result + this._getReg(rn, a, false)) >>> 0;
    this._setReg(rd, result, false);
    if (S && rd !== 15) {
      let f = this.r[15] & ~FLAG_NZCV;
      if (result & 0x80000000) f |= FLAG_N;
      if (result === 0) f |= FLAG_Z;
      this.r[15] = f >>> 0;
    }
  }

  _swp(a, ins) {
    const B = (ins >>> 22) & 1;
    const rn = (ins >>> 16) & 0xF, rd = (ins >>> 12) & 0xF, rm = ins & 0xF;
    const addr = this._getReg(rn, a, false);
    let old;
    if (B) { old = this.rd8(addr); this.wr8(addr, this._getReg(rm, a, false) & 0xFF); }
    else   { old = this.rd32(addr); this.wr32(addr, this._getReg(rm, a, false)); }
    this._setReg(rd, old, false);
  }

  _dataProcessing(a, ins) {
    const opcode = (ins >>> 21) & 0xF;
    const S = (ins >>> 20) & 1;
    const rn = (ins >>> 16) & 0xF;
    const rd = (ins >>> 12) & 0xF;
    const op1 = this._getReg(rn, a, false);
    const [op2, shCarry] = this._shift(ins, a);
    let result = 0;
    let flagsC = (this.r[15] & FLAG_C) !== 0; // current C if shifter leaves it alone
    let flagsV = (this.r[15] & FLAG_V) !== 0;
    let isArith = false;

    switch (opcode) {
      case 0x0: result = (op1 & op2) >>> 0; break;
      case 0x1: result = (op1 ^ op2) >>> 0; break;
      case 0x2: { const u1 = op1 >>> 0, u2 = op2 >>> 0; result = (u1 - u2) >>> 0;
                  flagsC = u1 >= u2; const s1 = op1 | 0, s2 = op2 | 0;
                  flagsV = (s1 < 0) !== (s2 < 0) && ((result | 0) < 0) !== (s1 < 0); isArith = true; break; }
      case 0x3: { const u1 = op1 >>> 0, u2 = op2 >>> 0; result = (u2 - u1) >>> 0;
                  flagsC = u2 >= u1; const s1 = op1 | 0, s2 = op2 | 0;
                  flagsV = (s1 < 0) !== (s2 < 0) && ((result | 0) < 0) !== (s2 < 0); isArith = true; break; }
      case 0x4: { const sum = ((op1 >>> 0) + (op2 >>> 0)) >>> 0; result = sum;
                  flagsC = (op1 >>> 0) + (op2 >>> 0) > 0xFFFFFFFF; const s1 = op1 | 0, s2 = op2 | 0;
                  flagsV = (s1 < 0) === (s2 < 0) && ((result | 0) < 0) !== (s1 < 0); isArith = true; break; }
      case 0x5: { const cin = (this.r[15] & FLAG_C) ? 1 : 0;
                  const sum = ((op1 >>> 0) + (op2 >>> 0) + cin) >>> 0; result = sum;
                  flagsC = (op1 >>> 0) + (op2 >>> 0) + cin > 0xFFFFFFFF; const s1 = op1 | 0, s2 = op2 | 0;
                  flagsV = (s1 < 0) === (s2 < 0) && ((result | 0) < 0) !== (s1 < 0); isArith = true; break; }
      case 0x6: { const cin = (this.r[15] & FLAG_C) ? 1 : 0;
                  const total = (op1 >>> 0) + ((~op2) >>> 0) + cin; result = total >>> 0;
                  flagsC = total > 0xFFFFFFFF; const s1 = op1 | 0, s2 = op2 | 0;
                  flagsV = (s1 < 0) !== (s2 < 0) && ((result | 0) < 0) !== (s1 < 0); isArith = true; break; }
      case 0x7: { const cin = (this.r[15] & FLAG_C) ? 1 : 0;
                  const total = (op2 >>> 0) + ((~op1) >>> 0) + cin; result = total >>> 0;
                  flagsC = total > 0xFFFFFFFF; const s1 = op1 | 0, s2 = op2 | 0;
                  flagsV = (s1 < 0) !== (s2 < 0) && ((result | 0) < 0) !== (s2 < 0); isArith = true; break; }
      case 0x8: result = (op1 & op2) >>> 0; break;   // TST
      case 0x9: result = (op1 ^ op2) >>> 0; break;   // TEQ
      case 0xa: { const u1 = op1 >>> 0, u2 = op2 >>> 0; result = (u1 - u2) >>> 0;
                  flagsC = u1 >= u2; const s1 = op1 | 0, s2 = op2 | 0;
                  flagsV = (s1 < 0) !== (s2 < 0) && ((result | 0) < 0) !== (s1 < 0); isArith = true; break; } // CMP
      case 0xb: { const sum = ((op1 >>> 0) + (op2 >>> 0)) >>> 0; result = sum;
                  flagsC = (op1 >>> 0) + (op2 >>> 0) > 0xFFFFFFFF; const s1 = op1 | 0, s2 = op2 | 0;
                  flagsV = (s1 < 0) === (s2 < 0) && ((result | 0) < 0) !== (s1 < 0); isArith = true; break; } // CMN
      case 0xc: result = (op1 | op2) >>> 0; break;
      case 0xd: result = op2 >>> 0; break;          // MOV
      case 0xe: result = (op1 & ~op2) >>> 0; break;  // BIC
      default:  result = (~op2) >>> 0; break;        // MVN
    }

    const isCompare = opcode >= 0x8 && opcode <= 0xB;
    const writesReg = !isCompare;
    const target = isCompare ? -1 : rd;

    if (S) {
      if (shCarry !== undefined) flagsC = shCarry;
      let f = this.r[15] & ~FLAG_NZCV;
      if (result & 0x80000000) f |= FLAG_N;
      if (result === 0) f |= FLAG_Z;
      if (flagsC) f |= FLAG_C;
      if (flagsV) f |= FLAG_V;
      this.r[15] = f >>> 0;
    }

    if (rd === 15 && S) {
      // r15 as ALU destination with S: 26-bit CPSR semantics.
      // TEQP-style (TST/TEQ/logical with rd=15): the *operand* replaces the CPSR
      // (flags AND I/F AND mode, subject to privilege).
      // Arithmetic (SUBS pc, lr etc): NZCV from the op; I/F/mode preserved.
      if (isArith) this._writePCRestore(this.r[15], 'arith');
      else this._writePCLogical(result);
      return;
    }
    if (target === 15) {
      // plain data write to r15 = jump, all flags preserved
      const newPC = result & PC_MASK;
      this.r[15] = (this.r[15] & ~PC_MASK) | newPC;
      return;
    }
    if (writesReg) this._setReg(target, result, false);
  }

  _singleTransfer(a, ins) {
    const P = (ins >>> 24) & 1, U = (ins >>> 23) & 1, B = (ins >>> 22) & 1;
    const W = (ins >>> 21) & 1, L = (ins >>> 20) & 1;
    const rn = (ins >>> 16) & 0xF, rd = (ins >>> 12) & 0xF;
    const base = this._getReg(rn, a, false);

    let offset;
    if ((ins >>> 25) & 1) { // register offset, through the shifter (imm field = shift)
      const [v] = this._shift(ins & 0x0FFFFFF, a); // I bit cleared: treat as register form
      offset = v;
    } else {
      offset = ins & 0xFFF;
    }
    if (!U) offset = -offset;

    let addr = base;
    if (P) addr = (addr + offset) >>> 0;

    if (L) {
      let v;
      if (B) v = this.rd8(addr);
      else {
        const word = this.rd32(addr & ~3); // aligned read, then rotate
        const r = (addr & 3) * 8;
        v = r ? (((word >>> r) | (word << (32 - r))) >>> 0) : word; // unaligned load rotates so byte@addr is LSB
      }
      this._setReg(rd, v, false);
    } else {
      const v = this._getReg(rd, a, false);
      if (B) this.wr8(addr, v & 0xFF);
      else   this.wr32(addr, v);
    }

    if (!P || W) this._setReg(rn, (base + offset) >>> 0, false); // writeback (post) or P with W
  }

  _blockTransfer(a, ins) {
    const P = (ins >>> 24) & 1, U = (ins >>> 23) & 1, S = (ins >>> 22) & 1;
    const W = (ins >>> 21) & 1, L = (ins >>> 20) & 1;
    const rn = (ins >>> 16) & 0xF;
    let list = ins & 0xFFFF;
    const priv = this.mode !== MODE_USR;
    if (list === 0) list = 0x8000; // empty list codes r15 (rare; treated literally)

    // count set bits
    let count = 0; for (let i = 0; i < 16; i++) if (list & (1 << i)) count++;

    const base = this._getReg(rn, a, false);
    // ARM rule for all forms: lowest-numbered register <-> lowest address.
    let addr = (base + (U ? (P ? 4 : 0) : -(4 * (count - 1 + (P ? 1 : 0))))) >>> 0;
    const finalBase = (base + (U ? count * 4 : -count * 4)) >>> 0;

    const userBank = S && priv; // S without r15 in list = user-bank transfer

    for (let i = 0; i < 16; i++) {
      if ((list & (1 << i)) === 0) continue;
      if (L) {
        const v = this.rd32(addr);
        if (i === 15) {
          if (S && priv) this._writePCRestore(v, 'restore'); // LDM ^ with PC: full CPSR restore
          else { const pcField = v & PC_MASK;
                 this.r[15] = (this.r[15] & ~PC_MASK) | pcField; }
        } else {
          this._setReg(i, v, userBank);
        }
      } else {
        let v;
        if (i === 15) v = (this.r[15] & 0xFC000000) | ((a + 8) & PC_MASK) | (this.r[15] & 3);
        else v = this._getReg(i, a, userBank);
        this.wr32(addr, v >>> 0);
      }
      addr = (addr + 4) >>> 0;
    }
    if (W) this._setReg(rn, finalBase, false);
  }
}

