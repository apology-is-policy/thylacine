---
id: inv-i23
type: inv
title: "I-23 — a service's filesystem authority is bounded by its endowed storage capability"
number: I-23
guards: [sub-corvus]
validated-by: [prose]
strength: prose
created: 2026-08-04
updated: 2026-08-04
---
## Statement

A service that is handed a storage capability — an open directory
descriptor standing for the subtree it is entitled to — must have no
filesystem authority beyond it. Not "should not use more"; **cannot reach
more**: after the endowment is taken up, no path the service can name
resolves above it.

The capability is the grant. There is no separate policy statement, no
allowlist of paths, and no name the service could utter that would widen
it. This is [[inv-i28]]'s containment applied to a *service's* whole
world rather than to a single resolution: the root itself is the bound.

## Enforcement

**The service chroots itself, and that is the whole mechanism at v1.0.**
The kernel offers the operation — replacing a territory's root with a
descriptor the caller already holds — but does not compel it. So the
invariant is **cooperative**: it holds for a service that takes up its
endowment and does not hold for one that forgets. That is an unusual
shape for a section-28 invariant and is the reason this note exists
separately from the mechanism.

Two things make the cooperative form less fragile than it sounds:

- **The endowment is the only root the service is given.** A service is
  spawned with the descriptor at a fixed slot and is expected to chroot to
  it before its first file touch. The window between spawn and chroot is
  the entire exposure, and it is a handful of instructions in the
  service's own prologue.
- **Failing to receive one is fatal.** [[sub-corvus]] treats a missing or
  invalid descriptor as a boot failure rather than falling back to the
  inherited root — the fallback being exactly what would silently convert
  a bounded service into an unbounded one.

**[[sub-corvus]] is the worked example**, and demonstrates the ordering
constraint the invariant imposes on a service that also needs the
namespace: it posts its service endpoint *first*, using the inherited
namespace, and chroots *second*, because the chroot displaces the
namespace root and makes the service directory unnameable. The listener
survives that displacement because it is a handle rather than a name —
which is the capability model doing exactly what it is for. Any
reordering breaks one of the two: a file touch before the chroot escapes
the bound, and a post after it cannot find its directory.

corvus additionally **proves** the confinement at boot rather than
assuming it: it creates and reads inside the capability, and asserts that
a known path above it is unreachable. A cooperative invariant that the
cooperator verifies is meaningfully stronger than one it merely intends.

## Validation

Prose, plus the per-service boot-time confinement probe where one exists.
There is no model and no kernel-side check.

**blind-to:** everything about a service that does not chroot. Nothing
enumerates which services hold endowments, nothing verifies that each
took one up, and a new endowed service that simply omits the call would
boot, work, and pass every test — its excess authority invisible until
something reads its prologue. The confinement probe is per-service and
opt-in, so its absence is silent. A kernel-side "this Proc was handed a
storage capability and has not chrooted" assertion is the shape that
would make the invariant non-cooperative; it does not exist.
