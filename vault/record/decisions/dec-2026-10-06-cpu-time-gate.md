---
id: dec-2026-10-06-cpu-time-gate
type: dec
title: "CPU time and the scheduler's counters are shown only to their owner or a hostowner"
date: 2026-10-06
status: standing
decided-by: user-vote
affects: [sub-kernel-devproc, sub-kernel-devctl, sub-prowl, sub-coreutils-presenters, sub-diorama, sub-imperium]
created: 2026-10-06
---
## Fork

The IM Fable pass found (its F3, P2) that the trusted episode's keystroke
count and cadence were readable by any Proc: the authority's `cpu_ns` in
`/proc/<pid>/status` and `/ctl/procs`, and the per-CPU context-switch and
interrupt counts in `/ctl/cpu`. IMPERIUM-DESIGN 11.3 item 7 had already closed
the same datum on the poll path (`cons_poll.tla` NoSecretCadence). The
`/proc` formats are consumed by `ps`, so a change is an ABI change.

The first question put to the operator: gate CPU time to the owner (a Proc's
`cpu_ns` readable only by its owner or the hostowner, as `sched` already is;
`/ctl/cpu`'s interrupt and context-switch counts restricted or coarsened; `ps`
shows `-`), freeze the authority's published counters for the episode, or keep
them and document the channel.

The second, asked once the first was being built: per-CPU idle time carries
the same signal, since the kernel updates it at each wake. The options were to
restrict it too (prowl's whole-machine meters show `-` to an ordinary user),
to publish it as of the last whole second (keys per second and the secret's
length still leak, and the scheduler's idle path changes), or to keep it exact
and document it.

## Decision

The operator voted on 2026-10-06: **gate CPU time to the owner**, then
**restrict idle time too**.

Decided under the operator's "your guts" grant, as the votes' own scope:
`/ctl/sched`'s `runnable:` count and its work-conservation lines (park counts,
idle and starvation totals) are the same class as the counters the first vote
named and are restricted alike, since leaving them open would leave the gate
hollow. The system-wide counters belong to the system principal: a reader that
is `PRINCIPAL_SYSTEM` or holds `CAP_HOSTOWNER` sees them exactly, so joey's
boot benchmarks keep their measurements. Restricted values read `-`, never a
plausible zero.

## Rationale

The authority (corvus) runs as `PRINCIPAL_SYSTEM`, and no login session does,
so an owner-or-hostowner gate reaches every session that could watch an
episode. Rounding does not close a counter that changes at each wake: a key
every 100-300 ms crosses any 10 ms step. Freezing for the episode publishes the
episode's total at its end, which still gives the secret's length. Plan 9 and
Linux publish these counters to everyone; Thylacine already holds the trusted
path to a stricter rule (item 7), and a channel closed on one path and open on
another is not closed.
