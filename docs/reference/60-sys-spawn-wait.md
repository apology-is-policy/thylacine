# 60 — SYS_SPAWN + SYS_WAIT_PID (P5-spawn-wait) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-spawn-wait-doc-absorb`).
The P5 minimal orchestration primitive: `SYS_SPAWN` (21, combined `rfork(RFPROC)`
+ exec of a boot-initrd binary) and `SYS_WAIT_PID` (22, reap a ZOMBIE child) — the
two syscalls that turned `/joey` from "prints and exits" into a real orchestrator.
Its content lives, code-verified and current, in:

- the **spawn machinery + the blob-lifetime discipline** — the `exec_setup` blob
  path (the boot path `joey.c` loads init through), `exec_setup_from_spoor` (every
  `SYS_SPAWN_*`), and "the blob that belongs to the caller" (L-6a: the kmalloc'd
  ELF copy that must live from rfork through the child thunk's `exec_setup`):

      vault/system/kernel/execution/sub-kernel-exec.md   (audit: hard)

- the **wait/reap** — `wait_pid_for(want_pid, flags, status_out)`, the successor
  that added the pid/pgrp selectors + `WNOHANG` (and PTY-1e report-not-reap) the
  v1.0 doc deferred, plus the reap-any-hazard filter:

      vault/system/kernel/execution/sub-kernel-proc.md   (audit: hard)

- the **8-aligned ELF copy** (cpio newc pads to 4, `elf_load` needs 8):

      vault/system/kernel/execution/sub-kernel-elf.md

- the **handlers + ABI** — the thin wrappers, the name/blob bounds:

      vault/system/kernel/entry/sub-kernel-syscall-dispatch.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Nothing load-bearing — a clean zero-fold of a superseded first-cut doc.**
  Every one of its five "deferred" caveats has since landed and is carried by a
  richer successor: real `SYS_RFORK` with COW (I-44, the LINEAGE arc /
  `sub-kernel-burrow` COW break); `SYS_SPAWN_WITH_CAPS` and the whole spawn-variant
  family (`sub-kernel-exec` + `sub-kernel-caps`, absorbed at docs 62/63/64/73);
  argv via `SYS_SPAWN_FULL_ARGV` (`sub-kernel-exec`); the `SYS_WAIT_PID` PID
  selector + non-blocking via `wait_pid_for` (`sub-kernel-proc`). The
  `uaccess_store_u8`-per-byte status write and the partial-fault hazard are the
  byte-I/O surface's (`sub-kernel-uaccess`).
- **The combined-fork+exec rationale is history worth keeping** — at v1.0 there
  was no COW `rfork` returning 0-in-child, so spawn was the Plan 9
  `rfork(RFPROC|RFEXEC)` idiom; that reasoning is carried forward in
  `sub-kernel-exec` and the LINEAGE dossiers, where the real COW `rfork` now lives.
