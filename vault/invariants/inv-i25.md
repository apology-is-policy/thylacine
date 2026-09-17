---
id: inv-i25
type: inv
title: "I-25 — legate scope revocation cannot leave a late child"
number: I-25
guards: [sub-kernel-caps, sub-kernel-proc, sub-kernel-death, sub-imperium]
validated-by: [spec-imperium, gate-smp, gate-interactive]
strength: spec
created: 2026-09-17
updated: 2026-09-17
---
## Statement

Elevated authority belongs to one tagged scope and is revoked on root death or
expiry. Every member is marked for termination, independent of reparenting.
A fork racing teardown cannot publish an unmarked member after the sweep.
Elevation changes capabilities, not the user's durable principal identity.

## Enforcement

The root teardown sweep and `rfork_internal` table insertion share the process
table lock; insertion checks the parent's termination mark. Propagating scope
redemption cannot nest. Deadlines are read atomically at the EL0-return tail.

## Validation

[[spec-imperium]] includes a late-link mutant. Kernel probes and the
`ls-imperium` background-job test cover revocation; `haul-post` witnesses
revocation of a live network relay. These checks do not substitute for review
of every process-exit resource lifetime.
