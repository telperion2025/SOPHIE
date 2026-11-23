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
- r15 as an operand during the instruction at address `a` (Acorn 26-bit
  rule): as **Rm** it reads as `(r15 & 0xFC000000) | ((a+8) & PC_MASK) |
  (r15 & 3)` (`arm26_pc_operand`) — flags and mode included; as the **first
  operand Rn** of a data-processing instruction, or as the base register of
  LDR/STR/LDM/STM/SWP, only the 24 PC bits are presented and all PSR bits
  (flags 31:26 and mode 1:0) are supplied as zero.
- Reset state: I and F set, SVC mode, PC 0 (`arm26_reset`).

### 2.2 Exceptions

26-bit exception entry (`exception()`): banked `r14_mode := old CPSR` with
the PC field offset (`+0` for SWI/UNDEF = faulting+4, `+4` for IRQ/FIQ/DABT
= next/faulting+8, so `SUBS PC,lr,#4` / `#8` return correctly);
`CPSR := (NZCV preserved) | I set | (F set for FIQ) | vector | new mode`.
Vectors: RESET 0x00, UNDEF 0x04, SWI 0x08, PABT 0x0C, DABT 0x10, IRQ 0x18,
FIQ 0x1C. DABT is raised by the memory bus (§3) when a USR-mode data access
lands in the abort window; `lr_svc` holds the faulting pc+8 (return with
`SUBS PC,lr,#8` retries the faulting instruction). IRQ/FIQ lines
(`irqPending`/`fiqPending`) are sampled between instructions and taken only
when the corresponding mask bit is clear.
**Test-exit convention:** while `swi0_halt` is set (the default after
`arm26_reset`; used by the test harnesses), `SWI 0` halts the run loop
without an exception. The machine clears `swi0_halt` in `machine_init`, so
a booting ROM gets a real SWI exception — RISC OS defines SWI 0 as
OS_WriteC, which the ROMs call while printing boot messages.

### 2.3 Instruction coverage (v1)

- Data processing: all 16 opcodes with the barrel shifter (LSL/LSR/ASR/ROR,
  immediate and register shifts, RRX), immediate rotate encoding, full
  NZCV semantics. Shifter carry-out feeds C only for **logical** ops with
  S set; arithmetic ops (ADD/SUB/ADC/SBC/CMP/CMN/RSB/RSC) take C/V from
  the operation itself. S with rd=15 implements the 26-bit CPSR-write
  rules: logical ops (`MOVS pc, ..`) replace the full CPSR (privileged) via
  `write_pc_logical`; arithmetic ops (`ADDS/SUBS pc, …`, e.g. the ROM's
  `adds pc, r0, pc` veneers) write PC from the result and take NZCV from
  it, preserving I/F/mode. The compare "P" forms (`TSTP/TEQP/CMPP/CMNP` -
  TST/TEQ/CMP/CMN with rd=15 and S=1) instead load the CPSR from the ALU
  result and never write the PC (the ROM relies on this: `teq lr,#0`
  with a CPSR image in lr, e.g. the I2C bitbang at ROM300 0x03802a50).
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

The bus also models the MEMC data-abort line: a **data** access to the
supervisor-only window `[bus.io_base, bus.io_base+bus.io_size)` while
`bus.user_mode` is set aborts instead of reaching the device (read returns
open bus, write is dropped) and latches `bus.dabt`/`dabt_addr`/`dabt_count`.
Instruction fetches (`bus.fetch` set by the core around the opcode read) are
exempt, and the core drives `user_mode` only around its own accesses, so
host/device bus users never abort. After the faulting instruction the core
takes a DABT exception (vector 0x10, SVC mode, `lr_svc` = faulting pc+8).
The base machine enables the window for the unclaimed podule/expansion space
0x03000000-0x031FFFFF only: POST's own mode test performs USR-mode access to
claimed devices (VIDC/ROM window), so a blanket supervisor-only I/O abort is
not A310/MEMC1a behaviour (see §4.1).

## 4. Archimedes base machine (`emu/mach/machine.[ch]`)

### 4.1 Memory map

| Address range | Region |
| --- | --- |
| 0x00000000–0x01FFFFFF | logical window via MEMC — **boot ROM latched at reset**, then CAM-walked to physical DRAM |
| 0x02000000–(0x02000000+ram_size) | physical DRAM window (`is_ram`, direct alias); beyond installed RAM up to 0x02FFFFFF reads as open bus (0xFF), writes are dropped |
| 0x03000000–0x031FFFFF | podule/expansion space (stub, reads 0xFF); unclaimed: USR-mode data accesses data-abort here (§3), instruction fetches exempt |
| 0x03200000–0x033FFFFF | IOC (registers at low 0x80, mirrored; 1 MHz peripheral bus at +0x10000: peripheral 5 at 0x03250000 holds the floppy latches, the WD1772 block sits at 0x03310000; other blocks stub) |
| 0x03250018 | floppy latch B (bit 1 DDEN, bit 3 master reset, active low) |
| 0x03250040 | floppy latch A (bits 0-3 drive select active-low, bit 4 side, bit 5 motor active-low) |
| 0x03310000–0x0331FFFF | WD1772 floppy controller (registers at +0x00/+0x04/+0x08/+0x0C, see §4.6) |
| 0x03400000–0x035FFFFF | VIDC (write-only registers) |
| 0x03600000–0x037FFFFF | MEMC registers |
| 0x03800000–0x03FFFFFF | ROM window, 8MB: image mirrors across it (reads return ROM; **writes program the MEMC CAM**) |

```c
void machine_init(Machine *m, uint8_t *ram, uint32_t ram_size, uint8_t *rom, uint32_t rom_size);
int  machine_disc_attach(Machine *m, int drive, const char *path, int wprot); /* WP7 */
void machine_disc_detach(Machine *m, int drive);
void machine_run(Machine *m, uint64_t n);  /* executes and ticks IOC/VIDC/FDC */
```

`machine_run` runs the CPU in small batches and advances the IOC timers, the
VIDC frame timer and the FDC command engine by the elapsed cycles, so
IRQ/FIQ-driven guest loops work.

### 4.2 MEMC (`memc.[ch]`)

MEMC1A model with a real CAM walk (WP4), per the MEMC1A register/encoding
layout (see §10 for the facts-only boundary):

- **Reset latch**: `latchrom=1` — logical reads mirror the ROM window
  (`rom[addr & 0x1FFFFF]`), writes are ignored. Only a **write** to the
  ROM window (a CAM page write) drops the latch; ROM-window reads and
  IOC/VIDC/MEMC-register accesses leave it set, as on real hardware.
- **CAM**: 0x2000 entries, one per logical page. Page size 4K/8K/16K/32K is
  selected by control-register bits 3:2. Page entries are written by storing
  a data-encoded word anywhere in the ROM window 0x03800000–0x03FFFFFF:
  logical page, physical page, MEMC id and 2-bit protection level are packed
  per page size (e.g. 4K: `11 1LLL LLLL LLLL LLAA MPPP PPPP`). Writing a page
  first unmaps any other logical page resolving to the same physical page.
  Protection levels are stored in `ppl[]` but **not enforced**.
- **Logical window**: each access walks the CAM:
  `phys = 0x02000000 | pages[addr/pagesize]*pagesize + addr%pagesize`.
  Unmapped pages read open bus (0xFF) and ignore writes.
- **Registers** at 0x03600000–0x037FFFFF, selected by the data tag
  `(v & 0x03E00000) == 0x03600000`, register index in bits 19:17, value
  `((v>>2) & 0x7FFF) * 16`: 0 video init, 1 video start, 2 video end,
  3 cursor init, 4 sound start, 5 sound end, 6 sound pointer reset,
  7 control (page size bits 3:2, video DMA bit 10, sound DMA bit 11,
  OS mode bit 12).

### 4.3 VIDC (`vidc.[ch]`)

Write-only register region at 0x03400000: a word store delivers
`reg = v>>24, val = v & 0xFFFFFF` (byte stores replicate the byte on all
lanes: `reg = byte, val = 0xVVVVVV`); reads return open bus. Register file
per the VIDC1 datasheet (reg byte = `v>>24`; within each block the low two
bits of the reg byte are ignored, index = `(reg & 0xFC) >> 2`):

| Reg bytes | Meaning |
| --- | --- |
| 0x00–0x3F | display palette, 16 entries at reg `4*i`, 12-bit BGR (b=11:8, g=7:4, r=3:0); entry i also feeds 8bpp pen group `i*0x10..i*0x10+0xF0` (pixel high nibble = gun high nibble, register = low nibble) |
| 0x40–0x4F | cursor palette, 4 entries at reg `0x40+4*i`; entry 0 (reg 0x40) doubles as the **border colour** |
| 0x60–0x7F | stereo image, 8 channels at reg `0x60+4*c` (capture only) |
| 0x80–0xBF | CRTC, 16 registers at reg `0x80+4*i`: HCR HSWR HBSR HDSR HDER HBER HCSR HIR VCR VSWR VBSR VDSR VDER VBER VCSR VCER |
| 0xC0–0xC3 | sound frequency latch (register capture only; sound is M4) |
| 0xE0–0xE3 | control: bits 1:0 pixel clock, bits 3:2 bpp mode (0=1,1=2,2=4,3=8 bpp), bit 6 interlace |

`vidc_render(Vidc*, mem_bus*, uint32_t *fb, int w, int h)` decodes the CRTC
registers with the VIDC1 arithmetic (`HCR=((v>>14)<<1)+2`, `HBSR/HBER =
((v>>14)<<1)+1`, `HDSR/HDER = (v>>14)*2 + xstep[bpp]` with
`xstep={19,11,7,5}` (re-decoded when the control register changes),
`HCSR=((v>>13)&0x7FF)+6`, vertical regs `(v>>14)+1`, `VCSR/VCER =
((v>>14)&0x3FF)+1`) and rasterises from the MEMC video-start address into a
32bpp 0xRRGGBB framebuffer (pen table: 8bpp pens 0x00–0xFF, display
4bpp pens 0x100–0x10F, cursor/border pen 0x110; interlace plots each
source row on two consecutive framebuffer rows).

**Flyback (vsync) interrupt (WP4)**: `vidc_tick(vidc, cycles)` (called from
`machine_run` alongside `ioc_tick`) counts CPU cycles per frame as
`HCR * VCR * CPU_HZ / pixel_clock` (pixel clock `{8,12,16,24}` MHz from
control bits 1:0, CPU at 8 MHz; e.g. HCR=1024, VCR=312 at 16 MHz is 159744
cycles = 50 Hz). Each frame end raises
the VIDC flyback line, which the IOC latches as **IRQ status A bit 3 (0x08,
IR line)** — the Archimedes vsync interrupt that RISC OS's screen driver
hooks. With no valid CRTC mode programmed (HCR/VCR/HBER/VBER < 2, or
totals < border ends) no vsync fires.

### 4.4 IOC (`ioc.[ch]`)

Byte-wide register file at 0x03200000, mirrored through the region
(`reg = (addr-0x03200000) & 0x7F`); offsets ≥ 0x10000 are the 1 MHz bus
(stub). IOC registers sit on bus lanes D16–D23: word stores latch
`(v>>16)&0xFF`, byte stores latch the byte; word reads are composed
little-endian from byte reads (documented simplification).

| Byte offset | Register |
| --- | --- |
| 0x00 | CONTROL (GPIO C0–C5, IR/IF lines; C0=SDA, C1=SCL to the PCF8583 — §4.4a) |
| 0x04 | KART (read clears Rx-full and pops the queue; write starts Tx and clears Tx-empty until the shift register drains) |
| 0x10 / 0x14 / 0x18 | IRQ_STATUS_A / IRQ_REQUEST_A (write-1-clear, mask 0x7C) / IRQ_MASK_A |
| 0x20 / 0x24 / 0x28 | IRQ_STATUS_B / IRQ_REQUEST_B / IRQ_MASK_B |
| 0x30 / 0x34 / 0x38 | FIQ_STATUS / FIQ_REQUEST / FIQ_MASK |
| 0x40–0x4C | T0 latch lo/hi, GO, LATCH (likewise T1 0x50, T2 0x60, T3 0x70) |

IRQ A bits: 7 force, 6 timer1, 5 timer0, 4 POR, **3 IR (VIDC flyback/vsync)**,
2 IF, 1 IL7, 0 IL6. IRQ B bits: 7 KART Rx full, 6 KART Tx empty, 0–5 IL0–IL5.
Request registers (0x14/0x24/0x34) read as `status & mask`; writing them
clears the corresponding status bits (write-1-clear; mask 0x7C on A).
Reset status: IRA = 0x90, IRB = 0x40, FIQ = 0x80. Timers 0/1 run at 2 MHz
(4 CPU cycles per tick at the 8 MHz CPU clock), raise IRQ A bits 5/6 on
underflow and reload continuously from their latches; T2/T3 are baud/KART
clocks (no IRQ). Writing a timer LATCH register (base+0x0C) captures the
current counter into the lo/hi readback bytes (base+0x00/0x04), matching
the timed-measurement idiom RISC OS POST uses. The IRQ/FIQ lines to the CPU
are `(status & mask) != 0`.

```c
void ioc_tick(Ioc *ioc, Arm26 *cpu, uint32_t cycles_delta);
void ioc_flyback(Ioc *ioc);                    /* VIDC vsync -> IRQA bit 3 */
void ioc_key_press(Ioc *ioc, uint8_t byte);    /* KART make code */
void ioc_key_release(Ioc *ioc, uint8_t byte);  /* KART break code (byte|0x80) */
```

The KART path is a 16-byte Rx queue: a byte lands in the KART register with
IRQ B bit 7 (Rx full) set; reading KART clears Rx-full and pops the next
queued byte (keyboard make/break protocol: press = keycode, release =
keycode|0x80). Queuing a byte updates the CPU IRQ line immediately. A KART
*write* starts the transmit shift: IRQ B bit 6 (Tx empty) clears and
re-asserts after the modelled shift-out delay (64 x 2 MHz ticks), which is
what RISC OS POST polls (ROM300 0x03811084).

#### 4.4a CMOS / RTC (PCF8583) on IOC CONTROL

CONTROL bits C0/C1 are the I2C SDA/SCL lines to a PCF8583 clock/calendar
with 240 bytes of CMOS RAM (`mach/cmos.[ch]`, slave address 0xA0). Writes
to CONTROL forward bit 0 to `cmos_sda_w` and bit 1 to `cmos_scl_w`; reads
return bit 0 as the open-drain SDA line (host output ANDed with the device
output `cmos_sda_r`) and bits 1-7 as the written output latch. The device
implements the datasheet I2C state machine (START/STOP on SDA while SCL
high, address byte, register byte, data bytes with ACK on the 9th clock,
read mode shifts out `data[reg++]` MSB first, mask flag in control bit 3
for the date registers). The 256-byte file holds the clock registers at
0x00-0x0F (control bit 7 = stop) and CMOS RAM at 0x10-0xFF, initialised
with the standard *Configure defaults (0x30=0x20 Autoboot, 0x36=0x01,
0x45=0x17, 0x50=0x90, 0xb4=0x24, 0xc7=0x01 one floppy, 0xd0=0x08). RISC OS
3.00 POST bitbangs this bus (ROM300 0x38029ac) to read the configuration;
with no device modelled the reads fail and POST sets its "CMOS unreadable"
flag and takes a different (long-test) path, so the CMOS model is required
for real-ROM boot to proceed past POST's configuration stage.

### 4.5 Host window layer (`host/`, WP5)

Backend interface (`host/host.h`): `host_open/host_present/host_poll/
host_should_quit/host_close` present a 32bpp 0xRRGGBB framebuffer and deliver
key events; `host_pump_input()` injects them into the machine's KART as
RISC OS low-level key numbers (make = key, break = key|0x80) via
`ioc_key_press`/`ioc_key_release`. `host_key_to_riscos()` maps host key
codes (ASCII + `HOST_KEY_*` specials) to key numbers following the A3000
keyboard matrix layout (key = (column << 4) | row).

Two backends: `host_headless.c` (default; PPM snapshots via
`host_headless_set_ppm`, scripted input via `host_headless_push_key` — used
by CI and tests) and `host_x11.c` (real X11 window, libX11 via dlopen so the
default build has no X11 dependency; build with `make host-x11`, used when
`DISPLAY` is set). `tools/sophie.c` is the frontend: boot a ROM, render each
VIDC frame (1024x768, 160000 cycles/frame at 8 MHz/50 Hz), present, pump
input; `--disc N=path.adf` attaches a disc image to floppy drive N (0-3),
`--ro` makes it write-protected.

### 4.6 WD1772 floppy controller + disc images (`fdc.[ch]`, `disc.[ch]`, WP7)

The WD1772 model covers the register block at 0x03310000–0x0331FFFF
(register selected by address bits 3:2: +0x00 status/command, +0x04 track,
+0x08 sector, +0x0C data; like the IOC the chip latches D16–D23 on word
stores). Status bits: BUSY 0x01, DRQ 0x02, TR00 0x04, CRC 0x08, RNF 0x10,
spin-up 0x20, WP 0x40, motor-off 0x80. INTRQ drives IOC FIQ status bit 0x02
(FH1), DRQ drives FIQ bit 0x01 (FH0); reading status clears INTRQ, writing a
command clears INTRQ, reading/writing data clears DRQ. Commands are modelled
at sector granularity on an absolute 8 MHz cycle timeline (spin-up ~15 ms,
~3 ms/step, ~10 ms to a sector's first byte, ~6 µs/byte): type I
(restore/seek/step in/out with the U track-update flag), read sector (single
and multiple), write sector (single and multiple), read address (6 ID bytes),
read track, and force interrupt (0xD0, aborts and raises INTRQ if bit 3 set).
Drive select/side/motor come from latch A (0x03250040), density (DDEN) and
master reset from latch B (0x03250018). Write track (0xF0, used by formatters)
streams and discards the data — formatting a data-only image is not
supported (see §7).

`disc.c` backs up to 4 drives with raw sector images ("data only" .adf/.adl/
.img files); geometry is inferred from the file size (ADFS S/M/L, D/E, F, G
and DOS 360K/400K/720K). Linear layout: `offset = ((track*sides + side)*spt
+ (sector-base))*sectsize` with sides interleaved within a track; sector
numbers are 1-based for 512-byte sectors and 0-based otherwise, matching the
studied emulators. `tools/mkadfs.c` writes a blank, formatted ADFS L (640K,
80x2x16x256, old map) image: free-space map in sectors 0-1 (255-start
downward carry-fold checksums), empty "Hugo" root directory "$" in sectors
2-6.

## 5. Tests (`emu/tests/`)

- `arm26_test.c` — the 45-assertion suite transplanted verbatim from spec
  Appendix B (test names match one-to-one), plus 5 added checks pinning the
  26-bit r15 operand rules (Rn zeroes the PSR, Rm keeps it), LDR Rd==Rn
  load-wins writeback ordering, and SWI 0 vectoring when `swi0_halt` is
  cleared (50 checks total).
- `edge_test.c` — wide edge-case battery from the review pass (28 checks):
  MULS preserving V, Rs=15 PSR-zero register shifts, register-shift edge
  amounts (LSL 0 / ROR 32 / LSR#0 / RRX), STM base-in-list position rule,
  LDRH Rd==Rn load-wins, TST-with-S=0 and coprocessor UNDEF traps, NV
  condition, BL lr, STR pc CPSR store, LDM pc flag/CPSR behaviour, IRQ
  lr=pc+8, plus machine edges: unmapped-logical open bus, CAM alias
  unmapping, ROM window mirror + latch drop, open bus beyond installed
  DRAM, USR fetch exemption vs SVC podule reads, IOC timer mask/line,
  VIDC flyback only with a valid CRTC mode.
- `arm26_ext_test.c` — WP6 extension tests (15 checks): group-011
  register-offset LDR/STR incl. negative offset + writeback, LDRH/STRH
  immediate and register forms with writeback, LDRSB/LDRSH sign extension,
  undefined halfword-space combinations, `LDR pc`.
- `mmio_test.c` — WP2 dispatch tests (dummy MMIO device, CPU STR/LDR to the
  device, the 26-bit CPU address-bus mask, open-bus unmapped access, region
  priority, RAM regression, top-word byte masking).
- `machine_test.c` — WP3/WP4 dummy-ROM acceptance: the ROM programs the MEMC
  control register (32K pages) and the CAM from the ROM window, unlatching
  the boot ROM, then boots from CAM-mapped DRAM; the IRQ handler tallies
  timer, VIDC-flyback and KART-Rx interrupts so vsync/keyboard IRQ delivery
  is checked end to end (see §8).
- `bench.c` — throughput benchmark, mirrored by `emu/ref/bench.mjs`.
- `test.h` — shared `check()` harness and the `Rig` (CPU + flat-RAM bus).
- `host_test.c` — WP5 host layer tests (headless backend cycle, PPM pixels,
  keymap, KART injection via `host_pump_input`).
- `fdc_test.c` — WP7 acceptance (33 checks): WD1772 register map, D16-D23
  word-store latching, type I commands, read/write sector against a real
  image (DRQ→FIQ FH0 per byte, INTRQ→FH1 on completion), RNF, write protect,
  read address, force interrupt, master reset, drive latches, disc geometry
  table — plus an end-to-end guest ARM program that boots from the ROM window
  and reads a 1024-byte sector through the bus.
- `../tools/romboot.c` — WP4/WP7 smoke tool (not in `make test`): loads a ROM
  image from `roms/` into the Archimedes machine, optionally with
  `--disc N=path`, and runs N cycles, printing final PC/MEMC/bus/FDC stats.
- `../tools/sophie.c` — WP5/WP7 frontend (not in `make test`): boots a ROM and
  presents VIDC frames through the host window layer with keyboard input;
  `--disc N=path.adf [--ro]` attaches disc images.
- `../tools/mkadfs.c` — WP7 tool (not in `make test`): writes a blank,
  formatted ADFS L disc image.

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
- VIDC sound is register capture only; the IOC word-read lane simplification
  is documented in §4.4; MEMC protection levels are stored but not enforced
  (§4.2). VIDC flyback (vsync) and KART keyboard interrupts are delivered
  (§4.3/§4.4), and the WD1772 floppy controller with disc images is modelled
  (§4.6, WP7) — but the full podule/1MHz bus, CMOS/RTC and hard-disc
  (HD63463/IDE) are still stubs, and the WD1772 write-track (format) command
  discards data, so real-ROM boot still stops short of the desktop (M3
  continues). **M2 is reached**: with the 26-bit r15-as-Rn PSR-zero rule and
  the LDR Rd==Rn load-wins writeback order in the core, plus `swi0_halt`
  cleared for the machine (SWI 0 = OS_WriteC), the ROM300 POST completes,
  the kernel installs its SWI/IRQ vectors in RAM and dispatches SWIs from
  kernel code in DRAM, handles VIDC vsync IRQs (vector 0x18, `lr_irq`=
  next+4), and reaches the kernel idle polling loop (SVC 0x0381d1xx region,
  then the USR-mode CLI loop at 0x01f033xx) at ~66M cycles with zero data
  aborts and zero reboots (`tools/romboot roms/ROM300 70000000`);
  ROM200 likewise runs stably in USR mode. Past ~74M cycles a further M3
  issue appears: the USR-mode loop eventually marches through unmapped
  logical pages (open-bus NV nops) instead of idling - tracked under M3. CMOS/RTC
  (PCF8583) is modelled (§4.4a) and RISC OS 3.00 POST passes its
  configuration stage. The physical DRAM window decodes only the installed
  RAM (open bus beyond), which removed the POST reboot crash; the MEMC
  data-abort line is modelled (§3) and enabled for the unclaimed
  podule/expansion window: POST's memory-test stub is cut off at 0x03000000
  instead of wrapping the 26-bit space over itself (a blanket
  supervisor-only I/O abort is *not* A310 behaviour: POST's own mode test
  does USR-mode VIDC/ROM access, and ARM2 has no mode output pin, so claimed
  devices stay USR-accessible).
- `SWI 0` halt convention, gated by `swi0_halt`: set by `arm26_reset` so
  the test harnesses can exit, cleared by `machine_init` so a booting ROM
  gets a real SWI exception (RISC OS SWI 0 = OS_WriteC).
- Further ARM2 corner rules pinned by `edge_test`: MULS/MLAS update N and Z
  only (V preserved, C meaningless); Rs=r15 in a register shift reads PC+8
  with PSR zeroed (same Rn/Rs rule as data processing); STM with writeback
  stores the base unchanged when it is first in the list but the modified
  (final) value when stored second or later (Acorn PRM App. B); TST/TEQ/
  CMP/CMN with S=0 and all coprocessor-space encodings trap as UNDEF on
  ARMv2; exception `lr` offsets: SWI/UNDEF = faulting+4, IRQ/FIQ/DABT =
  next/faulting+8.
- Deliberate deviation from the JS reference at the RAM top boundary: the
  reference's `rd32/wr32` mask only byte `a`, leaving bytes `a+1..a+3`
  unmasked, so the final word of RAM (addr = ramSize−4) reads back with
  zeroed high bytes there (out-of-bounds `Uint8Array` access). The C bus
  masks every byte (`mem.c`), matching real hardware; pinned by the
  `ram-top-word-masked` check in `mmio_test`.
- Deliberate deviation toward the ARM ARM (WP4): the C core masks every CPU
  memory address to the 26-bit address bus (`ADDR_MASK = 0x03FFFFFF`,
  `arm26.[ch]`), so flag bits read from r15 never reach device decoding;
  the JS reference instead wraps addresses mod `ramSize` in `rd8/wr8/rd32`
  (WP2). Pinned by the `cpu-address-bus-26bit` check in `mmio_test`.
- Deliberate deviation toward the ARM ARM (WP4): when r15 is the **base
  register** of LDR/STR/LDM/STM/SWP, the C core uses PC+8 with the PSR bits
  supplied as zero (the 26-bit ARM first-operand rule), so `LDR [pc,#imm]`
  is word-aligned in any mode; the JS reference supplies the full r15
  (flags|mode included) and therefore rotates such loads by the mode bits.
  Data-processing reads of r15 as **Rm** keep the reference behaviour (full
  flags|PC|mode), but as **Rn** the C core now supplies PSR bits as zero
  (PC+8 only), matching the Acorn PRM Appendix B rule ("when R15 is used in
  the Rn or Rs positions, it gives the PC without the PSR flags") — this is
  what keeps the ROM300 POST mode-test chain in SVC/FIQ instead of derailing
  into USR mode. Similarly, on a load with writeback where Rd==Rn (e.g. the
  ROM's linked-list walk `ldr fp,[fp],#0`), the C core lets the loaded value
  win over the base writeback (ARM2 silicon; Acorn calls the combination
  "ambiguous"), while the JS reference lets the writeback win. Needed for
  real RISC OS ROM literal pools and list walks.

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
| `arm26_test`: rn15-psr-zero, rm15-keeps-psr, ldr-rd-rn-*-load-wins, swi0-vectors | 26-bit r15 operand rules (Rn zeroes PSR, Rm keeps it), LDR Rd==Rn load-wins writeback order, SWI 0 vectoring when `swi0_halt`=0 (§2.1, §2.2; Acorn PRM App. B, RISC OS PRM) |
| `mmio_test` (9 checks) | WP2 acceptance: registry dispatch, CPU-to-device, 26-bit address-bus mask, open bus, priority, RAM regression, top-word byte masking (§10 WP2) |
| `host_test` (11 checks) | WP5: host window layer — headless present/poll/quit, PPM snapshot contents, keymap, KART injection (§10 WP5) |
| `machine_test` (24 checks) | WP3/WP4 acceptance: dummy-ROM boots through the MEMC CAM walk (control reg + CAM writes from the ROM window unlatch the boot ROM), memory R/W via CAM, timer + VIDC-flyback (frame and forced) + KART IRQs delivered, 1M-cycle no-lockup loop, MEMC/VIDC/IOC models, PCF8583 CMOS over IOC CONTROL I2C (§10 WP3/WP4, §4.4a), plus MEMC data-abort: USR-mode podule access aborts to vector 0x10 with `lr_svc`=faulting pc+8 while SVC/claimed-device/fetch accesses stay abort-free (§3) |
| `edge_test` (28 checks) | review-pass edge battery: ARM2 corner semantics (shifter carry edges, Rs/Rn=15 rules, STM base position, LDRH Rd==Rn, UNDEF traps, exception lr offsets) and machine/bus edges (CAM aliasing, ROM mirror, open bus, DABT fetch exemption, IOC mask, VIDC mode gate) (§2, §3, §4, §7) |
| `fdc_test` (33 checks) | WP7 acceptance: WD1772 register map/status/command model, DRQ/INTRQ→IOC FIQ FH0/FH1, latches A/B, type I + read/write sector + read address + force interrupt + master reset against disc images, plus guest-code end-to-end sector read (§10 WP7; WD1772 datasheet) |
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
