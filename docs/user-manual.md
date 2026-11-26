# SOPHIE — User Manual

This manual covers building, testing and using the SOPHIE emulator tier on a
host machine (the only part that runs without target hardware today). For the
project's architecture and design decisions see [spec.md](spec.md); for the
technical reference (memory map, device registers, API, test traceability)
see [reference-manual.md](reference-manual.md).

## 1. What SOPHIE is

SOPHIE is a small OS project for Raspberry Pi whose long-term shape is a
modern BSD-core kernel with a natively implemented RISC OS-idiom desktop,
plus full-machine emulation of Acorn Archimedes / Risc PC machines running
real RISC OS ROMs (including current ROOL RISC OS 5 builds). The emulator is
an ordinary userland process; all "RISC OS-ness" lives inside the emulated
machine (spec section 1).

Today the repository contains the emulator tier's foundations, built and
tested on any host:

- `emu/core/` — the **arm26** ARMv2/ARMv3 26-bit interpreter core (C11 port
  of the JS reference, spec Appendix A);
- `emu/mach/` — the memory bus / device registry and the **Archimedes base
  machine**: MEMC, VIDC and IOC device models;
- `emu/tests/` — the test suites and benchmark;
- `emu/ref/` — the JS reference implementation, kept for parity checks and
  benchmarking.

## 2. Requirements

- A C11 compiler (`cc`/`gcc`/`clang`)
- GNU `make`
- Optional: `node` (to also run the JS reference suite and benchmark)
- No target hardware is required for anything in this repository today.

## 3. Building

```sh
make            # build library objects, tests and benchmark into emu/
make clean      # remove build products
```

The build is plain `make` (spec section 10): no build system heavier than
make. Compiler and flags can be overridden:

```sh
make CC=clang CFLAGS="-O2 -std=c11 -Wall -Wextra"
```

Build products land next to their sources (`emu/core/*.o`, `emu/mach/*.o`,
`emu/tests/arm26_test`, `emu/tests/mmio_test`, `emu/tests/machine_test`,
`emu/tests/bench`). Header dependencies are tracked automatically (`-MMD`).

## 4. Running the tests

```sh
make test
```

This runs six suites:

| Suite | Binary | What it proves |
| --- | --- | --- |
| `arm26` | `emu/tests/arm26_test` | The C core passes all **42 assertions** of the transplanted JS suite (spec Appendix B): data processing and flags, shifter carries, MUL, SWP, LDR/STR incl. unaligned-load rotation, LDM/STM all four stack orders with r15-as-CPSR stores, exception entry (SWI/IRQ/undefined), MOVS pc full-CPSR restore, TEQP mode switch, loops, SWI-0 halt convention. |
| `arm26_ext` | `emu/tests/arm26_ext_test` | WP6 core extensions (**15 assertions**): group-011 register-offset LDR/STR, ARMv4 halfword/signed transfers (LDRH/STRH/LDRSB/LDRSH), `LDR pc`, undefined-space traps. |
| `mmio` | `emu/tests/mmio_test` | WP2: the device registry routes rd8/wr8/rd32/wr32, the CPU reaches MMIO devices through the bus, unmapped accesses are safe open-bus, region priority, and the flat-RAM path is unaffected. |
| `machine` | `emu/tests/machine_test` | WP3/WP4: a hand-built dummy ROM boots the Archimedes base machine through the MEMC CAM walk, reads/writes emulated memory, receives IOC timer, VIDC-flyback (vsync) and KART keyboard IRQs, and an IRQ-driven loop runs **1,000,000 cycles without lockup**; plus ROM-window mirroring, VIDC framebuffer rendering and the keyboard path. |
| `host` | `emu/tests/host_test` | WP5 host window layer (**11 assertions**): headless present/poll/quit cycle, PPM snapshot contents, host→RISC OS keymap, and `host_pump_input()` injecting KART make/break bytes into the machine. |
| `fdc` | `emu/tests/fdc_test` | WP7 (**33 assertions**): WD1772 floppy controller model — register map at 0x03310000, type I/seek/read/write sector/read address/force-interrupt commands, DRQ/INTRQ wired to IOC FIQ FH0/FH1, drive latches at 0x03250040/0x03250018 — verified against real disc images, including an end-to-end guest program that reads a 1024-byte sector through the memory bus. |

Each suite prints `name: N passed, M failed` and exits non-zero on any
failure; `make test` stops at the first failing suite.

The JS reference suite can be run on its own for a parity check:

```sh
node emu/ref/arm26.test.mjs     # prints "arm26: 42 passed, 0 failed"
```

## 5. Benchmarking

```sh
make bench
```

runs `emu/tests/bench` (the C core) and, if `node` is available,
`emu/ref/bench.mjs` (the JS reference), each executing the same 1,000,000
instruction loop. The WP1 acceptance criterion is that the C core is within
3x of the JS reference on the same host; measured here it is ~12x *faster*
(e.g. ~292 MIPS C vs ~23 MIPS JS on an x86-64 host), i.e. ample headroom
over the ~8–14 MIPS of real Archimedes-era hardware (spec sections 2 and 9).

## 6. Using the emulator library

The machine model is a plain C library; a host frontend (window layer, WP5)
attaches to it. Minimal example:

```c
#include "machine.h"

static uint8_t ram[0x400000];          /* 4 MB DRAM */
static uint8_t rom[0x80000];           /* ROM image (acquired separately) */

Machine m;
machine_init(&m, ram, sizeof ram, rom, sizeof rom);
machine_run(&m, 1000000);              /* run 1M instructions, IOC timers tick along */

uint32_t fb[640 * 480];
vidc_render(&m.vidc, &m.bus, fb, 640, 480);   /* current VIDC mode -> 32bpp framebuffer */
ioc_key_press(&m.ioc, 'A');                   /* inject a keypress (KART) */
```

Memory map and device register details are in the
[reference manual](reference-manual.md). ROM images are runtime data, not
repo content at release (spec section 5; see README/NOTICE for the
development-vs-distribution policy): the WP4+ loader reads them from a local
`roms/` directory (gitignored), e.g. `roms/ROM300` for RISC OS 3.00 or
`roms/riscos5-5.30-pi.img` for RISC OS 5.30.

ROM boot smoke test (WP4 groundwork — not part of `make test`):

```sh
make                                   # builds emu/tools/romboot
emu/tools/romboot roms/ROM300 5000000  # run 5M instructions, print final state
```

Host window frontend (WP5 — not part of `make test`):

```sh
emu/tools/sophie roms/ROM300 100 --ppm /tmp/frame.ppm   # headless: PPM snapshot per frame
emu/tools/sophie roms/ROM300 2000 --keys "*Help\n" --keys-at 500 --ppm /tmp/f.ppm  # type into the guest
emu/tools/sophie x 20 --stub-desktop --ppm /tmp/stub.ppm  # M3 stub desktop (no ROM needed)
make -C emu host-x11 && DISPLAY=:0 emu/tools/sophie-x11 roms/ROM300   # real X11 window
```

`sophie` boots the ROM in the Archimedes machine, renders each VIDC frame
through the host window layer and pumps host keyboard input into the IOC
KART. The default build is headless (PPM snapshots via `--ppm`, scripted
input in tests); `make host-x11` adds a real X11 window backend (libX11 is
dlopen'd at runtime, so the default build has no X11 dependency).

`--keys "text"` types text into the guest one character per 4 frames starting
at `--keys-at` frame (`\n` = RETURN, `\t` = TAB) — used to drive the CLI/Wimp
(M3). `--stub-desktop` skips the ROM boot and renders a built-in stub desktop
(backdrop, icon bar, window, pointer) through the real VIDC/MEMC pipeline —
a placeholder for the visible desktop (M3) while the ROM boot stall
(GitHub issue #3) is unresolved.

Disc images (WP7 — not part of `make test`). Create a blank, formatted ADFS
floppy image (ADFS L, 640K, old map) and attach it to floppy drive 0:

```sh
emu/tools/mkadfs /tmp/blank.adl MyDisc      # blank ADFS L image, name "MyDisc"
emu/tools/sophie roms/ROM300 100 --ppm /tmp/frame.ppm --disc 0=/tmp/blank.adl
emu/tools/romboot roms/ROM300 5000000 --disc 0=/tmp/blank.adl   # smoke run
```

`--disc N=path` attaches a raw sector image to drive N (0-3); `--ro` attaches
it write-protected. Supported image sizes are the ADFS S/M/L/D/E/F/G and DOS
360K/400K/720K geometries (see the reference manual §4.6); the WD1772 is
modelled at 0x03310000 with DRQ/INTRQ on IOC FIQ bits FH0/FH1, so guest code
driving the controller sees a real filing-system device.

Battery-backed CMOS RAM (PCF8583, reference manual §4.4a) persists via
`--cmos FILE`: the 240-byte config region (0x10-0xFF) is loaded before the
boot and saved on exit (the file is created on first save), so *Configure
settings survive across runs. The PCF8583 clock registers tick at 100 Hz
from the emulated 8 MHz clock. The built-in defaults are the RISC OS 3
*Configure map; RISC OS 2 uses a different CMOS map, so with ROM200 point
`--cmos` at a file the RO2 install has written itself:

```sh
emu/tools/sophie roms/ROM200 100 --ppm /tmp/frame.ppm --cmos /tmp/rom200.cmos
emu/tools/romboot roms/ROM200 5000000 --cmos /tmp/rom200.cmos
```

## 7. Repository layout

```text
emu/
  core/arm26.[ch]     ARMv2/3 26-bit interpreter core (WP1)
  mach/mem.[ch]       memory bus + device registry (WP2)
  mach/memc.[ch]      MEMC model        \
  mach/vidc.[ch]      VIDC model         > Archimedes base machine (WP3)
  mach/ioc.[ch]       IOC model         |
  mach/machine.[ch]   memory map + wiring /
  tests/              arm26_test, mmio_test, machine_test, bench, test.h
  ref/                JS reference (arm26.mjs, arm26.test.mjs, bench.mjs)
native/               native module set / WIMP server (M6+; not started)
docs/                 spec.md, user-manual.md, reference-manual.md
Makefile, emu/Makefile
LICENSE (Apache-2.0), NOTICE
```

## 8. Conventions

- C11, no C++; plain make; hand-encoded instruction words in tests (no
  external assembler), per spec section 10.
- Every work package ends with the full host test suite passing.
- Test names correspond one-to-one with the spec's traceability matrix (see
  reference manual section 8); the suite doubles as the RISC-V/PowerPC
  host-port acceptance criterion (spec section 3).
- The emulator core contains no host-ISA assumptions; all guest memory
  access is mediated byte-wise through the bus (spec section 3, PowerPC
  plan: endianness and alignment).

## 9. Licensing and acquisition boundaries

Original code is Apache-2.0 (`LICENSE`, `NOTICE`). The emulator tier is
written clean from primary specifications; RPCEmu (GPL v2) and RPCEmu
Extended, Arculator (closed), and MAME's Acorn drivers are **reference and
credit only** — no code or pseudocode is derived from them. RISC OS ROM
images and Wi-Fi firmware blobs are runtime-loaded data acquired separately
under their own terms and are not part of this distribution. See README.md
and spec section 5.
