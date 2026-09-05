---
id: inv-i22
type: inv
title: "I-22 — no identity carries ambient super-authority"
number: I-22
guards: [sub-kernel-perm, sub-kernel-caps, sub-kernel-devproc]
validated-by: [prose, gate-smp]
strength: prose
created: 2026-08-02
updated: 2026-08-02
---
## Statement

There is no privileged **identity**. No principal — not the system principal,
not the first user, not the boot chain's — is granted an authority by virtue of
who it is. Every elevated authority is a **capability**: something a Proc holds,
that can be examined, that is stripped at fork, and that is obtained only through
an auditable grant.

This is the invariant that says Thylacine has no root. Unix's `uid == 0` is
exactly the thing it forbids.

## Enforcement

Three surfaces enforce it, and two of the three do so by a conspicuous *absence*.

**[[sub-kernel-perm]] — the identity axis.** The filesystem permission check
special-cases **no** `principal_id`. Not even the system principal: a
system-identity Proc that has not been elevated is judged by the same mode bits
as anyone else. The two things that bypass an rwx denial are the unified
host-owner capability and the finer discretionary-override capability — both
capabilities, neither an identity. The absence of a `PRINCIPAL_SYSTEM` branch in
that function *is* the enforcement, which makes it the kind of invariant a
refactor can quietly break by adding a helpful special case.

**[[sub-kernel-caps]] — the elevation path.** Capabilities that confer authority
over others are marked elevation-only and stripped unconditionally at fork, so
they cannot be inherited into a lineage. They are acquired only by redeeming a
grant through the capability device, and the strongest of them additionally
requires the console — a physical-presence gate no remote or spawned Proc can
satisfy. The scope-bounded elevation this produces is I-25's.

**[[sub-kernel-devproc]] — the process-control gates.** Every gate is computed
directly rather than routed through the permission check, precisely so no
identity can short-circuit it and so each capability axis stays separable per
gate. The kill gate and the debug gate ([[inv-i26]], [[inv-i39]]) name their axes
explicitly and admit nothing else.

The system principal is nonetheless *distinguished* in one respect, and the
distinction is worth stating so it is not mistaken for a violation: it is exempt
from the per-Proc resource caps ([[inv-i32]]). That is a **resource** axis, not a
privilege one — an exemption from a denial-of-service bound, not an authority
over another Proc — and it is unforgeable because identity is immutable on a
running Proc and the capability that sets identity refuses to set it to system.

## Validation

Prose and unit tests over the predicates, plus the negative tests that drive a
non-system child through a denial. There is no model: the property is the
*absence* of a branch, and a model proves the presence of behaviour rather than
the absence of a special case.

**blind-to:** nothing checks this mechanically. A future privileged-identity
check would compile, pass every test that does not specifically construct a
system-identity Proc without capabilities, and read as a helpful fix. The
defences are the audit-trigger discipline on these files and the fact that all
three sites document the absence explicitly rather than leaving it implied —
which is why the "no `PRINCIPAL_SYSTEM` branch" line appears in the prosecution
list of every dossier that guards this.
