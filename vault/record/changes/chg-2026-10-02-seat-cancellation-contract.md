---
id: chg-2026-10-02-seat-cancellation-contract
type: chg
title: "Ratify strict clipboard cancellation and identify the progress prerequisite"
date: 2026-10-02
arc: arc-halcyon-interaction
commits: []
touched: [sub-halcyond, sub-tapestryd, sub-lictor]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
The operator chose option A in HALCYON-INTERACTION-SEAT-REVIEW: require exact-
generation aggregate cancellation before trusted input, retaining the five-
second deadline and bounded refusal on a stalled participant. Option B's weaker
completion policy is rejected. This commit ratifies the contract without
installing the barrier or enabling the application clipboard endpoint.

Further source tracing at 981b89caa found a dependency absent from the original
review: Halcyon's synchronous present waits for a compositor Rwrite; Tapestry's
synchronous GPU call can wait for a normal request Lictor parks during SAK.
Requiring cancellation from those same blocked owners would wait until the
SAK timeout even with live, otherwise healthy renderers. Returning a fabricated
present success is unsafe because Rwrite is the slot recycle gate.

The operator subsequently approved HALCYON-INTERACTION-SEAT-PROGRESS option 1:
independent bounded control/service owners, replacing the session service's
readiness-only ownership. The broader resumable-graphics alternative was not
selected. Neither approval is evidence of implementation. Source hashes and review
notes are in work/oct2-hi-seat. The analysis is blind to actual runtime frequency
and does not replace native concurrency, lifetime or graphical qualification.
No production source, ABI, role, kernel syscall or protected draft is changed.
