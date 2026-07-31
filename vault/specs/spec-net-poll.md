---
id: spec-net-poll
type: spec
title: "net_poll.tla"
models: [sub-kernel-ninep-dev9p-poll]
pins: [inv-i9]
cfgs:
  - "net_poll.cfg -- clean (NoMissedNetPoll: no readiness edge lost between the sample and the park)"
  - "net_poll_liveness.cfg -- Spec_Live: PollerEventuallyServed (a ready socket eventually wakes its poller)"
  - "net_poll_buggy_lost_ready.cfg -- buggy: BUGGY_LOST_READY, the NoMissedNetPoll counterexample (sample before register/probe -> the edge lands unobserved and the poller strands)"
gate: "Pre-commit re-run for ANY change to the dev9p_poll register/probe/park protocol (the spec-first re-enablement terms for this surface)."
created: 2026-07-31
updated: 2026-07-31
---
## Abstraction

One QTPOLL Spoor, one poller, one kthread, one netd socket; readiness is a
boolean edge. Deliberately beneath the model: the multi-client pump
fairness (F1/R2-F1 — prose + the collect bound), the widen/union
machinery, OOM degrades, memory ordering of the cached-revents bridge, and
the #845 abandon plumbing. The teardown lifetime is its own module —
[[spec-net-poll-teardown]].

## Action-site map

| Spec action | Impl (kernel/dev9p_poll.c) |
|---|---|
| `PollerRegister` | `dev9p_poll` — hook registered, covering probe ensured, THEN the not-ready sample |
| `NetdReplyDemux` | `dev9p_poll_complete` (under `c->lock`: record bitmap + terminal + wake) |
| `KthreadWalk` | `dev9p_poll_service_once` (reap/GC/pump/park) |

NoMissedNetPoll is [[inv-i9]] specialized to the elicited-readiness relay.
