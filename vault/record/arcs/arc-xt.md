---
id: arc-xt
type: arc
title: "XT -- x86 translation, the Rosetta-class layer"
status: active
design: ["docs/X86-TRANSLATION-DESIGN.md", "docs/xt-status.md"]
chunks: []
follow-ons: []
exit-criteria:
  - "[ ] An x86-64 or i386 Linux program runs by plain exec from a declared tree or container, translated by a native runtime, with no prompt"
  - "[ ] Its faults, signals and threads behave as on Linux; its Linux semantics are Vivarium's"
  - "[ ] A second run uses stored, shared translated text"
  - "[ ] An x86 SDL program draws and sounds through native Thylacine libraries"
  - "[ ] I-48 and the amendments are in ARCH section 28; fault_note and hosted_decode pass their clean and buggy configs; every audit row is closed"
  - "[ ] No page is ever writable and executable, and a translated program executes only guest code a native program could"
  - "[ ] The kernel contains no x86"
created: 2026-10-08
---
## Goal

x86 software runs on Thylacine as ordinary processes: launched by `exec`,
granted what translation needs by the system rather than by a prompt, and
sharing translated code as Plan 9 shares text. The kernel gains ISA-neutral
primitives only. Ratified by [[dec-2026-10-08-xt-design]] (F1-F9) and
[[dec-2026-10-08-xt-guest-code]] (F10). The design is
`docs/X86-TRANSLATION-DESIGN.md`; the
motivating survey is `docs/WINE-STUDY.md`; live state is `docs/xt-status.md`.

## Planned chunks

- **XT-0:** ratification, the scripture commit.
- **XT-1:** exact faults, with `fault_note.tla`.
- **XT-2:** thread-directed notes and `usr:*` note names.
- **XT-3:** thread reaping and feature discovery (study F2, F3).
- **XT-4:** address-space shapes.
- **XT-5:** objtype declaration, the objtype table, foreign exec and the
  entitlement.
- **XT-6:** hosted Procs, with `hosted_decode.tla`; guest-X marking;
  don't-fork code regions.
- **XT-7:** the memory-model primitive.
- **XT-8 to XT-13:** the FEX port, the amd64 territory, i386, the store,
  bridges, and the Pi 400 engine evaluation.
- **XT-14:** hand-off to Wine.

## Close summary
(written at status flip to complete)
