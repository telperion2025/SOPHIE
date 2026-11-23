# SOPHIE — Reference Manual

Technical reference for the SOPHIE emulator tier: architecture, CPU core
semantics, memory map, device models, bus API, and test traceability.
Design rationale and milestones: [spec.md](spec.md). Build/run instructions:
[user-manual.md](user-manual.md).

## 1. Architecture overview

Three pillars (spec section 7):

1. **Emulation** (this repository, WP1–WP3 done): full-machine emulation of
   Archimedes / Risc PC class machines; the emulator is an ordinary userland
   process on the host.
2. **Compatibility layer** (M7/M8, future): RISC OS 5 binaries call the same
   native module set as native apps — the convergence insight of spec
   section 7.
3. **Native desktop** (`native/`, M6+, not started): RISC OS structure, look
   and feel implemented natively on the BSD-derived core.

Host/emulator split (spec section 1): the host kernel is a clean small
Unix-like system (BSD-derived); drivers, filesystems and the network stack
come from the BSD core; the emulator core is ISA-clean (no host-ISA
assumptions anywhere inside it — the RISC-V/PowerPC host-port rule).

## 2. CPU core (`emu/core/arm26.[ch]`)

ARMv2/ARMv3 26-bit interpreter — the ARM2/ARM250/ARM3/ARM610/710
PC+flags-in-r15 model used by all Archimedes and early Risc PC guests.
C11 port of the verified JS reference (spec Appendix A); behaviour is
bug-compatible with that reference (see §7 known limitations).

### 2.1 Register model

- `r[0..14]` general registers; banked sets: USR r8–r14 (r8–r12 shared with
  SVC/IRQ), SVC r13/r14, IRQ r13/r14, FIQ r8–r14 (`bankUsr`, `bankSvc`,
  `bankIrq`, `bankFiq`).
- `r[15]` = CPSR: bits 31:28 NZCV, bit 27 I, bit 26 F, bits 25:2 PC
  (`PC_MASK = 0x03FFFFFC`), bits 1:0 mode (`MODE_USR/FIQ/IRQ/SVC`).
- When r15 is read as an operand during the instruction at address `a`, it
  reads as `(r15 & 0xFC000000) | ((a+8) & PC_MASK) | (r15 & 3)`
  (`arm26_pc_operand`).
- Reset state: I and F set, SVC mode, PC 0 (`arm26_reset`).

### 2.2 Exceptions

26-bit exception entry (`exception()`): banked `r14_mode := old CPSR`;
`CPSR := (NZCV preserved) | I set | (F set for FIQ) | vector | new mode`.
Vectors: RESET 0x00, UNDEF 0x04, SWI 0x08, PABT 0x0C, DABT 0x10, IRQ 0x18,
FIQ 0x1C. IRQ/FIQ lines (`irqPending`/`fiqPending`) are sampled between
instructions and taken only when the corresponding mask bit is clear.
**Test-exit convention:** `SWI 0` halts the run loop without an exception
(harness use only; ROM code never sees it).

### 2.3 Instruction coverage (v1)

- Data processing: all 16 opcodes with the barrel shifter (LSL/LSR/ASR/ROR,
  immediate and register shifts, RRX), immediate rotate encoding, full
  NZCV semantics; S with rd=15 implements the 26-bit CPSR-write rules:
  logical/TEQP-style ops replace the full CPSR (privileged) via
  `write_pc_logical`, arithmetic ops (SUBS pc, …) take NZCV from the result
  and preserve I/F/mode via `write_pc_restore`.
- MUL/MLA (with S → N,Z), SWP/SWPB.
- LDR/STR word and byte, all P/U/W combinations, immediate offset (group
  010) and register offset (group 011, bit 4 = 0); unaligned word loads
  rotate the aligned word by `(addr&3)*8` (ARM semantics). A load to r15
  (`LDR pc, …`) writes the PC with flags preserved — needed by real ROM
  vector tables.
- ARMv4 halfword/signed transfers (group 000 with bits 7:4 = 1SH1):
  LDRH/STRH (zero-extended halfword), LDRSB, LDRSH, immediate and register
  offset forms, with the same P/U/W writeback rules; the undefined
  combinations (S=0,H=0 load; any S=1 store) raise UNDEF.
- LDM/STM all four addressing modes; lowest register at lowest address;
  STM stores r15 as CPSR (`flags | pc+8 | mode`); LDM with r15 and S in a
  privileged mode restores the full CPSR; S without r15 in the list is a
  user-bank transfer.
- B/BL with 26-bit sign-extended offset.
- Condition codes 0–E; NV never executes.
- Coprocessor instructions and ARMv3 MRS/MSR patterns raise UNDEF
  (exactly how ARMv2a hardware treats them).

### 2.4 API

```c
void     arm26_init(Arm26 *cpu, mem_bus *mem);
void     arm26_reset(Arm26 *cpu);
void     arm26_run(Arm26 *cpu, uint64_t n);          /* up to n instructions */
void     arm26_load_program(Arm26 *cpu, const uint32_t *words, size_t n, uint32_t base);
uint8_t  arm26_rd8(Arm26 *cpu, uint32_t addr);       /* through the bus */
void     arm26_wr8(Arm26 *cpu, uint32_t addr, uint8_t v);
uint32_t arm26_rd32(Arm26 *cpu, uint32_t addr);
void     arm26_wr32(Arm26 *cpu, uint32_t addr, uint32_t v);
uint32_t arm26_mode(const Arm26 *cpu);
uint32_t arm26_pc_operand(const Arm26 *cpu, uint32_t a);
```

## 3. Memory bus and device registry (`emu/mach/mem.[ch]`)

All guest memory access is mediated by `mem_bus`; the CPU core never touches
raw storage. Regions are `mem_region` structs matched in registration order
(first match wins); a region flagged `is_ram` is a flat byte array used as a
fast path. Word access is composed byte-by-byte, so there are **no
endianness or alignment assumptions** (PowerPC host-port rule, spec §3).

```c
void     mem_bus_init(mem_bus *bus, uint8_t *ram, uint32_t ram_size); /* power-of-two size; NULL ram = empty bus */
void     mem_region_add(mem_bus *bus, mem_region *r);                /* append (lowest priority) */
uint8_t  mem_rd8(mem_bus *bus, uint32_t addr);
void     mem_wr8(mem_bus *bus, uint32_t addr, uint8_t v);
uint32_t mem_rd32(mem_bus *bus, uint32_t addr);
void     mem_wr32(mem_bus *bus, uint32_t addr, uint32_t v);
```

A region provides `rd8/wr8` (mandatory for device regions) and optionally
`rd32/wr32` (if NULL, the bus composes/splits word access from/to bytes).
Unmapped reads return 0xFF per byte (open bus) and unmapped writes are
ignored; both are counted in `bus.unmapped_rd`/`unmapped_wr`.

## 4. Archimedes base machine (`emu/mach/machine.[ch]`)

### 4.1 Memory map

| Address range | Region |
| --- | --- |
| 0x00000000–0x01FFFFFF | logical window via MEMC — **ROM at reset**, identity-mapped DRAM after the control register is written |
| 0x02000000–0x02FFFFFF | physical DRAM window (direct alias, `is_ram`) |
| 0x03000000–0x031FFFFF | podule/expansion space (stub, reads 0xFF) |
| 0x03200000–0x033FFFFF | IOC (registers at low 0x80, mirrored; 1 MHz peripheral bus at +0x10000, stub) |
| 0x03400000–0x035FFFFF | VIDC (write-only registers) |
| 0x03600000–0x037FFFFF | MEMC registers |
| 0x03800000–0x03BFFFFF | ROM (read-only) |

```c
void machine_init(Machine *m, uint8_t *ram, uint32_t ram_size, uint8_t *rom, uint32_t rom_size);
void machine_run(Machine *m, uint64_t n);  /* executes and ticks the IOC timers */
```

`machine_run` runs the CPU in small batches and advances the IOC timers by
the elapsed cycles, so IRQ-driven guest loops work.

### 4.2 MEMC (`memc.[ch]`)

Model v1: control register at 0x03600000+0x00 (any write leaves the ROM reset
map — the logical window becomes identity-mapped DRAM); video DMA start
(logical address) at +0x04. The CAM (logical→physical page map) is not
walked yet; identity mapping covers the first `ram_size` bytes. Refined at
WP4/WP5 against the MEMC1A datasheet.

### 4.3 VIDC (`vidc.[ch]`)

Write-only register region at 0x03400000: a word store delivers
`reg = v>>24, val = v & 0xFFFFFF` (byte stores replicate the byte on all
lanes: `reg = byte, val = 0xFFFFFF`); reads return open bus. Register file
(reg index = byte offset >> 2):

| Regs | Meaning |
| --- | --- |
| 0x00–0x0F | display palette, 12-bit BGR (b=11:8, g=7:4, r=3:0); expanded to 256 8bpp entries |
| 0x10 | border colour |
| 0x11–0x13 | cursor palette |
| 0x18–0x1F | stereo image |
| 0x20–0x2F | CRTC: HCR HSWR HBSR HDSR HDER HBER HCSR HIR VCR VSWR VBSR VDSR VDER VBER VCSR VCER |
| 0x30 | sound frequency latch (register capture only; sound is M4) |
| 0x38 | control: bits 1:0 pixel clock, bits 3:2 bpp mode (0=1,1=2,2=4,3=8 bpp), bit 6 interlace |

`vidc_render(Vidc*, mem_bus*, uint32_t *fb, int w, int h)` decodes the CRTC
registers with the VIDC1 arithmetic (`HCR=((v>>14)<<1)+2`, `HBSR/HBER =
((v>>14)<<1)+1`, `HDSR/HDER = (v>>14)*2 + xstep[bpp]` with
`xstep={19,11,7,5}`, vertical regs `(v>>14)+1`, cursor regs masked to 10
bits +1) and rasterises from the MEMC video-start address into a 32bpp
0xRRGGBB framebuffer.

### 4.4 IOC (`ioc.[ch]`)

Byte-wide register file at 0x03200000, mirrored through the region
(`reg = (addr-0x03200000) & 0x7F`); offsets ≥ 0x10000 are the 1 MHz bus
(stub). IOC registers sit on bus lanes D16–D23: word stores latch
`(v>>16)&0xFF`, byte stores latch the byte; word reads are composed
little-endian from byte reads (documented simplification).

| Byte offset | Register |
| --- | --- |
| 0x00 | CONTROL (GPIO C0–C5, IR/IF lines) |
| 0x04 | KART keyboard data (read clears Rx-full, pops the key queue) |
| 0x10 / 0x14 / 0x18 | IRQ_STATUS_A / IRQ_REQUEST_A (write-1-clear, mask 0x7C) / IRQ_MASK_A |
| 0x20 / 0x24 / 0x28 | IRQ_STATUS_B / IRQ_REQUEST_B / IRQ_MASK_B |
| 0x30 / 0x34 / 0x38 | FIQ_STATUS / FIQ_REQUEST / FIQ_MASK |
| 0x40–0x4C | T0 latch lo/hi, GO, LATCH (likewise T1 0x50, T2 0x60, T3 0x70) |

IRQ A bits: 7 force (POR), 6 timer1, 5 timer0, 4 POR, 2 IR, 1 IF, 0 IL7.
IRQ B bits: 7 KART Rx full, 6 KART Tx empty, 0–5 IL0–IL5. Reset status:
IRA = 0x90, IRB = 0x40, FIQ = 0x80. Timers 0/1 run at 2 MHz (4 CPU cycles
per tick at the 8 MHz CPU clock), raise IRQ A bits 5/6 on underflow and
reload continuously from their latches; T2/T3 are baud/KART clocks (no
IRQ). The IRQ/FIQ lines to the CPU are `(status & mask) != 0`.

```c
void ioc_tick(Ioc *ioc, Arm26 *cpu, uint32_t cycles_delta);
void ioc_key_press(Ioc *ioc, uint8_t byte);   /* KART queue */
```

## 5. Tests (`emu/tests/`)

- `arm26_test.c` — the 42-assertion suite transplanted verbatim from spec
  Appendix B (test names match one-to-one).
- `arm26_ext_test.c` — WP6 extension tests (15 checks): group-011
  register-offset LDR/STR incl. negative offset + writeback, LDRH/STRH
  immediate and register forms with writeback, LDRSB/LDRSH sign extension,
  undefined halfword-space combinations, `LDR pc`.
- `mmio_test.c` — WP2 dispatch tests (dummy MMIO device, CPU STR/LDR to the
  device, open-bus unmapped access, region priority, RAM regression,
  top-word byte masking).
- `machine_test.c` — WP3 dummy-ROM acceptance (see §8).
- `bench.c` — throughput benchmark, mirrored by `emu/ref/bench.mjs`.
- `test.h` — shared `check()` harness and the `Rig` (CPU + flat-RAM bus).
- `../tools/romboot.c` — WP4 smoke tool (not in `make test`): loads a ROM
  image from `roms/` into the Archimedes machine and runs N instructions,
  printing final PC/mode/MEMC/bus stats.

Run everything with `make test`; benchmark with `make bench`.

## 6. Performance

Measured on an x86-64 host (`make bench`, 1,000,000-instruction loop):
C core ≈ 290 MIPS, JS reference ≈ 23 MIPS — the C port is ~12x faster than
the JS reference (WP1 acceptance: within 3x) and ~20–35x faster than the
~8–14 MIPS of real Archimedes-era hardware (spec §2, §9).

## 7. Known limitations and deliberate deviations from the JS reference

The WP6 ARMv4-extension pass implemented group 011 (register-offset
LDR/STR), the halfword/signed transfer space, and `LDR pc` — all deliberate
deviations from the JS reference *toward* the ARM ARM (the reference shadows
011 with UNDEF and no-ops LDR pc), pinned by `arm26_ext_test` (15 checks).
Remaining known limitations:

- StrongARM-specific writeback/timing nuances are not modeled (an
  interpreter has no cycle timing to diverge in; LDM/STM writeback follows
  the ARM ARM).
- MEMC CAM is not walked (identity map); VIDC sound is register capture
  only; the IOC word-read lane simplification is documented in §4.4.
- `SWI 0` halt convention (test harness only).
- Deliberate deviation from the JS reference at the RAM top boundary: the
  reference's `rd32/wr32` mask only byte `a`, leaving bytes `a+1..a+3`
  unmasked, so the final word of RAM (addr = ramSize−4) reads back with
  zeroed high bytes there (out-of-bounds `Uint8Array` access). The C bus
  masks every byte (`mem.c`), matching real hardware; pinned by the
  `ram-top-word-masked` check in `mmio_test`.

## 8. Traceability matrix (test → spec)

| Suite / test group | Verifies (spec reference) |
| --- | --- |
| `arm26_ext_test` (15 checks) | WP6: group-011 register-offset transfers, ARMv4 halfword/signed transfers, LDR pc, undefined-space traps (§10 WP6; ARM ARM) |
| `arm26_test`: mov-imm … bic-val | data processing, shifter, flags (§2, §9; ARM ARM) |
| `arm26_test`: str-word … strb | single transfers incl. unaligned rotation (§2) |
| `arm26_test`: stmfd-*, ldmfd-*, stm-r15-cpsr | block transfers, stack orders, r15-as-CPSR (§2, §9) |
| `arm26_test`: swp-*, mul-42 | SWP, MUL (§2) |
| `arm26_test`: swi-*, rti-*, undef-*, irq-*, teqp-usr | exception entry, banking, CPSR restore, mode switch (§2, §9) |
| `arm26_test`: branch-*, sum-*, halt, lsl-*, adc-*, tst-* | branches, loops, carry chains, test ops, SWI-0 halt (§9) |
| `mmio_test` (8 checks) | WP2 acceptance: registry dispatch, CPU-to-device, open bus, priority, RAM regression, top-word byte masking (§10 WP2) |
| `machine_test` (13 checks) | WP3 acceptance: dummy-ROM boot, memory R/W, timer IRQs, 1M-cycle no-lockup loop, MEMC/VIDC/IOC models (§10 WP3) |
| `bench` vs `emu/ref/bench.mjs` | WP1 benchmark criterion (within 3x of JS) (§10 WP1) |

The whole suite is the acceptance criterion for the future RISC-V and
PowerPC host ports: it must pass identically on every host ISA (§3).

## 9. Portability rules

- No host-ISA assumptions inside the emulator core; host optimisations stay
  pluggable beside the portable core (§2).
- All guest memory access is mediated byte-wise through `mem_bus`; guest
  little-endian semantics hold on big-endian hosts, and unaligned guest
  accesses never trap on hosts that permit them (§3, PowerPC plan).
- C11, no C++, plain make; tests are hand-encoded instruction words (§10).

## 10. Licensing / derivation boundary

Original code: Apache-2.0 (`LICENSE`, `NOTICE`). The emulator tier is
specification-driven only. RPCEmu / RPCEmu Extended (GPL v2) and Arculator
(closed) are behaviour reference and credit only — no code or pseudocode
derivation (pseudocode does not launder GPL). MAME's Acorn drivers
contributed register-layout facts only. RISC OS ROM images and Wi-Fi
firmware blobs are runtime data acquired separately under their own terms
(§5, README).
