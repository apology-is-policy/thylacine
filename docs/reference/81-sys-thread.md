# 81 — `SYS_THREAD_SPAWN` / `SYS_THREAD_EXIT`: multi-thread Procs [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-sys-thread-doc-absorb`).
The kernel-side pthread substrate: `SYS_THREAD_SPAWN` (41) creates a peer Thread in
the calling Proc that erets to EL0 at a caller entry on a caller stack;
`SYS_THREAD_EXIT` (42) terminates the calling Thread, atomically clearing a
registered `clear_child_tid` word and torpor-waking joiners; `SYS_SET_TID_ADDRESS`
(36) stores that tidptr. An audit-trigger surface; its content is carried by:

- the **peer-Thread creation** — `thread_create_user` (the EL0 creation shape, its
  own 32 KiB kstack, deliberately not validating the four user-VAs), and the
  atomic tid allocation that the pre-P6 non-atomic `++` could double-hand under
  concurrent spawns:

      vault/system/kernel/execution/sub-kernel-thread.md

- the **EL0 entry trampoline** — `thread_user_trampoline` (`context.S`), which
  reaches EL0 under the #713 mask across its `eret` window:

      vault/system/kernel/scheduling/sub-kernel-sched-smp.md

- **thread exit, the pthread-join wakeup, and cross-thread shootdown** —
  `thread_exit_self` (the last one out zombies the Proc through the ZOMBIE
  chokepoint), the `thread_clear_child_tid_handoff` (a peer's exit zeroes its
  `clear_child_tid` and `torpor_wake`s joiners — the kernel half of
  `pthread_join`, folded here at absorption), and `SYS_EXIT_GROUP` (60):

      vault/system/kernel/execution/sub-kernel-death.md

- the **futex the join parks on** — `torpor_wait`/`torpor_wake` (the `(Proc, VA)`
  bucket, `SYS_TORPOR_WAKE` = 40):

      vault/system/kernel/ipc-wake/sub-kernel-torpor.md

- the **user-VA store primitive** — `uaccess_store_u32` (store-a-word with its
  fixup, so a bad tidptr faults out instead of extincting):

      vault/system/kernel/entry/sub-kernel-uaccess.md

- the **syscall numbers and handlers** — 41 / 42 / 36 in the number space and its
  dispatch:

      vault/system/kernel/entry/sub-kernel-syscall-abi.md
      vault/system/kernel/entry/sub-kernel-syscall-dispatch.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **The kernel-side `clear_child_tid` join handshake had no kernel dossier home**
  until this absorption — only the pouch userspace side (`sub-pouch-thread`)
  documented it. The kernel primitive (atomic zero + `torpor_wake` on exit, the bad
  tidptr skipped silently) is now folded into `sub-kernel-death`.
- **The content is distributed** across the seven dossiers above; this P6-sub-9a
  doc is the composite view, and the pouch mutex/condvar/rwlock layer it points
  forward to (sub-chunk 9b) is built.
