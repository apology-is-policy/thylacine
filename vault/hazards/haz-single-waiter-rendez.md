---
id: haz-single-waiter-rendez
type: haz
title: "Single-waiter Rendez on shared-reachable state"
applies-to: [global]
instances: [fnd-349-r1-f1, fnd-rw4-rev2-f1]
created: 2026-07-31
updated: 2026-08-01
---
## The failure shape

A single-waiter `Rendez` guarding state reachable from more than one thread
(peer Threads of a Proc, or any Proc resolving through a shared object like
the dev9p client): the SECOND concurrent sleeper trips
`extinction("sleep: rendez already has a waiter")` — an unprivileged,
SMP-reachable kernel panic. The kernel must be sound against any EL0
program, so "no current program drives two threads in here" is the
latent-P1 trap, not a safety argument.

## The tell

- A `Rendez` added for one field of a struct whose siblings are shared.
- "Follows the `rpc->done` pattern" reasoning — `rpc->done` is safe BECAUSE
  each rpc owns its rendez; the pattern does not transfer to a shared one.
- Any wait added to an object reachable via a mount, a handle table, or an
  `rfork`-shared group.

## The countermeasure

A multi-waiter `poll_waiter_list` (each waiter parks on its OWN stack
rendez; the waker walks the list) — or a structural single-drainer guard
(the devcons single-reader busy-guard shape). The per-struct sweep in the
self-audit checklist exists for exactly this class.
