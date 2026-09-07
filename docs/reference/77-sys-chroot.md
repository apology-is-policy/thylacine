# 77 — SYS_CHROOT (P5-stratumd-stub-bringup-e2) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-chroot-doc-absorb`).
The v1.0 territory-root pivot syscall (`SYS_CHROOT` = 35): stamps the calling
Proc's Territory `root_spoor` so a subsequent `SYS_WALK_OPEN(FROM_ROOT, ...)`
walks from the pivoted root. Its content lives, code-verified and current, in:

- the **`territory_chroot` mechanism** — bump-before-swap (`spoor_ref` extincts on
  a corrupted source, leaving `root_spoor` untouched), `spoor_clunk`-not-`unref`
  on the displaced root (the Dev close hook must run), the idempotent same-pointer
  no-op, the chroot-vs-pivot precondition split, the clone ref-copy, the
  final-release drop, and `MountRefcountConsistency` (root_spoor is a term in the
  formula) — **plus the one-way lifetime caveat folded here at this absorption**:

      vault/system/kernel/namespace/sub-kernel-territory.md   (audit: hard, inv-i1/inv-i3/inv-i28)

- the **syscall handler + the FROM_ROOT walk companion** — the `KOBJ_SPOOR` +
  `RIGHT_READ` gate, and the `SYS_WALK_OPEN_FROM_ROOT` sentinel resolving the
  source from `root_spoor` without a fresh ref (the Territory's ref keeps it live):

      vault/system/kernel/entry/sub-kernel-syscall-dispatch.md

- the **spec** — `specs/territory.tla::Chroot(p, s)` + the
  `territory_buggy_chroot_no_refbump` counterexample:

      vault/system/kernel/namespace/sub-kernel-territory.md (spec cross-ref)

**What this file got WRONG or MISSED by the time it was absorbed:**

- **The one-way-chroot lifetime caveat was uncovered — folded at absorption.** The
  dossier carried the chroot/pivot mechanism, the refcount discipline, and the
  spec, but not the caller-discipline consequence: a chroot is one-way at v1.0
  (no unchroot / `chroot(NULL)`), so the ref it takes on the root Spoor is held for
  the Proc's whole life — which is why the long-running init exercises chroot only
  through short-lived child probes that release it on exit, never in its own
  persistent context, where the pin would wedge a teardown waiting for the 9P
  session's EOF. Now folded into the dossier's chroot mechanism.
- **The rest is current and home** — the dossier laps this doc (it covers
  `SYS_PIVOT_ROOT`, the v1.x successor the doc only sketches). Zero code change.
