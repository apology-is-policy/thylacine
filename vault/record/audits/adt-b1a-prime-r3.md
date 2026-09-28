---
id: adt-b1a-prime-r3
type: adt
title: "B-1a' (capacity) round 3: the copy-on-write copy released its share while its own stale leaf still translated to the page, and a read queued behind a peer's break was refused as a mismatch and killed"
date: 2026-09-23
scope: [sub-kernel-fault, sub-kernel-mmu, sub-kernel-image, sub-kernel-burrow, sub-kernel-exec, sub-kernel-syscall-dispatch, sub-kernel-mm-phys, spec-capacity, inv-i32]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: dirty
counts: {p0: 0, p1: 2, p2: 0, p3: 3}
findings: [fnd-b1a-prime-r3-f12, fnd-b1a-prime-r3-f13]
round-of: chg-2026-09-23-b1a-prime-close-r2
created: 2026-09-23
---
## Scope

Branch `b1a-prime-wip` at f1497e7f (WIP 7, the round-2 fixes: the pool's
reclaim of idle images, the FILE holder charge, the leaf replace, keep-tables,
`-T_E_NOMEM` at attach / Loom). Read-only. The brief asked for the reclaim's
lock order and reentrancy, the {1,0} idleness proof under the strip on every
teardown path including death, the holder charge / refund pairing on every
path, the replace's break-before-make against peer CPUs, keep-tables' durable
occupancy-0 state, the `-T_E_NOMEM` sweep, the probe's census, the tests'
physical controls, and the two round-2 withdrawals the fixes had voided.

## Verdict

0 P0 / 2 P1 / 0 P2 / 3 P3; dirty by the shape of what it found, not by the
count: F12 is a REGRESSION the F9 fix introduced (the uninstall it removed
had been the load-bearing half of the I-44 argument), F13 is older than the
chunk but sits in the function the fix rewrote and voids the idempotent-
install proof the code still stated. Both P1s carry deterministic witnesses.
The withdrawals -- the reclaim's lock order, the {1,0} proof under the
strip, the charge pairing, the BBM, keep-tables, the tag, the pool edge,
I-36, the holes, the probe's census, the spec's disclosure -- stand.

## Findings

- [[fnd-b1a-prime-r3-f12]] [P1] the copy branch's early put (fixed).
- [[fnd-b1a-prime-r3-f13]] [P1] the queued reader refused as a mismatch (fixed).
- F14 [P3] the strip took a whole image per pick, under `g_image_lock` and
  the faulter's `as->lock`: fixed in its first half (`burrow_image_strip(v,
  want)`); its second half -- give up when the pool is still over-full after
  a reclaim -- declined with a reason: an exempt overshoot is the reserve's
  designed case and the bar forbids refusing a user while idle cache pages
  exist; with the strip bounded, the loop frees the overshoot plus the
  request, once.
- F15 [P3] a pool refusal inside exec surfaced as EINVAL: fixed
  (`-T_E_NOMEM` through both mappers and `sys_execve_core`).
- F16 [P3] the F9 witness satisfied by the defect it named and the F8 refund
  witness blind to leaves versus slots: fixed (the L3 table's PA and
  `mmu_uninstall_pte_calls()` pinned across the break; the unfaulted half
  detached first).

The close is [[chg-2026-09-23-b1a-prime-close-r3]]; round 4 ran on it.
