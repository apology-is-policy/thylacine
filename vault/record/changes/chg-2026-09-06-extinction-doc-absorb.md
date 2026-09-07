---
id: chg-2026-09-06-extinction-doc-absorb
type: chg
title: "absorb docs/reference/04-extinction (kernel ELE): fold the unowned extinction.c into sub-kernel-halls"
date: 2026-09-06
arc: arc-vault
commits: ["8cade34d"]
touched: [sub-kernel-halls]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---

# docs/reference/04-extinction.md -> ABSORBED (kernel ELE)

Absorbed the 342-line extinction reference doc into a multi-redirect stub. ONE
genuine gap, folded:

- **kernel/extinction.c was UNOWNED** (quaestor owner -> UNOWNED). sub-kernel-halls
  described the crash dump and referenced the extinction path but its code: claimed
  only halls.c. Folded the extinction() entry (extinction.c, 229 lines): the ELE
  entry + extinction_with_addr, the EXTINCTION: tooling-ABI marker (matched
  literally -> a torn prefix is a false-negative boot-failure), ASSERT_OR_DIE, the
  recursive-extinction suppression (the second entry parks rather than
  stack-overflowing), the _torpor halt, and the owed-IPI_HALT line-tearing seam
  (#243, a known-open SMP tearing window -- the crash emitter serializes its own
  output under the console ring lock but peers are not halted before the print).
  sub-kernel-halls now claims extinction.c + extinction.h.

Redirects: the crash-emitter console serialization -> sub-kernel-cons; the
EXTINCTION: ABI -> abi-boot-banner.

98 -> 99 absorbed of 157. lint 0-fail.
