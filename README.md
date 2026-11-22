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
| WP2 | MMIO dispatch layer: device registry routing rd8/wr8/rd32/wr32 | **done** — `emu/mach/mem.[ch]`, `emu/tests/mmio_test.c` (8/8 pass) |
| WP3 | Archimedes base machine: MEMC, VIDC, IOC device models + dummy-ROM boot test (1M-cycle IRQ-driven loop) | **done** — `emu/mach/{memc,vidc,ioc,machine}.[ch]`, `emu/tests/machine_test.c` (13/13 pass) |
| WP4–WP7 | Real ROM boot, host window layer, ARMv4 extensions, disc images | blocked on RISC OS ROM-image acquisition (see *Licensing and acquisition boundaries*) and host windowing; see spec section 10 |
| M6+ | Native RISC OS-shaped desktop / module set | `native/` — not started (per spec) |

## Quick start (host build)

Requirements: a C11 compiler (`cc`), `make`, and optionally `node` (to run
the JS reference suite/benchmark for comparison).

```sh
make          # build the emulator library, tests and benchmark
make test     # run the full test suite (arm26 42 + mmio 8 + machine 13 assertions)
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

Two acquisition boundaries are kept explicit, per the spec:

- **RISC OS ROM images** are *not* distributed here; they are a separate
  acquisition problem from the emulator code (ROOL publishes sources under
  Apache-2.0; ROM images you load into the emulator are obtained separately).
- **Wi-Fi firmware blobs** (Broadcom/Cypress family) are loaded at runtime as
  data under their own redistributable terms, never linked or mixed into the
  licensed code.

## References

- RPCEmu (GPL v2) — <https://www.marutan.net/rpcemu/index.php>; RPCEmu
  Extended — <https://github.com/andrewtimmins/rpcemu-extended> — behaviour
  reference and credit only.
- Arculator (freeware, closed source) — Archimedes-era behaviour reference only.
- ROOL RISC OS sources (Apache-2.0) and RISC OS 5 PRMs — behavioural reference.
- MAME's Acorn device drivers — register-layout facts for MEMC/VIDC/IOC.
- ARM Architecture Reference Manual — the authoritative ISA specification.
