# 22 — ASID allocator [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-asid-absorb`; the
memory area's rolling-ASID surface, under the routing flip). Its content now
lives, code-verified and current, in the dossier:

    vault/system/kernel/memory/sub-kernel-asid.md

(the rolling generation-rollover allocator that replaced the per-Proc-permanent
design's unprivileged exhaustion DoS, the two-guard lockless fast path, the
rollover-versus-switch race and its single-location `g_active_asids[cpu]`
interlock, `new_context`/`flush_context` and the NOSTEAL reservation, the
full-`context_id` ownership compare in `check_update_reserved` [the audit-F1
alias], the no-per-Proc-free teardown and why it is TLB-safe, the leaf lock
order, and I-31. The five buggy-cfg counterexamples live in the spec note
[[spec-asid]].)

**What this file got WRONG or MISSED by the time it was absorbed** (the reason
the dossiers are written from the code):

- Its context-switch-wiring paragraph asserts `sizeof(struct Proc)` "unchanged
  at 264". The Proc is **392 bytes and holds no page table at all** since the
  LINEAGE address-space extraction — it holds a pointer to a refcounted
  `AddrSpace`, which is what makes the vfork and copy-on-write shapes
  expressible. The dossier for it (`vault/system/kernel/execution/sub-kernel-proc.md`)
  records the real size and its history.
- Its **Status** section freezes a test count ("806/806 PASS") and the RW-1
  landing hashes as the whole story; the dossier's Provenance carries the
  current lineage via chg notes instead, and the suite has grown well past that
  figure.
- The exact register-bit and constant tables it reproduces
  (`ID_AA64MMFR0_EL1.ASIDBits` bit positions, `ASID_TTBR0_SHIFT`, the
  `context_id` value/generation field split) live in `arch/arm64/asid.h` — the
  source of truth — which the dossier points at rather than duplicating (a
  duplicated constant is a constant that rots, which is why docs/reference is
  being retired).
