---
id: chg-2026-09-06-hardening-doc-absorb
type: chg
title: "absorb docs/reference/12-hardening (P1-H): fold the unowned stack canary into sub-kernel-boot-sequence"
date: 2026-09-06
arc: arc-vault
commits: ["PENDING"]
touched: [sub-kernel-boot-sequence]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---

# docs/reference/12-hardening.md -> ABSORBED (P1-H hardening posture)

Absorbed the 399-line hardening reference doc into a multi-redirect stub. Every
hardening surface had a dossier EXCEPT the stack canary:

- hwfeat detection + the banner hardening lines -> sub-kernel-boot-sequence
- PAC keys + BTI + SCTLR enable (start.S) -> sub-kernel-boot-entry
- W^X + PTE_GP (mmu.{h,c}) -> sub-kernel-mmu
- LSE alternatives-patcher -> sub-kernel-alternatives
- KASLR (I-16) -> sub-kernel-kaslr
- the banner lines -> abi-boot-banner

ONE genuine gap, folded:

- **kernel/canary.c was UNOWNED and undescribed** (verified: quaestor owner ->
  UNOWNED; not in boot-sequence/boot-entry -- the boot-entry "guard page" hits are
  the boot-STACK guard, not the cookie). The mechanism (canary.c, 111 lines):
  __stack_chk_guard starts at a link-time magic so pre-canary_init frames validate
  consistently; canary_init(seed) overwrites it with a KASLR-seeded runtime cookie
  under a barrier so every frame sees exactly one cookie for its lifetime;
  __stack_chk_fail -> extinction; one of the seven test-fault.sh provokers. Folded
  into sub-kernel-boot-sequence (the sibling boot-time hardening-init + banner
  owner), which now claims canary.c + canary.h in its code: list.

96 -> 97 absorbed of 157. lint 0-fail.
