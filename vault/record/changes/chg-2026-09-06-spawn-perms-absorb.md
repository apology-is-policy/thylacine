---
id: chg-2026-09-06-spawn-perms-absorb
type: chg
title: "absorb docs/reference/73-sys-spawn-with-perms: fold the SPAWN_PERM_* grant-gate security mechanism into sub-kernel-syscall-dispatch, multi-redirect stub"
date: 2026-09-06
arc: arc-vault
commits: ["f17f0e59"]
touched: [sub-kernel-syscall-dispatch]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---
The fifth spawn variant (`SYS_SPAWN_WITH_PERMS` = 31) + the `SPAWN_PERM_*` grant
gate. An I-2-adjacent SECURITY surface, so verified atom-by-atom against the code
before stubbing rather than trusting the distributed consumer-dossier coverage.

WHAT I FOUND (verified against kernel/syscall.c, not assumed):
- The ABI (number, `SPAWN_PERM_*` constants, three-copy pinning) -> covered by
  `sub-kernel-syscall-abi` (owns syscall.h + libthyla-rs), though it names only
  `SYS_POST_SERVICE` (26) as a retired slot; the perm-word ABI rides the general
  spawn-variant coverage.
- The bits' CONSUMERS (`MAY_POST_SERVICE`, `CONSOLE_TRUSTED`, `CONSOLE_OWNER`)
  are named across SEVEN dossiers -- sub-stratum-boot/session, sub-halcyond,
  sub-ptyfs, sub-netd-nic, sub-viv, sub-kernel-devsrv -- but each names the bit
  it consumes, none OWNS the gate.
- `sub-kernel-syscall-dispatch` carried the one-hop delegation SHAPE, but
  explicitly for the sibling I-32 `MAY_RAISE_PAGE_BUDGET` raise authority
  (line 473) -- NOT the `MAY_POST_SERVICE` service-posting gate.

THE FOLD (the load-bearing atom that lived only in the doc):
The grant gate as a SECURITY MECHANISM was unowned. Two soundness atoms:
1. The I-27 reason `CONSOLE_TRUSTED` is console-attach-only + never-delegable --
   a service-poster must not confer the console-trust used for hostowner
   elevation. `spawn_perm_grant_check` (kernel/syscall.c:8428).
2. The SMP race the atomic-stamp-in-thunk closes -- a child scheduled onto
   another CPU between the parent's spawn-return and its next syscall could reach
   `SYS_POST_SERVICE` before a naive mark-after-spawn lands. `apply_spawn_perms`
   (kernel/syscall.c:8470) runs in the child's thread context BEFORE `exec_setup`,
   so no `userland_enter` precedes the stamp. Backstopped by a tail `extinction`
   on any bit outside `SPAWN_PERM_ALL` surviving to the thunk.
Plus the I-2 non-propagation: perm bits are spawn-time `perm_flags`, not
`cap_mask`, and none is `rfork`-propagated, so the fork-grantable ceiling is
untouched (I-2 has no vault note -- prose reference, never guarded-by).

Folded into `sub-kernel-syscall-dispatch` (audit: hard; owns kernel/syscall.c
where both functions live; theme is "where a gate is allowed to live"): a new
Mechanism subsection "The spawn-permission gate is two sites", the I-27 invariant
line extended to name it, and a Prosecution bullet on the non-interchangeable
placements.

NOT REFUTED: the doc's five-variant table and SMP-race narrative are current --
the code matches. The gap was ownership, not correctness. Zero code change.

STUB: multi-redirect across dispatch (mechanism, folded) + syscall-abi (ABI) +
devsrv (what MAY_POST_SERVICE unlocks) + the six consumer dossiers.
