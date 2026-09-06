# 90 — /u-test (libthyla-rs uplift integration smoke) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-u-test-doc-absorb`).
This documents a **test binary**, not a subsystem: `/u-test` is the boot-time
integration smoke for the libthyla-rs uplift (U-2 arc), six composed cross-module
flows that joey spawns after `/alloc-smoke` and treats as a boot gate. There is no
subsystem dossier for a test probe, so the redirect is to the surfaces it exercises
and the record is the test itself:

- the **libthyla-rs surface it composes** — `t::alloc` / `handle` / `fs` / `io` /
  `process` / `notes` / `poll` / `thread` / `torpor` / `ninep` / `hardware` — is the
  userspace face of the kernel syscalls, whose ABI mirrors are owned by:

      vault/system/kernel/entry/sub-kernel-syscall-abi.md

- and each flow's mechanism lives in the kernel dossier for the surface it drives —
  pipes ([[sub-kernel-pipe]]), notes ([[sub-kernel-notes]]), poll
  ([[sub-kernel-poll]]), the thread/torpor join ([[sub-kernel-thread]] +
  [[sub-kernel-torpor]], and the `clear_child_tid` handshake in
  [[sub-kernel-death]]), the 9P codec ([[sub-kernel-ninep-wire]]), and the
  hardware-cap negative paths ([[sub-kernel-allowance]]).

- the **test's own record** is the binary (`usr/u-test/src/main.rs`), its boot-log
  signature, and the phase-7 status row — not a dossier.

**What this file got WRONG or MISSED by the time it was absorbed:** it is an
accurate U-2-test-era description of a still-live boot probe; nothing is stale. The
change is only that a test-binary reference has no dossier owner — the composed
surfaces it validates are documented at their kernel homes above, and the six-flow
boot gate is exercised on every boot rather than described anywhere else.
