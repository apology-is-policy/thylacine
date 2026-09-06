# 21 — ELF64 ARM64 loader (P2-G) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-elf-doc-absorb`). The
ELF64 loader — the validator that turns untrusted bytes into a checked segment
description (or refuses them), enforcing W^X (I-12) at parse time. Its content is
carried, more currently and more completely, by:

- the **loader itself** — `elf_load`, the full rejection taxonomy, the W^X
  type-blind check (hoisted above the type switch), the ET_DYN/PIE placement at
  `ELF_PIE_LOAD_BIAS` (D-2), the shared `elf_read_interp` walk and its #215
  alignment guards (F61/F62), the `AT_PHDR`/`phoff` auxv inputs, and the
  8-byte-alignment preconditions:

      vault/system/kernel/execution/sub-kernel-elf.md

The segment **MAPPING** this file defers to "Phase 3" (segment → Burrow → VMA,
demand-paging, the file-backed REVENANT arm) is the exec side's, not the
loader's — `sub-kernel-exec` / the exec-load surface own it; `elf_load` still
only parses.

**What this file got WRONG or MISSED by the time it was absorbed:** its P2-Ga
framing is the stale part; the body is largely current but superseded by a
more-complete dossier.

- Its header says "the actual MAPPING of segments … is deferred to Phase 3" and
  "the `exec()` syscall surface lands at Phase 5+" — both landed long ago
  (mapping + exec + the REVENANT file-backed demand-paged arm are built).
- The rejection taxonomy has **twenty-four** distinct codes now; this file
  tabulates ~21 (the prose long undercounted, and D-2/D-4 added their own).
- `ELFOSABI_GNU` is now accepted alongside `ELFOSABI_NONE`; this file lists only
  `ELFOSABI_NONE`.
- Its "static binaries only / PT_INTERP rejected" framing is only half the story
  post-D-4: `elf_load` still refuses `PT_INTERP`, but the vivarium exec
  chokepoint reads it via `elf_read_interp` and rewrites the exec onto the
  interpreter, so Linux dynamic binaries run (the reject is what makes the
  one-image rule *structural*, per `sub-kernel-elf`).
