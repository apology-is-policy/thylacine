# 63 — SYS_SPAWN_WITH_CAPS: cap-subset spawning (P5-spawn-caps) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-sys-spawn-with-caps-doc-absorb`).
The third spawn variant: gives the child `parent->caps & cap_mask` (a subset of the
parent's fork-grantable caps), wrapping the kernel-internal `rfork_with_caps`.
Needed at P5-corvus-bringup so joey can spawn `/sbin/corvus` with a subset of its
`CAP_ALL`. Its content lives, code-verified and current, in:

- the **fork-grantable capability ceiling** — `rfork_with_caps`'s
  `(parent_caps & caps_mask) & ~CAP_ELEVATION_ONLY` (I-2 monotonic reduction; a
  child can never exceed the parent's grantable set), the `cap_mask`, and the
  `cap_grant_entry` machinery:

      vault/system/kernel/security/sub-kernel-caps.md   (guarded-by inv-i2 in prose)

- the **spawn handler** that carries the cap_mask to `rfork_with_caps`:

      vault/system/kernel/execution/sub-kernel-exec.md
      vault/system/kernel/entry/sub-kernel-syscall-dispatch.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Nothing load-bearing — a clean zero-fold.** The cap-subset mechanism (the
  `& cap_mask & ~CAP_ELEVATION_ONLY` reduction, I-2) is `sub-kernel-caps`'s (whose
  Contract spells out the rfork computation); the spawn dispatch is the exec/
  dispatch dossiers'.
