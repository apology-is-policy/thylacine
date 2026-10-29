---
id: chg-2026-10-03-halcyon-host-handoff
type: chg
title: "Move sealed terminal binding to the session service owner"
date: 2026-10-03
arc: arc-halcyon-interaction
commits: []
touched: [sub-halcyond]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
Move sealed-host Bind/Unbind to the dedicated ordered service executor. Preserve
exact route and remote observer lifetimes across mailbox coalescing, late
success, retirement and SAK. The UI alone emits diagnostics; missing routes
withhold interaction registration without killing a healthy terminal. Enabled
NORMAL snapshots restore local membership after SAK.

523 host tests and eight intended state-machine mutations pass. Fresh native
boot/service and rebuilt media/F10SAK runs, plus a third terminal bound after
restoration, are measured in HALCYON-INTERACTION-STATUS and work/oct3-hi-bindings.
Source pins disclose the UI refusal correction between the two image pairs.
Review is single-agent. App clipboard activation, total allocation accounting
and modal clients remain open; no Main landing is implied.
