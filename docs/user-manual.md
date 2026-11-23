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

This runs three suites:

| Suite | Binary | What it proves |
| --- | --- | --- |
| `arm26` | `emu/tests/arm26_test` | The C core passes all **42 assertions** of the transplanted JS suite (spec Appendix B): data processing and flags, shifter carries, MUL, SWP, LDR/STR incl. unaligned-load rotation, LDM/STM all four stack orders with r15-as-CPSR stores, exception entry (SWI/IRQ/undefined), MOVS pc full-CPSR restore, TEQP mode switch, loops, SWI-0 halt convention. |
| `mmio` | `emu/tests/mmio_test` | WP2: the device registry routes rd8/wr8/rd32/wr32, the CPU reaches MMIO devices through the bus, unmapped accesses are safe open-bus, region priority, and the flat-RAM path is unaffected. |
| `machine` | `emu/tests/machine_test` | WP3: a hand-built dummy ROM boots the Archimedes base machine, reads/writes emulated memory, receives IOC timer IRQs, and an IRQ-driven loop runs **1,000,000 cycles without lockup**; plus VIDC framebuffer rendering and the IOC keyboard path. |

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
