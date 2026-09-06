# 98 — Capability-scoped service storage + FS-delta (O_PATH) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-capstorage-doc-absorb`).
A system service reaches its persistent storage through a handed storage-root
capability (a `KObj_Spoor` for its subtree, endowed at spawn like fd 0/1/2), its FS
authority bounded by that capability (**I-23**, NOVEL lead #10); plus **FS-delta** —
`SYS_WALK_OPEN` with `T_OPATH`, the Linux `O_PATH` / Plan 9 walk-without-open
equivalent. Its content lives, code-verified and current, in:

- the **I-23 invariant** — the cooperative chroot enforcement, the post-service-
  FIRST / chroot-SECOND ordering (the chroot displaces the namespace root, so the
  service directory becomes unnameable — the listener survives as a handle), the
  boot-time confinement proof, the "blind-to" a service that never chroots, and
  **the F1 monotonic-bound reconciliation folded here at this absorption**:

      vault/invariants/inv-i23.md

- the **FS-delta / T_OPATH mechanism** — `CWALKONLY` (the `O_PATH` navigation
  handle: walkable + create/rename/unlink/chroot base, but byte-I/O-blocked), and
  the `sys_walk_open_handler` arm that declines `dev->open`:

      vault/system/kernel/namespace/sub-kernel-spoor.md   (the CWALKONLY flag)
      vault/system/kernel/namespace/sub-kernel-stalk.md
      vault/system/kernel/entry/sub-kernel-syscall-dispatch.md

- the **worked consumer** — corvus's chroot-to-fd-0 confinement, the
  `mkdir_or_open` O_PATH `mkdir -p`, the boot smoke proof, and the shared-9P-session
  lifetime (corvus outlives joey via the `p9_attached_ref` chain):

      vault/system/userspace/services/sub-corvus.md
      vault/system/kernel/ninep/sub-kernel-ninep-attach.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **The F1 audit reconciliation was uncovered — folded at absorption.** inv-i23
  carried the cooperative model, the chroot ordering, and the boot-proof, but not
  the A-1.7 F1 correction: the doc records (line 163) that an *earlier claim was
  false* — withholding `RIGHT_TRANSFER` does **not** block a grantee from
  re-handing its capability, because the endow + `handle_dup` gate on kind + a
  rights subset, never on `TRANSFER`. The real bound is the monotonic rights
  reduction (a delegate stays `<=` R + same subtree, and cannot manufacture rights
  it lacks); withholding `TRANSFER` is least-authority hardening, not the
  enforcement. Now folded into inv-i23's Enforcement, so a future reader cannot
  re-assert the corrected claim.
- **The rest is current and home** — I-23 in its note; the F2 cooperative-not-
  spawner-set confinement in inv-i23; `T_OPATH`->`CWALKONLY` in sub-kernel-spoor;
  the shared-session lifetime in the 9P-attach dossiers. Zero code change.
