---
id: dec-2026-09-24-user-authority-direction
type: dec
title: "Separate use, identity administration and delegation authority"
date: 2026-09-24
status: standing
decided-by: user-vote
affects: [sub-corvus, sub-imperium, sub-kernel-caps]
created: 2026-09-24
---
## Fork

How should production user administration replace ordinary reliance on broad
hostowner authority, while preserving scoped authority and trusted attention?

## Research

The September 24 review compared MANDATE-DESIGN / IDENTITY-DESIGN / NOVEL with
the current Corvus admin gates and primary documentation for Plan 9 factotum,
seL4 capabilities, Genode sessions and Fuchsia handle rights/routing. Sources
and verified tree mechanisms are in USER-AUTHORITY-DESIGN sections 2 and 18.

## Options

Expose existing CAP_HOSTOWNER through a management client; add one broad
CAP_USER_ADMIN; or separate bounded operational use, identity administration
and explicit delegation. The first two retain unnecessary cross-domain power.

## The call

The operator endorsed the research recommendations and asked for an
implementation specification including missing authority, with `imperium` as
the userspace driver. This records that direction and specification request,
not advance approval of every new kernel/transaction/ABI detail. The concrete
specification is `docs/USER-AUTHORITY-DESIGN.md`, for review. Implementation and
numeric reservations require the detailed design contract to be ratified.

## Rationale

A temporary permission to use a resource is not sufficient to create a permanent
grant for another user. Administrative authority needs explicit subject, action,
resource and delegation bounds, exercised through the existing trusted path.
Corvus holds the policy; the command submits intent and never becomes an
unbounded source of authority. Durable admin eligibility must not become an
ambient privilege in every application. Live revocation and precursor integrity
are explicit prerequisites, not consequences of changing the command's name.
