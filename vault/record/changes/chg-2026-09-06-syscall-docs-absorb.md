---
id: chg-2026-09-06-syscall-docs-absorb
type: chg
title: "absorb docs/reference/28-syscall (P3-Ec, the two-syscall minimum surface): superseded by the ~107-syscall ABI; zero-fold multi-redirect stub to syscall-dispatch + proc + cons"
date: 2026-09-06
arc: arc-vault
commits: ["d6c3469a"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The P3-Ec milestone doc for userspace syscall dispatch -- the absolute-minimum
surface (`SYS_EXITS = 0` / `SYS_PUTS = 1`) that first proved EL0 could trap and
be answered. Superseded by the frozen ~107-syscall ABI. Zero-fold: every current
atom already lives in a comprehensive dossier, verified atom-by-atom (not a bare
"covered"):

- the dispatcher (`x8` nr / `x0..x5` args / `x0` return / unknown -> -1), the SVC
  entry, and the two-tier staging + copy-before-role discipline that `SYS_PUTS`
  established -> `sub-kernel-syscall-dispatch` (its subject; I folded the A-3
  syscall-path atoms into it earlier this run, so it is fresh);
- `SYS_EXITS` -> `exits()` -> `sub-kernel-proc`;
- `SYS_PUTS` -> the shared `cons_output_write` path (role / ring / ONLCR / the
  `#76` short-count) -> `sub-kernel-cons`.

The uaccess fault mechanism it cross-references is `40-uaccess.md`'s (still live).

What it got wrong (in the stub): the two-syscall enum is a placeholder (ABI froze
at Phase 5); caveat 1 "no userspace pointer validation" is FALSE -- the staging
path validates the VA and recovers from a fault via `userland_demand_page` + the
fixup label (a whole-op EFAULT, not an extinction), which the doc's own `#76`/R12
body contradicts; "copy_from_user stubbed" is built; the `imm16`-unused and
unstable-numbers notes are stale.

No dossier content changed. Render clean; lint 0-fail. view-absorption 68 -> 69.
