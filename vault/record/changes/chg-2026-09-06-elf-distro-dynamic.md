---
id: chg-2026-09-06-elf-distro-dynamic
type: chg
title: "kernel-elf de-stale: the DISTRO D-2/D-4 dynamic-binary surface -- ET_DYN/PIE placement + AT_ENTRY (D-2), the shared elf_read_interp for the rewrite-to-ldso route (D-4/#215), and the measured code count (twenty-four, not twenty-two)"
date: 2026-09-06
arc: arc-vault
commits: []
touched:
  - sub-kernel-elf
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---
[[sub-kernel-elf]] read `updated: 2026-08-03`, and its whole thesis -- "a
validator that refuses more than it accepts", static `ET_EXEC` only, "dynamic
linking refused permanently" -- was INVERTED by the DISTRO arc (three commits
`61e7f57e..HEAD`, +235 lines on `kernel/{elf.c,elf.h}`) it never mentioned.
Ground-truthed by reading both diffs and the current code; the header comments
are essentially the spec. Folded:

- **D-2: ET_DYN / PIE placement.** `ET_DYN` joins `ET_EXEC`; `elf_load` adds
  `ELF_PIE_LOAD_BIAS` (512 MiB) to `e_entry` and every `PT_LOAD` vaddr in ONE
  place, so everything downstream reads FINAL addresses unchanged (`bias == 0`
  keeps the `ET_EXEC` path byte-identical). The bias is 64 KiB-aligned so the
  gABI `p_vaddr == p_offset (mod p_align)` congruence survives (preserving a
  segment's shared-file-backed-arm eligibility); the biased span is bounded to
  the PIE window (`ELF_LOAD_PIE_OOB`). `PT_DYNAMIC` narrowed to ET_EXEC-only (a
  PIE carries one legitimately, never PROCESSED -- static-PIE and stock ldso both
  self-relocate from their entry). New `struct elf_image` fields `load_bias` +
  `type`; `AT_ENTRY` (tag 9) emitted -- NOT what makes ldso work (musl writes it
  itself on the direct-invocation branch, #186), but the v1.x dual-image lift
  needs it. ONE constant deliberately (per-exec randomization = an I-16 seam).
- **D-4: the rewrite-to-ldso route.** `PT_INTERP` is STILL rejected AT
  `elf_load`; the vivarium exec chokepoint reads it via the new PUBLIC
  `elf_read_interp` and restarts resolution on the interpreter, so a Linux
  dynamic binary RUNS (the interpreter reaching `elf_load` carries no PT_INTERP,
  so the reject stays correct on both sides). `elf_read_interp` is the bounded
  walk `elf_brand_hint` used to carry privately, promoted to ONE home because the
  rewrite ACTS on the path and a second copy would let the bounds judgement drift
  (#140). #215 gave the promoted walk its sibling's alignment guards (R5-G
  F61 buffer + F62 attacker-controlled phoff).
- **The code count MEASURED**: twenty-FOUR now, not the prose's "twenty-two"
  (which undercounted by one at 23 even at the dossier's own commit, then D-2
  added `ELF_LOAD_PIE_OOB`). `ELF_LOAD_HAS_DYNAMIC` now fires only on an ET_EXEC.

Folded into Contract, Mechanism (the PIE bias + the shared interp walk),
Data structures, Invariants (the W^X leg is byte-identical under bias), Error
paths, Seams, Provenance; `design:` gains docs/DISTRO.md. `updated:` ->
2026-09-06. Stale backlog 40 -> 39.
