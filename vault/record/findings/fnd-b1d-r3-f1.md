---
id: fnd-b1d-r3-f1
type: fnd
title: "The confined leg's control was one variable away on the pivot axis and zero on the working-directory axis"
round: adt-b1d-r3
severity: P3
status: fixed
surface: [sub-pouch-mem]
threatens: []
fixed-by: chg-2026-09-25-b1d-round3-close
regression: "pouch-hello-dlopen confined leg: post-pivot open(\"..\") must succeed (exit 8); sabotage U6 (drop the chdir): GREEN at a157d47c, RED after the fix (child confined exited 8)"
created: 2026-09-25
---
## Prosecution

Round 2's fix ([[fnd-b1d-r2-f1]]) made the confined child chdir to `/` and
open `../lib/libdlprobe.so` before the pivot as a control. Delete the chdir and
the child keeps `/bin`: the pre-pivot control joins to `/bin/../lib/X`, which
the initrd resolves (`bin` exists there, and the `..` pops it), so the control
passes; after the pivot the deny name misses at `bin` again. Every leg stays
green, which is the vacuity round 2 fixed, returned without a sound. The main
session's parallel self-audit found the same gap independently.

## Disposition

Fixed. After the pivot the child must open `..` (exit 8 otherwise): from `/`
the `..` clamps at the new root and opens it, from `/bin` it misses at `bin`.
Pouch passes the name to the kernel verbatim (patch 0030's non-create open), so
the kernel's join is what the control tests.
