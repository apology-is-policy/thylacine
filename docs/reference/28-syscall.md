# 28 — userspace syscall dispatch (P3-Ec) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-syscall-docs-absorb`).
This is the **P3-Ec milestone** record — the absolute-minimum syscall surface
(`SYS_EXITS = 0`, `SYS_PUTS = 1`, nothing else) that first proved an EL0 thread
could trap into the kernel and be answered. The dispatcher grew into the full
Thylacine ABI (~107 live syscalls); what exists now lives in:

- the **dispatcher itself** — the `x8` number / `x0..x5` args / `x0` return
  convention, the unknown-number path, the SVC entry, and the two-tier staging
  (the `SYS_RW_STACK` stack scratch + the heap tier) with the copy-before-role
  discipline that `SYS_PUTS` established:

      vault/system/kernel/entry/sub-kernel-syscall-dispatch.md

- **`SYS_EXITS`** → `exits()` (ZOMBIE, wake the parent, `sched()` away):

      vault/system/kernel/execution/sub-kernel-proc.md

- **`SYS_PUTS`** → the shared console write path (`cons_output_write`: the writer
  role, the ring, ONLCR, and the `#76` short-count cut on a stalled-consumer
  deadline or a death):

      vault/system/kernel/console-gfx/sub-kernel-cons.md

The uaccess fault mechanism this file describes (copy-in, demand-page-on-fault,
the fixup label) is `40-uaccess.md`'s subject and is not yet absorbed.

**What this file got WRONG or MISSED by the time it was absorbed:** it is a
P3-Ec snapshot, and most of its caveats are now false.

- The **two-syscall enum** (`SYS_EXITS = 0` / `SYS_PUTS = 1`) is a placeholder;
  the ABI froze at Phase 5 into ~107 numbered syscalls (`sub-kernel-syscall-dispatch`
  carries the census). `SYS_EXITS`/`SYS_PUTS` were renumbered.
- Caveat 1, "**No userspace pointer validation at v1.0**" (a bad VA extincts the
  kernel), is **false**: the staging path validates the user VA (kernel-half
  reject, `USER_VA_TOP` bound) and recovers from a fault via
  `userland_demand_page` + the primitive's fixup label, returning a whole-op
  `-1` (EFAULT) rather than extincting. The body already records this (the `#76`
  + R7/R12 updates); caveat 1 is the stale header it contradicts.
- "**Stubbed: copy_from_user / copy_to_user with fault-recovery**" is built (the
  uaccess primitives + the staging tiers).
- The **`imm16`-unused** and **"syscall numbers unstable until Phase 5+"** notes
  are stale — the ABI is frozen, and the phenotype dispatch (not `imm16`) is how
  a Linux-shaped call is distinguished (`sub-kernel-syscall-dispatch`'s phenotype
  prologue).
- `SYS_EXITS`'s **binary `0`/`1` exit-status collapse** was a P3-Ec limitation;
  the current exit-status semantics are `sub-kernel-proc`'s to state.
