---
id: fnd-net6b-r1-f1
type: fnd
title: "The global poll-pump pumped only the head op's client -- a second QTPOLL client starves"
round: adt-net6b-r1
severity: P1
status: fixed
surface: [sub-kernel-ninep-dev9p-poll]
threatens: [inv-i9]
fixed-by: chg-2026-06-18-net6b4-close
created: 2026-07-31
---
## Prosecution

Phase 3 pumped only the FIRST non-terminal op's client. A perpetually-
parked op on client CX (non-terminal + non-empty poll_list -> never
reaped/GC'd, at the LIFO head) pins the pump to CX forever; client CY's
ready socket is never demuxed; CY's poller hangs on a satisfiable socket.
v1.0-SAFE (exactly one QTPOLL client -- the single netd mount whose one
elected reader demuxes all readiness replies by tag); latent under the
per-user-netd v1.x config. The self-audit traced the single-client path
exhaustively and stopped; the prosecutor found the multi-client gap.

## Disposition

Fixed: `dev9p_poll_collect_clients` -- distinct clients deduped into a
bounded array (DEV9P_POLL_MAX_PUMP=16), an extra session-ref borrow-guard
per client, every collected client pumped per cycle. The deterministic
two-QTPOLL-client fairness regression remains OWED (no in-tree config
drives two clients); verification rests on the two-prosecutor review, the
round-2 self-audit ref-balance trace, the boot proof, and the SMP gate.
