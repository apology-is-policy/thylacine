# 147 — `SYS_EXECVE` (LINEAGE L-2a) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-execve-doc-absorb`).
The image-replacement syscall — the first (and only) path that changes a **live**
Proc's address space in place, LINEAGE L-2a + L-6a, enforcing I-44. A good as-built
reference; its content is now spread across the dossiers that own each half, which
carry it more completely (the L-6a execve-core front-end story was folded into
`sub-kernel-exec` at absorption). It is carried by:

- the **exec core** — the copy-from-old / resolve / build-DETACHED / commit /
  rewrite-trapframe ordering (nothing observable fails, so a bad ELF or OOM leaves
  nothing to undo), the `exec_load_into(as, exempt, ...)` detached-target loader
  and the `_in` VMA forms, the phenotype threaded as a `pheno` parameter (I-43),
  the System V startup frame (argv/envp/auxv, the two-axis envp bound), and **the
  L-6a two front ends** (`sys_execve_core` + native/`viv_execve`, the
  caller-owns-the-blob double-free lesson, the two-pass I-30 argv bound, and the
  envp #140 decline-as-detector):

      vault/system/kernel/execution/sub-kernel-exec.md

- the **live address-space swap** — `sched_activate_addrspace`, the one place
  TTBR0 moves outside a context switch, and why the compose-and-install pair runs
  IRQ-masked (the ASID resolver's contract + the TTBR0-readback in
  `cpu_switch_context`) with an `isb` barrier:

      vault/system/kernel/scheduling/sub-kernel-sched.md

- the **infallible commit** — `proc_exec_replace` (swap + activate is
  by-construction infallible, which is what makes "a failed execve returns with
  the caller intact" hold) and the signal-state reset leg:

      vault/system/kernel/execution/sub-kernel-proc.md

- the **old address space's teardown** — `addrspace_unref` (drains the VMA list,
  no TLB maintenance) and the I-32 counter arithmetic:

      vault/system/kernel/memory/sub-kernel-addrspace.md

- **why the teardown needs no TLB flush** — the ASID tag: every user PTE is
  non-global, and the rolling allocator's per-CPU `flush_pending` local flush runs
  before an ASID value can go live again (I-31):

      vault/system/kernel/memory/sub-kernel-asid.md

- the **handle table kept across exec, minus close-on-exec** — `O_CLOEXEC` /
  `handle_close_on_exec` and the cloexec bitmap (#151):

      vault/system/kernel/security/sub-kernel-handle.md

- the **phenotype-shaped signal reset** — the `sigtab` reset-in-place (#254: a
  cross-Proc lockless reader made a free here a UAF), `PHENO_LINUX` keeping SIG_IGN
  rows while clearing caught ones, and the note-mask inheritance rule:

      vault/system/kernel/entry/sub-kernel-vivarium.md

**What this file got WRONG or MISSED by the time it was absorbed** — little; it is
an as-built L-2a/L-6a doc and largely current. The distribution is the point:

- Its content is now **spread across the eight owning dossiers above**, each of
  which carries its half at more depth (the ASID-tag soundness, the
  `sched_activate_addrspace` two-reason IRQ mask, the cloexec bitmap layout).
- The **L-6a execve-core details** (the two front ends, the blob-ownership
  double-free, the two-pass I-30 bound, the envp #140 asymmetry) lived only here
  until absorption folded them into `sub-kernel-exec`.
- The **multi-thread refusal** (`-EAGAIN`, because the sole terminate primitive
  sets a never-cleared `group_exit_msg` and exempting the execer would break a
  later kill) and the **`ENOEXEC`-reported-as-`EINVAL` gap** (ABI-signoff-gated in
  ERRORS.md) are standing design stances, not defects — carried by the dossiers.
