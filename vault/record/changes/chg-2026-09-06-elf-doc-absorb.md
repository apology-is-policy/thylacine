---
id: chg-2026-09-06-elf-doc-absorb
type: chg
title: "absorb docs/reference/21-elf (P2-Ga ELF loader): superseded by sub-kernel-elf (more current -- 24 error codes, ELFOSABI_GNU, D-2/D-4); zero-fold stub"
date: 2026-09-06
arc: arc-vault
commits: []
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The P2-Ga ELF-loader doc. Zero-fold: sub-kernel-elf (audit:hard, inv-i12, updated
2026-09-06) covers the whole loader and is MORE current -- verified atom-by-atom
(the coverage check earned its keep: a mis-escaped grep first read "0 hits" on
every atom, and READING the dossier showed it plainly covers all of them -- the
check-the-checker lesson).

Ahead of 21-elf: 24 distinct rejection codes (the doc tabulates ~21); ELFOSABI_GNU
accepted alongside NONE; the D-2 PIE placement + D-4 elf_read_interp shared walk
(F61/F62/#215 alignment guards); the W^X type-blind hoist; the AT_PHDR auxv inputs.

Redirect: sub-kernel-elf (the loader). The segment MAPPING this doc defers to
"Phase 3" is the exec side's (sub-kernel-exec), not the loader's. What it got
wrong: the "mapping deferred / exec Phase 5+" header framing (both long built),
the error-code undercount, the missing ELFOSABI_GNU, the "static-only" framing
(D-4 runs dynamic binaries via the interp rewrite).

No dossier content changed. Render clean; lint 0-fail. view-absorption 70 -> 71.
