---
id: inv-i2
type: inv
title: "I-2 — fork attenuation and scoped elevation flow"
number: I-2
guards: [sub-kernel-caps, sub-kernel-proc, sub-imperium, sub-haul]
validated-by: [spec-imperium, gate-smp]
strength: spec
created: 2026-09-17
updated: 2026-09-17
---
## Statement

Fork never gives a child caps absent from its parent or requested mask.
Elevation-only caps are stripped unless they belong to the parent's granted,
propagating legate scope. Only the cap device may add elevation authority;
CAP_ALL and CAP_ELEVATION_ONLY are disjoint.

## Enforcement

`rfork_internal` computes the scoped carve, then intersects parent and requested
caps. `proc_become_legate` publishes a coherent scope and forbids nested
propagating redemption. `devcap` bounds grants to the authority-specific masks.
POST_SERVICE bit 13 follows these rules; it is not a TCB role flag.

## Validation

[[spec-imperium]] checks attenuation and unauthorized flow with clean and mutant
configurations. Kernel cap/fork tests and [[sub-imperium]]'s runtime tests cover
implementation behavior. The model abstracts individual bit values; registry
quota and byte-relay behavior require separate runtime tests.
