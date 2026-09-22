---
id: adt-pouchb0-r1
type: adt
title: "Pouch 0033-0035 round 1: the patches are sound; their siblings and their sentences are not"
date: 2026-09-21
scope: [sub-pouch-seam, sub-pouch-thread, sub-pouch-net, sub-kernel-exec]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: dirty
counts: {p0: 0, p1: 1, p2: 3, p3: 6}
findings: [fnd-pouchb0-r1-f1, fnd-pouchb0-r1-f2, fnd-pouchb0-r1-f3, fnd-pouchb0-r1-f4]
round-of: chg-2026-09-21-pouch-b0-libc
created: 2026-09-21
---
## Scope

Branch `browser-b0` @ e0fc2422 against `main` 47ba3295: patches 0033 / 0034 / 0035, the series file, three provers, the `exec.h` comment, three dossiers. Read-only. The reviewer built a differential stdio model (0035: 0 / 32,000; the 0002 backend about 1,890 / 2,000 -- the positive control) and fuzzed the 0034 parser (3,000,000 inputs, no out-of-bounds read).

## Convergence

The three patches do what their code says. Every finding is about what the change CLAIMS versus what it PINS, or a SIBLING of the fixed defect: the same unchecked-wrapper bug sixty lines up in the function 0034 had just fixed ([[fnd-pouchb0-r1-f1]]), a `tmpfile()` that had never unlinked ([[fnd-pouchb0-r1-f2]]), a device pin that compared libc's literals with the prover's own copy of them ([[fnd-pouchb0-r1-f3]]), and stdio backends that handed a socket tag to the kernel raw ([[fnd-pouchb0-r1-f4]]). Recorded `dirty` for the fixes, not the count: F2's suggested fix turned the boot red and became a redesign (delete-on-close), and F4 became a new patch. The six P3s and their dispositions are in `memory/audit_pouch_0033_0035_closed_list.md`.
