---
id: adt-stalk3c-r1
type: adt
title: "stalk-3c round 1 (the syscall retirement)"
date: 2026-06-03
scope: [sub-kernel-devsrv]
reviewer: opus
model-start: "opus (2026-06 tier; exact id not recorded in the round log)"
model-end: "opus (2026-06 tier; exact id not recorded in the round log)"
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 3}
findings: [fnd-stalk3c-r1-f1, fnd-stalk3c-r1-f2, fnd-stalk3c-r1-f3]
round-of: chg-2026-06-03-stalk3c-retire
created: 2026-07-31
---
## Scope

The four stalk-3c commits: corvus create=post pre-chroot, the pouch
bind/connect seam, the kernel-test migration, the ABI break (retire
26/30/43 + the dead client-KObj_Srv r/w arms). The prosecutor built the
kernel + full userspace + all 15 pouch patches CLEAN (a forced
from-scratch sysroot rebuild ground-truthed the patch-apply) and ran the
suite; an in-session self-audit covered the r/w resolver, the
number-indexed dispatch, the SO_PEERCRED gate, and the test-migration
non-vacuousness. CONVERGED on SOUND; all three findings are
documentation staleness, swept in the close.

## Convergence

SOUND set: the KOBJ_SPOOR-only r/w resolver (rights-then-kind before any
obj cast; a KObj_Srv listener rejected exactly as before); the
number-indexed dispatch makes number retirement mis-map-proof; zero live
callers of any retired symbol (grep + a clean Rust-workspace build);
per-territory isolation prosecuted DIRECTLY — the retirement removed the
last EL0-reachable global-registry bindings, so I-1 is HELD and
STRENGTHENED; the migrated tests drive the production cores and are
non-vacuous. stalk-3 ARC COMPLETE at this close; the 9p-mode-connect
coverage gap carries forward ([[seam-srv-9p-connect-unit]]).
