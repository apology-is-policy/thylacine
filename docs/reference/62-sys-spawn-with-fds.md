# 62 — SYS_SPAWN_WITH_FDS: fd inheritance on spawn (P5-stratumd-stub-bringup-b) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-sys-spawn-with-fds-doc-absorb`).
Extends `SYS_SPAWN` with explicit fd inheritance: the caller names a list of fds
(all `KOBJ_SPOOR` at v1.0) installed in the spawned child's handle table at slots
`0..fd_count-1` **before** `exec_setup` runs — the production-shape spawn a
userspace init needs (it cannot reach into a child's handle table otherwise). Its
content lives, code-verified and current, in:

- the **positional fd-inheritance mechanism** — `fd_list[i]` → child fd `i`
  (contiguous), the KOBJ_SPOOR-only v1.0 restriction, and the transfer-not-bump
  refcount discipline:

      vault/system/boundary/pouch-seam/sub-pouch-process.md

- the **kernel spawn handler + the install-before-exec_setup ordering**:

      vault/system/kernel/execution/sub-kernel-exec.md
      vault/system/kernel/entry/sub-kernel-syscall-dispatch.md

The `/stub-driver` production-shape orchestrator (spawn stub + attach + mount +
unmount + reap, all from EL0) is a test-scaffold binary — the P5 stub-bringup arc,
absorbed at `docs/reference/61-stratumd-stub` (the mechanisms it drives are the
surfaces above).

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Nothing load-bearing — a clean zero-fold.** The positional fd-inheritance
  mechanism and its refcount discipline are `sub-pouch-process`'s; the kernel
  handler is the exec/dispatch dossiers'; the orchestrator is test scaffold.
