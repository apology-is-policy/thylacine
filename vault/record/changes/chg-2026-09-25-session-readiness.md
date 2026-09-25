---
id: chg-2026-09-25-session-readiness
type: chg
title: "Connect Halcyon session media to native readiness aggregation"
date: 2026-09-25
arc: arc-halcyon-interaction
commits: []
touched: [sub-halcyond, sub-halcyond-service-wire, sub-libthyla-rs, sub-substrate-interactive]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-25
---
The existing two-connection session media service now contributes one notification
descriptor to the compositor poll set. Protocol work remains on the UI thread.
Owned endpoint retirement delays slot reuse until the worker's old poll returns;
listener acceptance waits for that reclamation notice. A published service failure
ends the poster, since closing a listener cannot unpost its name.

The nondefault qualification feature controls native poll schedules and injects
setup failures without exposing production fault controls. The media probe uses
two waves of routed, byte-verified image uploads and tests quiescence between
waves. Runtime results, failures, negative controls and artifact provenance
are recorded in docs/HALCYON-INTERACTION-STATUS.md. This remains single-agent
self-review; it does not claim expanded admission, clipboard activation or a
Main landing.

Native controlled schedules pass, all three targeted mutants fail their named
checks and restored code passes. Published failure/process-exit/repost passes
with the corrected short-lived fixture. Default Halcyon/probe binaries contain
no qualification controls; the normal readiness gate passes in 9 seconds and
the final graphical media/SAK/manual gate in 97 seconds. Pi is released; the
normal commit hooks await the Mac lease. Final evidence is retained under
work/hi1-session-evidence/. Graphical session recovery after a worker failure
remains open.
