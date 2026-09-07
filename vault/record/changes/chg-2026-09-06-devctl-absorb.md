---
id: chg-2026-09-06-devctl-absorb
type: chg
title: "docs/reference retirement: absorb 33-devctl -- mostly-already-covered (srvconn + cons carry the taxonomies); fold only the /ctl/procs STATE-column note into devctl, triple-redirect stub (56 absorbed / 101 live)"
date: 2026-09-06
arc: arc-vault
commits: ["1fb76ff4"]
touched: [sub-kernel-devctl]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The devices/introspection batch's last analyzed fold-file. Verify-before-stub
showed the Explore over-flagged its "two taxonomies": sub-kernel-srvconn ALREADY
carries the /ctl/9p-sessions wedge + conservation law (9 hits -> redirect), and
sub-kernel-cons ALREADY carries the /ctl/cons receive-drop semantics
(back-pressure raw/flush vs real-drop line vs the zero-witness counter, in its
"Receive back-pressure" section) + tx room-waits/drain-drop. So the /ctl device
is a RENDER surface whose diagnostic content is owned by the counter-owners.

The one genuine gap: the /ctl/procs STATE column (job_stop_req -> STOPPED; the
DEBUG stop debug_stop_req is deliberately HIDDEN as the debugger's private I-39
view, so the render reads job_stop alone) was absent from sub-kernel-devctl.
Folded it (updated: 2026-08-16 -> 2026-09-06). The /ctl/cons producer-implication
diagnostic was judged adequately covered by cons's back-pressure-vs-loss
distinction -- not worth a second cons fold.

Triple-redirect stub (devctl for the device + gates + STATE column; cons for the
/ctl/cons counters; srvconn for /ctl/9p-sessions). Drift: the doc reproduces
per-leaf render layouts that are append-only-by-contract and code-authoritative
(devctl.c). No code touched; no audit owed. view-absorption: 55 -> 56 absorbed,
101 live.
