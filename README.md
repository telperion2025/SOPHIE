# SOPHIE

**SOPHIE** — named in honour of **Sophie Wilson**, designer of the BBC Micro's
architecture and of the original ARM instruction set — is a small operating
system project for Raspberry Pi (primary target Pi 400; compatibility range
Pi 3 through Pi 5/500) that:

1. **emulates** Acorn Archimedes-era and Risc PC machines (real ROMs, real
   RISC OS — current ROOL RISC OS 5 builds included);
2. **runs RISC OS 5 binaries natively** through a compatibility layer on a
   modern pre-emptive, memory-protecting, SMP-capable BSD-derived kernel;
3. **natively runs its own applications** in an OS whose structure, look and
   feel — WIMP desktop, `!Application` directories, `*commands`, 12-bit
   filetypes, three-button mouse — follows the RISC OS idiom.

Planned future host ports: **RISC-V** and **PowerPC** (see
[docs/spec.md](docs/spec.md) sections 3–4).

The full design specification is exported at **[docs/spec.md](docs/spec.md)**.
User documentation: **[docs/user-manual.md](docs/user-manual.md)**. Technical
reference: **[docs/reference-manual.md](docs/reference-manual.md)**.

## Current status

Host-buildable work packages WP1–WP3 (spec section 10) are implemented and
tested:

| WP | Deliverable | Status |
| --- | --- | --- |
| WP1 | C11 port of the `arm26` ARMv2/3 26-bit interpreter core + transplanted 42-assertion suite | **done** — `emu/core/`, `emu/tests/arm26_test.c` (42/42 pass; ~292 MIPS vs ~23 MIPS for the JS reference on the same host) |
| WP2 | MMIO dispatch layer: device registry routing rd8/wr8/rd32/wr32 | **done** — `emu/mach/mem.[ch]`, `emu/tests/mmio_test.c` (9/9 pass) |
| WP3 | Archimedes base machine: MEMC, VIDC, IOC device models + dummy-ROM boot test (1M-cycle IRQ-driven loop) | **done** — `emu/mach/{memc,vidc,ioc,machine}.[ch]`, `emu/tests/machine_test.c` (13/13 pass) |
| WP6 (core part) | ARMv4 core extensions: group-011 register-offset LDR/STR, halfword/signed transfers (LDRH/STRH/LDRSB/LDRSH), `LDR pc` | **done** — `emu/core/arm26.c`, `emu/tests/arm26_ext_test.c` (15/15 pass); `emu/tools/romboot.c` smoke-boots real ROM images from `roms/`. Risc PC (IOMD/VIDC20) machine model: pending |
| WP4 (machine part) | MEMC1A CAM walk (reset ROM latch, data-encoded page entries via the ROM window, 4K/8K/16K/32K pages, control/DMA registers); VIDC flyback (vsync) IRQ via IOC IRQA bit 3; IOC KART keyboard path with make/break queue | **done** — `emu/mach/{memc,vidc,ioc,machine}.[ch]`, `machine_test` 18/18 (dummy ROM boots through the CAM; timer + vsync + keyboard IRQs verified end to end), `romboot`: ROM300 passes POST and settles in the OS idle loop (stable polling loop with IRQs flowing, 200M cycles, zero unmapped reads — spec M2); ROM200/ROM300 run from CAM-mapped DRAM; ROM window mirrors across the full 8MB (0x03800000–0x03FFFFFF) as on hardware |
| WP4–WP5, WP7 | Real ROM boot to desktop, host window layer, disc images | ROMs now available locally in `roms/` (dev branch carries `ROMS/`); needs WD1772 floppy + fuller VIDC modes, then the WP5 host window layer; see spec section 10 |
| M6+ | Native RISC OS-shaped desktop / module set | `native/` — not started (per spec) |

## Quick start (host build)

Requirements: a C11 compiler (`cc`), `make`, and optionally `node` (to run
the JS reference suite/benchmark for comparison).

```sh
make          # build the emulator library, tests and benchmark
make test     # run the full test suite (arm26 42 + arm26_ext 15 + mmio 8 + machine 13 assertions)
make bench    # C benchmark, then the JS reference benchmark if node is present
make clean
```

No target hardware is needed for any of the above.

## Repository layout

The repository root corresponds to the `sophie/` directory in the spec's
layout (spec section 10):

```text
emu/
  core/         arm26: ARMv2/3 26-bit interpreter core (WP1)
  mach/         memory bus + device registry (WP2), MEMC/VIDC/IOC/machine (WP3)
  tests/        arm26_test, mmio_test, machine_test, bench
  ref/          JS reference implementation (spec Appendix A/B) for parity + benchmarking
native/         future native module set / WIMP server (M6+; not started)
docs/
  spec.md             export of the design specification
  user-manual.md      build/run/usage manual
  reference-manual.md architecture, memory map, device registers, test traceability
LICENSE         Apache-2.0 (original code)
NOTICE          reference credits
```

## Licensing and acquisition boundaries

Original code in this repository is **Apache-2.0** (see `LICENSE`, `NOTICE`).
The emulator tier is written clean from primary specifications (ARM ARM,
Acorn MEMC/VIDC/IOC documentation, ROOL's Apache-2.0 sources and the RISC OS
PRMs); GPL'd reference emulators (RPCEmu, RPCEmu Extended) and the closed
Arculator are **studied as behaviour reference and credited only** — no code
or pseudocode is derived from them (spec section 5).

ROM-image policy (development vs. distribution), per the spec:

- **During development**, the development branch carries a `ROMS/` folder
  with the images used for the WP4/M2.5 boot work: `ROMS/ROM200` and
  `ROMS/ROM300` (Acorn RISC OS 2.00/3.00 images) and `ROMS/BCM2835.5.30/`
  (ROOL's RISC OS 5.30 Raspberry Pi image, including Broadcom boot firmware
  under its own `LICENCE.broadcom`). The WP4+ loader instead reads ROM images
  from a local, gitignored `roms/` directory.
- **For any distribution or public release**, the Acorn ROM images must be
  removed first: their redistribution rights are unconfirmed, so they are
  *not* licensed under the Apache-2.0 grant and must not ship with the code
  (see `NOTICE`). Release artifacts contain code only; users supply their own
  ROM images into `roms/` at runtime. The same applies to **Wi-Fi firmware
  blobs** (Broadcom/Cypress family): loaded at runtime as data under their own
  terms, never linked into or distributed with the licensed code.

## References

- RPCEmu (GPL v2) — <https://www.marutan.net/rpcemu/index.php>; RPCEmu
  Extended — <https://github.com/andrewtimmins/rpcemu-extended> — behaviour
  reference and credit only.
- Arculator (freeware, closed source) — Archimedes-era behaviour reference only.
- ROOL RISC OS sources (Apache-2.0) and RISC OS 5 PRMs — behavioural reference.
- MAME's Acorn device drivers — register-layout facts for MEMC/VIDC/IOC.
- ARM Architecture Reference Manual — the authoritative ISA specification.
