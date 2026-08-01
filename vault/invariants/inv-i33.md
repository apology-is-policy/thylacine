---
id: inv-i33
type: inv
title: "I-33 — namespace name retention is non-load-bearing"
number: I-33
guards: [sub-kernel-path, sub-kernel-stalk]
validated-by: [gate-smp]
strength: prose
created: 2026-08-01
updated: 2026-08-01
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

`kernel/path.c` (fresh-allocate-never-mutate; NULL on OOM/overflow/empty
component); `kernel/spoor.c` (`spoor_clone` shares via `path_ref`;
`spoor_path_extend`/`spoor_path_transplant` replace thread-local or
pre-publish only; `spoor_free_internal` drops); the three resolver hook
sites (stalk per-step + cross/adopt transplants, walk-open, walk-create).
The write-only property is a grep-complete obligation re-verified at the
#66a round; the territory-side `PgrpMount.mp_path` mirror (#66b — read
only by `territory_format_ns`) gains its edge at the territory sweep.

## Validation

Prose + the `path.*` battery (8) + `stalk.path_*` (4, with no-leak
balance asserts) + the joey fd2path boot probe + [[gate-smp]].
**blind-to:** semantic staleness (a rename/unmount leaves live Paths
describing the old layout — by design, fd2path is provenance not a
re-open key); the confined-Proc namespace-layout disclosure via inherited
names (#66a F4 — an information-leak framing, not an authority one; the
v1.x re-stamp-at-chroot is the recorded fix).
