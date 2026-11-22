# native/ — the native tier (M6+, not started)

Per the specification (docs/spec.md, sections 1, 7 and 10), this directory
will hold the native module set and WIMP server — the RISC OS-shaped
desktop (iconbar, pinboard, filer, `!App` directories, `*commands`,
12-bit filetypes, three-button mouse protocol) implemented as native modern
code on the BSD-derived pre-emptive, memory-protecting kernel:

- `wimp/`   — pre-emptive WIMP server, per-task address spaces, compositing
- `filer/`  — filer windows, `!App` launching, drag-and-drop
- `shell/`  — `*command` CLI and Obey scripts
- `modules/` — native module set (Wimp, Filer, Font, FileSwitch, ...) with
  `*`-command registration
- `oberon/` — the built-in Oberon-07 bytecode interpreter (the BBC BASIC
  role; spec section 7)

Milestones: M6 (first native vertical slice), M7 (unmodified RISC OS 5
binary in the desktop), M8 (Carbon-style compat library). The emulator tier
in `emu/` is the prerequisite and the behavioural oracle; it is complete
through WP3.
