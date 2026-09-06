---
id: chg-2026-09-06-allowance-doc-absorb
type: chg
title: "absorb docs/reference/117-allowance (I-34 hardware allowance): zero-fold; ref doc stale on the audit-F1 install-under-lock UAF"
date: 2026-09-06
arc: arc-vault
commits: ["PENDING"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---

# docs/reference/117-allowance.md -> ABSORBED (I-34 audit-trigger surface)

Absorbed the 804-line I-34 reference doc into a multi-redirect stub. The owning
dossier `sub-kernel-allowance` (188 lines, updated 2026-08-02) is CURRENT and
covers every load-bearing atom, verified atom-by-atom against the ref doc:

- the NULL-pointer broad/narrowed hinge; the two-step create gate + the
  revoke-vs-create SMP race close; confer (set-once) / revoke (#160 fold) / clone
  (born-revoked) / free; `allowance_confer_within_parent` (I-2 narrowing);
  the drivers-are-leaves rfork refusal; the un-widenable window arithmetic; the
  four legs of I-34; the PCI fourth-door TOCTOU (resolve the boot-immutable table
  the claim walks). All present.

The dossier is not merely current -- it is AHEAD of the ref doc on the one point
they diverge, which is the "what it got wrong" headline:

- **The ref doc's set-once caveat is pre-audit-F1.** It claims the confer
  `kfree(old)` is lock-free and "sound only because the Proc has not entered EL0,
  so nothing reads p->allowance concurrently." Code-grounded FALSE:
  `kernel/allowance.c:66-77` (`proc_allowance_install_locked`, "audit F1") runs
  the swap UNDER `g_proc_table_lock` precisely because the child IS reachable by
  a concurrent `proc_group_terminate -> proc_revoke_allowance` on the
  inherited-clone `old` allowance (independent of EL0 entry) -- the lockless swap
  was a real UAF on the narrowed-parent-spawns-child path. The dossier documents
  the corrected mechanism (Mechanism + Prosecution: "The install must stay under
  g_proc_table_lock; the lockless swap was a real UAF"). The ref doc has F4 (the
  RELEASE/ACQUIRE visibility edge) but lacks F1.

Zero fold -- nothing to add to a dossier already ahead of the doc.

Redirects: the mechanism -> sub-kernel-allowance; the MMIO/IRQ/DMA constructors +
the PCI fourth door (kobj_pci_claim + kobj_pci_resolve_bdf) -> sub-kernel-hwcap;
the leaf gate + #160 revoke-fold + rfork inherit/reap -> sub-kernel-proc /
sub-kernel-death; the confer-at-spawn ABI + the warden grant -> sub-libdriver-grant
/ sub-kernel-exec; the model -> specs/allowance.tla.

87 -> 88 absorbed of 157. First of the big audit-trigger references. lint 0-fail.
