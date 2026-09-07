# 102 — The legate: bounded clearance elevation (I-25) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-legate-doc-absorb`).
The A-4a legate mechanism (IDENTITY-DESIGN.md §9.8, invariant **I-25**): a durable
user (its `principal_id` UNCHANGED) granted extra capabilities for a bounded
*scope* — a process subtree plus an optional time — fully revoked on scope exit.
Its content lives, code-verified and current, in:

- the **legate kernel mechanism** — the fork-grantable capability ceiling,
  `CAP_ELEVATION_ONLY` (held by no Proc at creation, stripped at every fork via
  `(parent_caps & caps_mask) & ~CAP_ELEVATION_ONLY`), `CAP_GRANT_CLEARANCE`,
  `proc_become_legate` (the cross-thread writer), the `cap` device wire (the
  32-byte clearance grant, its `kind` on the table entry), and the legate
  lifecycle:

      vault/system/kernel/security/sub-kernel-caps.md   (guarded-by inv-i25;
      its title is "the fork-grantable ceiling, the cap device, and the legate")

- the **corvus clearance subsystem + the A-4a-3 verbs** — the clearance database
  (who may activate which level), the activation path that *mints* a legate, and
  the `ADMIN_ELEVATE` / `RECOVER` verbs on the 9P verb wire:

      vault/system/userspace/services/sub-corvus.md

- the **`perm_check` axis** the elevated caps feed (`CAP_DAC_OVERRIDE` etc.):

      vault/system/kernel/security/sub-kernel-perm.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Nothing load-bearing — a clean zero-fold.** The legate mechanism, its
  lifecycle, and the I-25 invariant are all carried by `sub-kernel-caps`; the
  corvus-side clearance subsystem and verbs by `sub-corvus`; the enforcement axis
  by `sub-kernel-perm`.
- **The cross-referenced docs are themselves absorbed or absorbing** — `75-devcap`
  (the `cap` device this generalizes) and `95-identity` (durable identity,
  unchanged by elevation) and `99-fs-permission` (the `perm_check` axis) all point
  into `sub-kernel-caps` / `sub-kernel-perm`; the content is distributed there.
