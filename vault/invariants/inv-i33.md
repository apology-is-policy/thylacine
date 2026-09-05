---
id: inv-i33
type: inv
title: "I-33 — namespace name retention is non-load-bearing"
number: I-33
guards: [sub-kernel-path, sub-kernel-spoor, sub-kernel-stalk, sub-kernel-territory, sub-kernel-content]
validated-by: [gate-smp]
strength: prose
created: 2026-08-01
updated: 2026-08-03
---
## Statement

Every Spoor carries a refcounted copy-on-walk `Path` (its cleaned
namespace name — the Plan 9 `Chan.path`), but the resolver is WRITE-ONLY
to it: stalk and the walk/create handlers append; nothing reads `->path`
to resolve, permission-check, or cross. A wrong, stale, absent, or
failed-to-allocate Path therefore changes only the cosmetic content of
the introspection readers (`SYS_FD2PATH`, `/proc/<pid>/ns`,
`/proc/<pid>/fd`), never a resolution or permission result; a path-alloc
failure leaves the Path NULL and the WALK SUCCEEDS. Path lifetime is
subordinate to its Spoor's (one ref per referencing Spoor, atomic, freed
with the last holder); the string is immutable once built — only
`path->ref` is ever concurrently mutated.

## Enforcement

`kernel/path.c` ([[sub-kernel-path]]: fresh-allocate-never-mutate; NULL
on OOM/overflow/empty component); `kernel/spoor.c` ([[sub-kernel-spoor]],
which owns the field: `spoor_clone` shares via `path_ref`;
`spoor_path_extend`/`spoor_path_transplant` replace thread-local or
pre-publish only; `spoor_free_internal` drops); the three resolver hook
sites (stalk per-step + cross/adopt transplants, walk-open, walk-create).
The write-only property is a grep-complete obligation re-verified at the
#66a round.

The Spoor side is where the fail-soft is *implemented by omission*:
`spoor_path_extend` installs whatever `path_addelem` returned without
checking it, so an allocation failure becomes a NULL name and the walk
proceeds. That is the invariant working, not a missing check — but it is
the one place where the correct code and the bug look identical, so it
carries a comment rather than a guard.

The one place a Path is *not* an accumulation is the boot filesystem's attach
root ([[sub-kernel-content]]), which seeds itself as `/` at birth. That is sound
for the narrow reason that it happens before publication, so the immutability the
rest of this invariant relies on is established rather than violated — and it is
visible only when that filesystem is the namespace root, since crossing a mount
transplants the mount point's name over it.

On the territory side ([[sub-kernel-territory]]), `PgrpMount.mp_path`
mirrors the same discipline for the mount POINT's name (#66b). Its
refcount is ledgered at the same four hooks as the entry's `source`
Spoor — mount's append, MREPL's ref-new-before-unref-old, unmount's
drop-before-overwrite, clone's share, final release — and it is
grep-complete NEVER read for a decision: `mount_key_eq`,
`mount_is_point_id`, `would_create_mount_cycle`, and `mount_lookup` all
key on `(mp_dc, mp_devno, mp_qid_path)`, leaving `territory_format_ns`
as the sole reader anywhere. A wrong or NULL `mp_path` misreports
`/proc/<pid>/ns` and changes no crossing decision.

## Validation

Prose + the `path.*` battery (8) + `stalk.path_*` (4, with no-leak
balance asserts) + the joey fd2path boot probe + [[gate-smp]].
**blind-to:** semantic staleness (a rename/unmount leaves live Paths
describing the old layout — by design, fd2path is provenance not a
re-open key); the confined-Proc namespace-layout disclosure via inherited
names (#66a F4 — an information-leak framing, not an authority one; the
v1.x re-stamp-at-chroot is the recorded fix).
