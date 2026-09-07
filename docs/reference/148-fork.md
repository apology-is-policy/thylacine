# 148 — `SYS_RFORK` + child-context restoration (LINEAGE L-3b) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-fork-doc-absorb`). The
process-creation family: `rfork(RFPROC|RFMEM)` (the child that shares the address
space and resumes the parent's frame, L-3b), descriptor inheritance (L-3c-1), the
vfork suspend (L-3c-2), and stock COW `fork()` (L-5) — the I-44 arc. A rich
as-built doc; its content is now spread across the dossiers that own each half,
which carry it more completely (the #137 WnR-decode lesson was folded into
`sub-kernel-fault` at absorption). It is carried by:

- the **rfork mechanics and the vfork suspend** — `rfork_internal` (the one body
  with its `fork_context` arm and the three address-space answers: empty for a
  plain spawn, shared for `RFMEM`, an `addrspace_clone` for a fork), what the
  child inherits (identity, caps minus `CAP_ELEVATION_ONLY`, phenotype, allowance,
  the Territory clone), and the vfork suspend (`vfork_await_release`, the
  release-*is*-the-release predicate `state != ALIVE || as != parent->as`, the
  ABA-safety from the parent's held reference, and the capture-`pid`-before-`ready`
  UAF the park aligned):

      vault/system/kernel/execution/sub-kernel-proc.md

- the **frame construction** — `fork_frame_init` (the pure two-edit decision:
  `regs[0] = 0` and `sp = child_sp`, everything else verbatim) and
  `thread_create_forked` (the third creation shape, the frame carved at the child
  kstack top, FP from the *live* registers):

      vault/system/kernel/execution/sub-kernel-thread.md

- the **fork trampoline** — `thread_fork_trampoline` in `vectors.S`, which reaches
  EL0 by branching into the shared exception return rather than a fourth `eret`
  (the #811 die-check, the #713 mask, and no GPR sweep because the child continues
  the parent's userspace frame):

      vault/system/kernel/entry/sub-kernel-exception.md

- the **`SYS_RFORK = 102` handler** — the flags/`child_sp`/`child_tls` validation
  and the `0`-means-inherit resolutions done in the handler layer:

      vault/system/kernel/entry/sub-kernel-syscall-dispatch.md

- **descriptor inheritance (L-3c-1)** — `handle_table_copy_into` (fork-shape only,
  indices preserved so a skipped slot leaves a hole), `handle_slot_may_alias` (the
  kind clause = I-5, the object clause excluding a devsrv Spoor), rights verbatim
  (I-6), and why the fork proceeds with a hole rather than refusing:

      vault/system/kernel/security/sub-kernel-handle.md

- **the COW clone (L-5)** — `addrspace_clone`, the #136 per-kind decision where
  writability decides (`FILE` and read-only eager-`ANON` shared, writable `ANON`
  refused, MMIO/DMA refused at any prot), and the I-44 break machinery:

      vault/system/kernel/memory/sub-kernel-addrspace.md

- the **`CAP_ELEVATION_ONLY` strip** on the forked child (I-2):

      vault/system/kernel/security/sub-kernel-caps.md

- the **#137 WnR-decode bug + its test tautology** — `is_write` was decoded from
  the wrong `ISS` bit, unreachable until the COW break needed it, hidden by a
  unit test that mirrored the constant (folded here at absorption):

      vault/system/kernel/memory/sub-kernel-fault.md

**What this file got WRONG or MISSED by the time it was absorbed** — little; it is
an as-built L-3b..L-5 doc and largely current (it even self-corrects its own
`CLONE_VM`-without-`CLONE_FILES` misdescription). The change is distribution:

- Its content is now **spread across the nine owning dossiers above**, each
  carrying its half at more depth.
- The **#137 finding** (the always-false `is_write`, the hang-not-error symptom,
  and the constant-mirroring test that agreed with the code instead of the
  hardware) lived only here as a fork-story aside; it is a general fault-decode
  lesson, now folded into `sub-kernel-fault`.
- The audit findings it uniquely narrates — #136 (the clone that refused every
  real address space because the vDSO clock page is eager-anon in every Proc), the
  vfork ABA, and "a new park inherits every unsynchronised access that follows
  it" — are carried by their owners (`sub-kernel-addrspace`, `sub-kernel-proc`).
