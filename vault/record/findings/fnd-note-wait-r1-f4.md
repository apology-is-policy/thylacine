---
id: fnd-note-wait-r1-f4
type: fnd
round: adt-note-wait-r1
severity: P3
status: fixed
title: "joey's capture margin"
surface: [sub-kernel-notes, sub-pouch-signal]
threatens: []
fixed-by: chg-2026-11-03-note-wait
regression: "run-lease records the prover bytes at the green line"
created: 2026-11-03
---
## Prosecution

21 new per-leg lines put the prover near joey's 4 KiB reap-first ring. A wide failure could fill it, block the child in write, and lose the lines that say why, though the verdict survives.

## Disposition

The note-wait legs print on failure only, with one summary line, which keeps the output near the measured 2.3 KiB. joey is untouched.
