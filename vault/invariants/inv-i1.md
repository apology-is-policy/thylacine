---
id: inv-i1
type: inv
title: "I-1 — Territory operations in Proc A don't affect Proc B"
number: I-1
guards: [sub-kernel-territory, sub-kernel-devsrv, sub-pouch-net]
validated-by: [spec-territory, gate-smp]
strength: spec
created: 2026-07-31
updated: 2026-08-01
---
## Statement

Namespace state is per-Proc (per-Territory): a mount, bind, chroot, or
pivot performed by one Proc is invisible to every Proc that does not
share its Territory, and a Proc can name only what its own namespace
reaches. Isolation is the namespace boundary itself — visibility, not
rwx, is the first wall.

## Enforcement

On the Territory ([[sub-kernel-territory]]) — the primary surface. Every
namespace operation takes ONE `struct Territory *`; no call mutates two.
`territory_clone` reads the parent and writes only the child, so an
`rfork` produces an independent function value rather than a shared one:
the child inherits a deep copy of the mount table (with its own ref per
entry), of `root_spoor`, and of the cwd string, and diverges from that
instant. `rfork(RFNAMEG)` — the flag that WOULD share a Territory across
Procs — extincts at v1.0 ([[seam-rfnameg-shared-territory]]), so the
only multi-holder case is the peer Threads of one Proc, serialized by
[[lock-territory-ns-lock]] and [[lock-territory-dot-lock]].

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

[[spec-territory]] pins the Territory half — though as STRUCTURE, not as
a checked invariant: isolation is encoded by the data model (every
action touches one Proc's slot), so a buggy variant updating two Procs
in one step would need a temporal property to catch. RFNAMEG is the
point at which that must change.

Audit: the stalk-3c round prosecuted the no-global-bypass property
directly (HELD and STRENGTHENED — see [[fnd-stalk3c-r1-f3]]'s
disposition). **blind-to:** the
as-built boot chain mounts ONE shared registry into every session
(see [[sub-kernel-devsrv]] Caveats — cross-session NAME visibility is
real and deliberate at v1.0; the isolation this invariant demands is
carried downstream by dataset scope + per-user DEKs until the
per-session-registry seam closes — [[seam-srv-registry-lifecycle]]).
