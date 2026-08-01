# 18 — Territory primitives [ABSORBED INTO THE VAULT]

This document was absorbed at the territory sweep
(`chg-2026-08-01-territory-sweep`). Its content now lives, code-verified
and current, in the dossier:

    vault/system/kernel/namespace/sub-kernel-territory.md

(the mount table and its `(dc, devno, qid.path)` keying, MREPL
displacement, both cycle checks, chroot vs pivot_root, `territory_clone`'s
three ref classes, the final-release ordering, the LS-4 cwd, and
`territory_format_ns`).

**What this file got WRONG by the time it was absorbed** (the reason the
dossiers are written from the code, and the sharpest staleness the
migration has found so far):

- `PGRP_MAX_MOUNTS 8` — is 20 (grown 8 -> 16 -> 20).
- `sizeof(struct PgrpMount) == 32` — is 40.
- `struct Territory` shown with no `dot_lock` / `dot_path` / `ns_lock`,
  and `struct PgrpMount` with no `mp_path` — while its OWN Status table
  said both `mp_path` and `ns_lock` had landed. Self-contradictory.
- `mount`'s return table omitted `-3` (the mount-cycle reject), and the
  Cycle-detection section said "unchanged from P2-Eb, over `binds[]`" —
  omitting `would_create_mount_cycle` entirely, i.e. the whole I-3 fix
  that stalk-2's audit forced.
- The public-API block omitted `territory_pivot_root`,
  `territory_root_ref`, `mount_is_point_id`, `territory_format_ns`, and
  the entire LS-4 cwd quartet.
- The `mount`/`unmount` code sketches still showed the pre-stalk-2
  `path_id_t target` signature.
- "~290 LOC" (is 988); "16 tests" (are 29).
- Status listed `pivot_root` as "v1.x per CORVUS-DESIGN §10.1 Q2" though
  it landed at 16c, and "multi-component walker consuming mount table"
  as Phase 5+ though that is `stalk`; plus a literal duplicate row.

The invariants live at `vault/invariants/inv-i1.md` (per-Proc namespace
isolation) and `inv-i3.md` (the DAG); the spec at
`vault/specs/spec-territory.md` — which records that `NoCycle` proves
the DEAD bind table while the live mount-graph check is unmodeled. The
locks are `vault/locks/lock-territory-ns-lock.md` and
`lock-territory-dot-lock.md`. The audit history (P5-attach-mount,
P5-mount-syscall, the stub-e2 chroot round, LS-4, #66b, plus stalk-2 and
RW-4 recorded at the stalk sweep) lives as adt-/fnd- Record notes; the
open debt as `seam-union-mount-walk`,
`seam-rfnameg-shared-territory`, `seam-80-pivot-orphan-mounts`,
`seam-handle-based-dot`, `seam-mount-graph-unmodeled`, and
`seam-848-pivot-walk-race` (closed).

Design scripture is unchanged: `docs/ARCHITECTURE.md` §9.1 + §9.6,
`docs/STALK-DESIGN.md`, `docs/LIFE-SUPPORT.md` LS-4.
