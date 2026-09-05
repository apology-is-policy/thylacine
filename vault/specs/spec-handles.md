---
id: spec-handles
type: spec
title: "handles.tla"
models: [sub-kernel-handle, sub-kernel-caps, sub-kernel-hwcap]
pins: [inv-i5]
cfgs:
  - "handles.cfg -- clean, 10 invariants (51,744,096 distinct / depth 28 / ~56 min on 8 cores)"
  - "handles_srv.cfg -- clean, focused on the service-kobj partition (26,784 distinct / depth 12 / 2s)"
  - "handles_buggy_elevate.cfg -- dup fabricates rights: RightsCeiling"
  - "handles_buggy_hw.cfg -- a hardware handle reaches a non-origin Proc: HwHandlesAtOrigin"
  - "handles_buggy_direct.cfg -- a cross-Proc transfer with no session: OnlyTransferVia9P"
  - "handles_buggy_caps.cfg -- a Proc gains a capability it never started with: CapsCeiling"
  - "handles_buggy_hw_dup.cfg -- a hardware handle is duplicated: NoHwDup"
  - "handles_buggy_hw_overlap.cfg -- two live handles on one hardware resource: HwResourceExclusive"
  - "handles_buggy_hw_nocap.cfg -- a hardware object created without the capability: HwHandleImpliesCap"
  - "handles_buggy_rfork_elevate.cfg -- a fork grants more than the parent holds: CapsCeiling"
  - "handles_buggy_rfork_hostowner.cfg -- a fork fails to strip the elevation-only capability: CapsCeiling"
  - "handles_buggy_spawn_fds_elevate.cfg -- an inherited fd carries rights above the parent's: SpawnFdsRightsMonotonic"
  - "handles_buggy_srv_walk_tx.cfg -- a walk mints the child in the transferable partition: WalkChildIsSrv"
gate: "any change to handle allocation, dup, the kobj partition masks, or the fork capability arithmetic"
created: 2026-08-02
updated: 2026-08-02
---
## Abstraction

The largest model in the tree by state count — fifty-two million states at depth
28, about an hour on eight cores — and the one with the most counterexamples,
eleven. It earns both by modeling **two orthogonal authority axes at once**: the
per-handle rights that bound what you may do with an object, and the per-Proc
capabilities that bound what you may create.

Handles are a *set* per Proc rather than a table, because the slot index is an
implementation detail and every invariant reasons over "what does this Proc
hold", never over indices. Each record carries the object, its rights, the Proc
it originated in, and a **provenance tag** — kernel grant, intra-Proc duplicate,
transfer over a session, spawn inheritance, walk derivation, or the buggy
direct transfer. Several invariants are stated purely over that tag, which is
what lets a structural property ("no handle ever got here by a route that does
not exist") be checked as a state predicate.

Objects are partitioned into three constant sets: transferable, hardware, and
service. Per-object **origin rights** are fixed at allocation and never change,
giving every later rights check a ceiling to be a subset of. Per-Proc
**capability ceilings** are dynamic — set at fork from the parent's *current*
capabilities — which is what turns "capabilities only reduce" into a checkable
state invariant rather than a claim about a sequence of operations.

**Deliberately beneath the model:**

- **the handle table's internals.** The per-Proc lock, the by-value snapshot
  with the object's refcount held, the paired put that drops it outside the
  lock — none of it is here. The model's handles are a set, so the lifetime pass
  that made concurrent access safe is invisible to it;
- **object lifetime.** Refcounts, the free decision, mapping counts — all of it
  belongs to [[spec-burrow]]. This model is about handle *policy*;
- **the rights semantics.** What a right permits is never modeled; only that
  rights sets shrink;
- **the session's wire protocol** — sessions open and close as abstract
  directed pairs;
- **the fourth partition.** See below.

## Action-site map

| Action | Site |
|---|---|
| `Init` | `handle_init` plus the kernel Proc's initial capability mask; every other Proc starts with an empty table, empty capabilities, and an empty ceiling |
| `HandleAlloc(p, k, granted)` | `handle_alloc`, and the capability-gated hardware-object creation paths that call it. Origin rights are fixed here, permanently |
| `HandleClose(p, h)` | `handle_close` — releases the slot and drops the object reference |
| `HandleDup(p, h, r)` | `handle_dup` — a subset check on rights, and an outright rejection of any non-transferable kind |
| `RforkWithCaps(parent, child, granted)` | `rfork_internal` — the child's capabilities are the parent's *current* set intersected with the requested mask, and the elevation-only bits are stripped. The intersection is what makes elevation impossible regardless of what the caller asks for |
| `HostownerGrant(p)` | the capability device's redemption path — the **sole** action that admits the elevation-only capability into any Proc, and it raises the ceiling alongside the capability so the ceiling invariant still holds |
| `WalkDerive(p, h, k)` | the service Dev's walk — the child object is structurally in the service partition because the Dev is its own |
| `OpenSession` / `CloseSession` | abstract; the 9P client's attach and clunk |
| `HandleTransferVia9P(...)` | **no site — see below** |
| every `Buggy*` action | no sites; each is prevented structurally, and the note against each says what future change would re-open it |

| Invariant | Obligation |
|---|---|
| `RightsCeiling` | every handle's rights are a subset of its object's origin rights |
| `HwHandlesAtOrigin` | [[inv-i5]] — a hardware handle is held only by the Proc it was granted to |
| `SrvHandlesAtOrigin` | the same statement for service connections |
| `NoHwDup` | a hardware handle is never duplicated |
| `HwResourceExclusive` | at most one live handle per hardware resource, across all Procs |
| `HwHandleImpliesCap` | holding a hardware handle implies holding the capability to create one |
| `OnlyTransferVia9P` | no handle carries the direct-transfer provenance |
| `CapsCeiling` | every Proc's capabilities are a subset of its ceiling |
| `SpawnFdsRightsMonotonic` | an inherited fd's rights are a subset of the parent's on the same object, snapshotted at spawn |
| `WalkChildIsSrv` | a walk-derived handle points at a service object — a connection's kernel-stamped identity cannot be forged across a walk |

## Two places the model and the tree have diverged

**The transfer path does not exist.** `HandleTransferVia9P` is a modeled action
with no implementation — not a stub, not a rejected call, *nothing*. The name
appears only in comments. So `OnlyTransferVia9P` is proven of a system where the
sole cross-Proc handle route is unbuilt, and holds **vacuously**: there is no
transfer at all, let alone a direct one.

That is a perfectly good posture — the safe direction, and the invariant is
ready for the day the path lands. What is not good is that the action-site map
this note replaces says the opposite in two voices: a currency note at the top
states plainly that no transfer codepath was ever built, and the table below it
still says the path is "defined as a stub, returns unsupported." The correction
was written **above** the stale text rather than applied to it, so the document
contradicts itself and the wrong half is the one in the table a reader consults.

**There is a fourth partition in the code and three in the model.** The
asynchronous-ring object kind landed later as a fourth non-transferable
partition, with its own mask and its own disjointness assertions — the header
now carries seven static assertions where the model knows of three sets. Its
non-transferability is enforced structurally and identically to the other two,
so nothing is unsound; but the property is held by the compiler alone, with no
counterexample configuration behind it, and the model cannot express a statement
about it.

Both gaps are the same shape and both are **fail-safe** — an unmodeled
non-transferable partition and an unbuilt transfer path each err toward less
authority, not more. That is why they went unnoticed.

## What the counterexamples are for

Eleven configurations is a lot, and they are not eleven variations on one theme.
Four are about **rights** (fabricating on dup, escaping the origin ceiling,
inheriting more than the parent held at spawn, minting a walk child in the wrong
partition). Four are about **hardware exclusivity** (transfer, duplication,
overlapping claims, creation without the capability). Three are about
**capabilities** (raising them outright, raising them through a fork, failing to
strip the elevation-only bit at a fork).

The last of those is worth naming: an elevated parent that forks without
stripping leaks its elevation to a child that never passed the console gate. It
is caught not by a bespoke invariant but by `CapsCeiling`, because the child's
ceiling *is* correctly stripped while its capabilities are not — the two
diverge, and the general invariant notices. That is the argument for modeling
the ceiling as state rather than checking the strip at its call site.
