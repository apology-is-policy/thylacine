---
id: chg-2026-09-06-asid-absorb
type: chg
title: "docs/reference retirement: absorb 22-asid into sub-kernel-asid -- fold the missing no-per-Proc-free teardown-TLB-safety atom first, then stub-and-redirect (48 absorbed / 109 live)"
date: 2026-09-06
arc: arc-vault
commits: ["c637e9a8"]
touched: [sub-kernel-asid]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The second file absorbed under the retirement routing flip
([[chg-2026-09-06-docs-reference-retirement-flip]]; 03-mmu was first). The
memory area's rolling-ASID surface: `docs/reference/22-asid.md` -> a stub
redirecting to [[sub-kernel-asid]].

THE DISCIPLINE EARNED ITS KEEP ON THE FIRST FILE. A "COVERED" verdict is a
judgment, and stubbing on it blind loses whatever the dossier does not carry. I
read both documents and found a real gap: 22-asid documents the **no-per-Proc
asid_free teardown** and why it is TLB-safe (vma_drain's all-ASID broadcast ran
before the page table was destroyed; no live CPU holds a dead Proc's TTBR0; any
reuse is gated by the rollover's per-CPU flush_pending) -- an I-31-supporting
soundness argument that sub-kernel-asid did NOT hold. A careless stub would have
dropped it into git history and asserted it absorbed (the batch-23/28 class:
34-devramfs's cpio, 01-boot's uart).

So: FOLD then stub, in one commit.
- Folded a "Teardown: there is no per-Proc free" subsection into
  sub-kernel-asid's Mechanism (the three conjuncts that must all hold, linked to
  [[sub-kernel-vma]] and [[inv-i31]]); updated: 2026-08-02 -> 2026-09-06.
- Stubbed 22-asid with the redirect + an honest "what it got wrong": the stale
  `sizeof(struct Proc)` = 264 (it is 392 and holds no page table since LINEAGE
  -- owned by sub-kernel-proc); the frozen Status test-count/hashes; the
  register-bit/constant tables that live in arch/arm64/asid.h (the
  duplication-rot the retirement exists to end).

No code touched -- this is documentation curation of already-audited RW-1
behavior, so no audit round is owed; the fold is additive prose describing
existing code. view-absorption re-rendered (47 -> 48 absorbed). The remaining
memory-area single-owner folds (20-burrow, 26-vma, 25-fault-dispatcher,
146-addrspace) are the natural next batch; the multi-dossier sys-spawn family is
deferred (its syscall-ABI half is sub-kernel-syscall-dispatch's, not
sub-kernel-proc's -- a two-owner surface that needs the careful split).
