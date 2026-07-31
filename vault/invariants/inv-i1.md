---
id: inv-i1
type: inv
title: "I-1 — Territory operations in Proc A don't affect Proc B"
number: I-1
guards: [sub-kernel-devsrv]
validated-by: [prose]
strength: prose
created: 2026-07-31
updated: 2026-07-31
---
## Statement

Namespace state is per-Proc (per-Territory): a mount, bind, chroot, or
pivot performed by one Proc is invisible to every Proc that does not
share its Territory, and a Proc can name only what its own namespace
reaches. Isolation is the namespace boundary itself — visibility, not
rwx, is the first wall.

> Backfill note: the guard and validator sets are PARTIAL — the primary
> enforcement surface is the Territory layer (`kernel/territory.c`,
> pinned by `specs/territory.tla`), which joins with its own dossier and
> spec note at the namespace-area sweep. The devsrv edge below is the
> `/srv` realization recorded at the srv-area sweep.

## Enforcement

On the `/srv` surface ([[sub-kernel-devsrv]]): the service registry is
namespace-resident — a heap-refcounted `SrvRegistry` reached ONLY through
the mounted devsrv root Spoor's `aux`, never a global. `devsrv_post_listener`
and `devsrv_open_connect` both re-validate `root->aux` against
`SRV_REGISTRY_MAGIC` and resolve names in THAT registry; stalk-3c retired
the last EL0-reachable functions that bound the boot registry directly,
so a future per-session registry is structurally unnameable from outside
the territory that mounts it. A `/srv` connection endpoint is additionally
non-transferable (the KObj_Srv listener via `handle_dup`'s NoSrvDup; the
conn Spoor via the `dc == 's'` dup guard), so the kernel-stamped peer
identity behind it cannot cross Procs.

## Validation

Prose + audit at this sweep's edge: the stalk-3c round prosecuted the
no-global-bypass property directly (HELD and STRENGTHENED — see
[[fnd-stalk3c-r1-f3]]'s disposition). `specs/territory.tla` pins the
Territory half (its spec note is pending that sweep). **blind-to:** the
as-built boot chain mounts ONE shared registry into every session
(see [[sub-kernel-devsrv]] Caveats — cross-session NAME visibility is
real and deliberate at v1.0; the isolation this invariant demands is
carried downstream by dataset scope + per-user DEKs until the
per-session-registry seam closes — [[seam-srv-registry-lifecycle]]).
