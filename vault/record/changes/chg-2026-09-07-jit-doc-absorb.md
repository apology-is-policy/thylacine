---
id: chg-2026-09-07-jit-doc-absorb
type: chg
title: "absorb docs/reference/145-jit (the JIT capability, I-42): fold the I-cache contract (CL-7k-3 F1 create-invalidate + the cross-PE ISB publish contract) into sub-kernel-mmu"
date: 2026-09-07
arc: arc-vault
commits: ["8efdea10"]
touched: [sub-kernel-mmu]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-07
---
The W^X-preserving JIT (I-42, CL-7k, audit-trigger surface). JIT is DISTRIBUTED --
no dedicated dossier; inv-i42 has no note (I-42 written plain). quaestor owner:
jit.rs -> sub-libthyla-rs; test_jit.c -> UNOWNED. Verified atom-by-atom.

ALREADY COVERED (verified): the dual-map code Burrow (BURROW_TYPE_CODE admissibility
minted-at-create-not-flagged, the dual refcount, one-syscall-both-aliases,
destroy-by-writer-VA, W^X-clean PTEs) -> sub-kernel-burrow + inv-i12; CAP_JIT
elevation-only/non-heritable + the scripture-wording note ("CAP_HW_CREATE class"
read-literally-WRONG since that cap is fork-grantable) -> abi-caps + sub-kernel-caps;
the clearance walk (corvus jit level, SDL-walks-on-behalf bearer-vs-SELF) ->
sub-corvus; CodeRegion -> sub-libthyla-rs; the fault-path CODE=ANON arm ->
sub-kernel-fault.

THE FOLD (genuine gap -> sub-kernel-mmu, depth rich; updated 09-06 -> 09-07):
- The I-CACHE CONTRACT was homeless (grep recycled-icache / IVAU / ISB-peer /
  arch_icache_sync_range-cross-PE -> 0 JIT hits; sub-kernel-mmu owned the primitive
  arch_icache_sync_range but named I-42 only in Provenance). Folded as a proper
  I-42 Invariants entry, BOTH atoms: (a) CL-7k-3 F1 -- a recycled code page carries
  stale I-cache lines (zeroing decodes UDF#0 but doesn't touch I-cache; free path
  does no cache maint) -> sys_jit_create_region invalidates before any RX PTE names
  the pages (the code Burrow was the sole executable backing that skipped it); (b)
  the cross-PE ISB contract -- IC IVAU is IS-broadcast but the trailing ISB retires
  prefetch on the CALLING PE only, so a peer must take a context-sync event before
  branching in (the ORC DualMapMemoryMapper emit-on-one/execute-on-worker case);
  the maintenance runs on the DIRECT MAP (fault-safe + PIPT-exact), never the user VA.

EXTERNAL (named, not folded): the section-8.1 ORC mapper defects (writerFor
assert-compiled-out + upper_bound()-1 containment miss; initialize() phantom-
reservation insert -- the ordered-container-changed-the-cost-of-a-spurious-key
lesson) live in the vendored LLVM/Mesa fork, not the tree. test_jit.c stays UNOWNED
(pre-existing; JIT distributed, no single primary owner -- light follow-up).
Redirect stub. Zero code change.
