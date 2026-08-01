---
id: inv-i28
type: inv
title: "I-28 — path resolution contained + per-component X-search"
number: I-28
guards: [sub-kernel-stalk]
validated-by: [gate-smp]
strength: prose
created: 2026-08-01
updated: 2026-08-01
---
## Statement

Path resolution is contained at the Territory `root_spoor` and X-searched
per component:

- `..` can never ascend above the resolution base — the resolver pops its
  own in-call trail, and at the bottom the pop is a no-op, so no path
  escapes the chroot/pivot boundary. The cwd join is convenience, not
  authority: it hands the resolver an absolute path, and the resolver
  re-clamps.
- Every directory hop on a `perm_enforced` Dev requires PERM_X for the
  calling principal, fail-closed when the Dev cannot vouch for the
  metadata. Traversal denial MASKS deeper outcomes (ACCES, never NOENT —
  no existence probing under a forbidden directory), an ordering the
  POUNCE post-scan preserves bit-for-bit when components batch.
- Mount crossing is keyed by the full Plan 9 Spoor identity
  `(dc, devno, qid.path)` and grants nothing: the MOUNTED root's
  permissions govern traversal into a crossed tree, and reaching a mount
  requires X-searching the path to it.

## Enforcement

`stalk_core` (the `..` pop guard + the per-component and POUNCE X-gates +
the fail-ordering post-scan); `stalk_cross_mounts` + `mount_lookup` (the
identity key, ref-held under `ns_lock`); `sys_walk_open_handler` (the
single-hop twin — source + result crosses since #957, same X ordering);
`exec_load_from_namespace` (#58 — every spawn resolves through stalk with
the OEXEC gate; no flat-table fallback survives on the EL0 path);
`territory_resolve_cwd` (the LS-4 join stays lexical — it can only produce
an absolute path the resolver then contains). The territory half (the
mount-table serialization + `root_spoor` swap under `ns_lock`) and the
exec/spawn half gain their `guards` edges at those surfaces' sweeps — the
backfill-hook pattern.

## Validation

Prose + the kernel battery (`stalk.dotdot_containment`,
`stalk.xsearch_deny`, `stalk.cross_mount_xsearch_deny`,
`stalk.pounce_acces_masks_noent`, `exec_ns.*`) + the boot E2Es (the joey
stalk-1/stalk-2 dev9p resolutions) + [[gate-smp]] for the concurrent
mount-table legs. **blind-to:** TOCTOU between the X-search snapshot and
later byte I/O (the A-3 open-time-snapshot model — deliberate); the POSIX
pathname-FORM gaps tracked at [[seam-posix-pathname-form-gates]] (a
non-directory mid-path reports NOENT rather than ENOTDIR — a lie about
WHY, never a containment breach); single-hop handler arms that no kernel
test drives (the user-VA harness gap — covered E2E by boot probes).
