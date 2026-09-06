# 57 — /attach-probe — userspace integration test of the Phase 5 mount surface [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-attach-probe-doc-absorb`).
Like `90-u-test`, this documents a **test binary**, not a subsystem: `/attach-probe`
drives `SYS_ATTACH_9P` + `SYS_MOUNT` + `SYS_UNMOUNT` end-to-end from a real EL0 Proc
against a kernel-thread 9P responder — the userspace companion to
`test_sys_mount.c`'s kernel-internal coverage, exercising the actual SVC dispatch
and the wire codec over real pipe Spoors. A test probe has no dossier owner, so the
redirect is to the surfaces it exercises, and the record is the test itself:

- the **mount / attach syscall surface it drives** — the `SYS_ATTACH_9P` /
  `SYS_MOUNT` / `SYS_UNMOUNT` handlers and their validation:

      vault/system/kernel/entry/sub-kernel-syscall-dispatch.md

- the **9P attach mechanism** (`SYS_ATTACH_9P`, the per-connection fid namespace,
  the kernel-stamped `SO_PEERCRED` identity):

      vault/system/kernel/ninep/sub-kernel-ninep-attach.md

- the **territory mount composition** (`domount`/`cross_mounts`, the mount DAG,
  I-1/I-3):

      vault/system/kernel/namespace/sub-kernel-territory.md

- the **9P transport** the probe composes over pipe Spoors:

      vault/system/kernel/ninep/sub-kernel-ninep-transport.md

- the **test's own record** — the binary (`usr/attach-probe/`), its boot orchestration
  in joey, and the phase-5 status row — not a dossier.

**What this file got WRONG or MISSED by the time it was absorbed:** it is an accurate
P5-attach-probe-era description of a live integration probe; nothing is stale. The
only change is that a test-binary reference has no dossier owner — the mount/attach
surface it validates is documented at its kernel homes above, and the E2E probe is a
boot witness rather than reference prose.
