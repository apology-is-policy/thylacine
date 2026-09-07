# 59 — /joey loaded from the initrd (P5-joey-from-ramfs) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-joey-docs-absorb`).
This is the P5 step that replaced the P3-F kernel-embedded blob
(`29-joey`) with a real `usr/joey/` binary shipped in the cpio initrd and loaded
by `devramfs_lookup`. Its **`#85` exec-window-transient content is current** and
is carried, verbatim in substance, by the vault. Its subject spans:

- the **kernel-side load + orchestration** — `joey_run`'s `devramfs_lookup` of
  `/joey`, the mandatory 8-aligned copy (cpio newc is 4-aligned; `elf_load`
  casts an `Ehdr` and requires 8), the `#85` transient exact-size `kmalloc`
  freed by the child at the exec window, the `KP_ZERO` tail, the boot-log
  `released` line, and the boot-fatal failure paths:

      vault/system/kernel/boot/sub-kernel-joey.md

- the **userspace supervisor** `/joey` grew into — everything this file lists as
  DEFERRED (fork stratumd-system, attach the pool's 9P tree, `pivot_root`, start
  corvus + login): now built, in

      vault/system/stratum/sub-stratum-boot.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- Its `usr/joey/joey.c` body ("prints a banner via `t_putstr` and exits 0") is
  the P5 minimum-viable snapshot; `/joey` is now the long-running supervisor
  (`sub-stratum-boot`), and every row in its own Status table marked DEFERRED
  (supervisor / stratumd fork / `/sysroot` 9P mount / `pivot_root`) has since
  landed.
- **Caveat 3 contradicts this file's own body.** It states
  `JOEY_BLOB_MAX = 32 KiB`, but the body already records that `#85` retired the
  static `JOEY_BLOB_MAX` array for a transient exact-size heap buffer — the
  caveat is a leftover the `#85` edit did not scrub. There is no `JOEY_BLOB_MAX`
  now; the bound is `EXEC_FILE_MAX`.
- The **416/416 test count** is a P5 snapshot, long superseded.
- The **"pre-existing flaky EL1 extinction on a secondary CPU"** caveat is a P5
  boot-note, not a joey property; the SMP-soundness story is the gate's, not
  this file's.
