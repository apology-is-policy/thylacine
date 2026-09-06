# 96 — Filesystem-mutation syscalls (the FS foundation) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-fs-mutation-doc-absorb`).
The FS-mutation syscall family — `SYS_WALK_CREATE`=54 (FS-alpha), `SYS_FSYNC`=55 +
`SYS_READDIR`=56 (FS-beta), `SYS_RENAME`=57 + `SYS_UNLINK`=58 (FS-gamma, the atomic
rename+unlink corvus's identity-DB persistence rides for its write-tmp → fsync →
rename-swap → dir-fsync sequence). Its content lives, code-verified and current, in:

- the **syscall handlers + the rwx enforcement gates** — `sys_walk_create_handler`,
  `sys_rename_handler` (write+search on source and destination),
  `sys_unlink_handler` (on the parent), all gated on `dev->perm_enforced`:

      vault/system/kernel/entry/sub-kernel-syscall-dispatch.md

- the **dev9p vtable half + the #99 create-errno propagation** — `dev9p_create`
  (Tlcreate creates-and-opens for a file; Tmkdir → walk for a dir), `dev9p_rename`,
  `dev9p_unlink`, `readdir`, `fsync`, and the `create_errno` choreography (the #99
  fix to the #102 errno-loss: `p->create_errno = rc` read once by the create
  handler via `dev9p_create_errno`, clamped to the `[-4095,-2]` passthrough
  window — plus the later #99-F1 spurious-ENOENT P1 the SMP gate found):

      vault/system/kernel/ninep/sub-kernel-ninep-dev9p.md

- the **libthyla-rs userspace wrappers** (LS-3b — `t_walk_create` / `t_rename` /
  `t_unlink` / the readdir/fsync surface):

      vault/system/boundary/pouch-seam/sub-pouch-fs.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Nothing load-bearing — a clean zero-fold.** The doc's Status is a
  point-in-time snapshot (FS-alpha/beta/gamma audit-clean; the once-blocking
  "AEGIS/mallocng write-path corruption" root-caused as the #713 `eret`-window IRQ
  race, not a heap bug); the dossiers carry the current picture, including the
  #99-F1 spurious-ENOENT P1 the SMP gate surfaced after this doc froze.
- **The content is distributed** — the syscall handlers + perm gates to
  `sub-kernel-syscall-dispatch`, the dev9p vtable + #99 errno to
  `sub-kernel-ninep-dev9p`, the libthyla-rs wrappers to `sub-pouch-fs`. The
  atomic rename+unlink *mechanism* is the dev9p dossier's; corvus's *use* of it
  (the write-tmp→rename-swap identity-DB sequence) is `sub-corvus-mint`'s.
