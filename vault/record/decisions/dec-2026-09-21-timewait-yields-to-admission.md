---
id: dec-2026-09-21-timewait-yields-to-admission
type: dec
title: "A TIME-WAIT retiree yields to admission; nothing else does"
date: 2026-09-21
status: standing
decided-by: autonomous
affects: [sub-netd-server, sub-netperf]
created: 2026-09-21
---
## Observation

One boot in roughly eighty died before the login prompt: `netperf: FAIL -- MW
(connect)`, then `joey: /joey exited non-zero`, then EXTINCTION. The probe's
50-dial churn phase fills the 64-transport bound of
[[dec-2026-09-17-tcp-transport-retirement]] with retirees, and the phase after
it opened its connection with a plain `connect`, so its admission depended on
whether a retiree happened to have aged out yet. The same logs showed the
larger cost on EVERY boot: one of the 50 dials waited 9.81-9.85 s -- the first
TIME-WAIT expiring -- against a 2.8 ms mean. Every image had booted ten
seconds slower since the bound landed, and a program opening more than about
six short connections a second was refused for up to ten seconds at a time.

## Decision

At the bound, admission releases the OLDEST retiree that has reached TIME-WAIT,
without an abort. A retiree in any other state still holds queued data or an
unfinished close and is never touched; with no TIME-WAIT retiree, admission
refuses with ENOMEM exactly as before. The probe phase that did not retry the
admission signal now does.

This REFINES a decision the operator made by vote, and is made autonomously.
It keeps that decision's proof obligation -- received bytes and lifecycle
cleanup -- because a TIME-WAIT retiree holds no byte anyone is owed: both FINs
are exchanged and acknowledged. What it gives up is the remainder of 2MSL of
quiet time under pressure, the trade Linux makes at `tcp_max_tw_buckets`. It
narrows one stated consequence of the earlier decision ("private transports
consume capacity until completion or the deadline"): that still holds for every
retiree that carries anything. The operator may overturn it; reverting is
`tcp_admit` returning `false` at the bound.

## Consequences

Connection churn no longer stalls on TIME-WAIT, and the measured boot is ten
seconds shorter. Connection-churn callers must still handle ENOMEM: a bound
full of transports that carry data refuses as before. A 4-tuple reused inside
2MSL can meet a delayed duplicate of the old connection; sequence validation
already has to survive that after any reboot. `stats` gains
`timewait-yielded` so the pressure is visible rather than silent.
