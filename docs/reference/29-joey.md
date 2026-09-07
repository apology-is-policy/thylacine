# 29 — /init bringup (P3-F) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-joey-docs-absorb`).
This is the **P3-F milestone** record of the first userspace process, and it is
**almost entirely superseded** — it describes a 9-instruction hand-encoded
AArch64 blob that printed `hello\n` and exited, synthesised into an ELF in an
8 KiB `.bss` array (`g_joey_elf_blob`) at boot. The current joey is a real
binary loaded from the initrd, rforked with `CAP_ALL` as the capability-delegate
root of a long-running init that builds the boot namespace and hands off to a
userspace supervisor. What exists now lives in:

- the **kernel-side kproc** — `joey_run`, the boot namespace it grafts, the
  `#85` exec-window init-blob transient (which replaced the BSS array this file
  describes), the trust-root stamps, and the wait-by-pid:

      vault/system/kernel/boot/sub-kernel-joey.md

- the **userspace supervisor** — `usr/joey/joey.c`, the long-running init that
  pivots root, brings up stratumd and corvus, and runs the getty loop:

      vault/system/stratum/sub-stratum-boot.md

**What this file got WRONG or MISSED by the time it was absorbed:** essentially
its whole mechanism is P3-F-era and gone.

- The **9-instruction hand-encoded blob** and its `build_init_elf()` synthetic
  ELF wrapper were removed at P5 (`59-joey-from-ramfs`); joey is now a compiled
  `usr/joey/` binary shipped in the cpio initrd and loaded by `devramfs_lookup`.
- The **8 KiB `g_joey_elf_blob` BSS array** is gone: since `#85` the init blob is
  a transient exact-size `kmalloc` freed by the child the moment `exec_setup`
  returns (the BSS array had grown to 640 KiB of permanently-resident kernel
  memory before it was retired). `sub-kernel-joey` documents the transient.
- "**Prints hello and exits**" is gone: joey is the long-running init (it does
  not exit in normal operation), rforked with `CAP_ALL` as the I-2 delegate root.
- The **~200 µs /init-phase cost** and **PID-not-1** notes are P3-F snapshots.
- The **`#157` second-userspace-iteration hang** was closed at P4-Fix157 (the
  `userland_enter` SPSel discipline); it is not a live joey caveat.
- The **naming rationale** (`joey` / `joey_run` / `joey_thunk`) carried forward,
  but the vault dossiers describe the as-built mechanism, not the milestone that
  named it.
