---
id: fnd-pollwalk-r1-f3
type: fnd
round: adt-pollwalk-r1
severity: P3
status: fixed
title: "A WAIT owner's free did not check the spec's empty-chain assumption"
surface: [sub-kernel-poll]
threatens: [inv-i8]
fixed-by: chg-2026-10-28-poll-hook-walks
regression: "assertions: poll_deferred_await extincts inside a deferring lock; thread_release extincts on a thread freed with defer_depth or a chain non-empty"
created: 2026-10-28
---
poll_defer.tla assumes the freer's own chain is empty; the code checked only the
awaited record. Both checks now hold the assumption.
