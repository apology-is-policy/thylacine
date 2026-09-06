# 64 — SYS_SPAWN_FULL: combined fds + caps spawning (P5-spawn-full) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-sys-spawn-full-doc-absorb`).
The most general spawn variant: unions `SYS_SPAWN_WITH_FDS` (fd inheritance) with
`SYS_SPAWN_WITH_CAPS` (cap-subset via `rfork_with_caps`). Needed at P5-corvus-bringup
where joey spawns `/sbin/corvus` with a pipe pair AND a cap subset. Its content
lives, code-verified and current, in:

- the **fd-inheritance half** (positional `fd_list[i]` → child fd `i`) and the
  richer `SYS_SPAWN_FULL_ARGV` successor (argv + identity + allowance grants):

      vault/system/boundary/pouch-seam/sub-pouch-process.md
      vault/system/kernel/execution/sub-kernel-exec.md   (incl. the argv-blob double-free finding)

- the **cap-subset half** — the `(parent_caps & caps_mask) & ~CAP_ELEVATION_ONLY`
  fork-grantable reduction (I-2):

      vault/system/kernel/security/sub-kernel-caps.md

- the **spawn handler + dispatch**:

      vault/system/kernel/entry/sub-kernel-syscall-dispatch.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Nothing load-bearing — a clean zero-fold.** SYS_SPAWN_FULL is the union of the
  two variants absorbed alongside it (62 fds, 63 caps); its richer
  `SYS_SPAWN_FULL_ARGV` successor (the one every Linux/native spawn now rides) is
  thoroughly covered in `sub-kernel-exec` and `sub-pouch-process`.
