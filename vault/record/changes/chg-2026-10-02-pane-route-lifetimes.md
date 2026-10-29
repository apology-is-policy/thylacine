---
id: chg-2026-10-02-pane-route-lifetimes
type: chg
title: "Pin service work to exact pane route lifetimes"
date: 2026-10-02
arc: arc-halcyon-interaction
commits: []
touched: [sub-halcyond]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
Fixed route incarnations distinguish even coalesced removal/recreation of the
same token/leaf pair. Live retargeting and duplicate leaves refuse. Media fids
and completed images retain the exact route; old fids cannot reach a replacement.
The service retires old controller routes before consuming admission records.

Actual-source 9P schedules, six intended counterexamples and native real-fid
qualification are recorded in HALCYON-INTERACTION-STATUS. Context names remain
application-owned and do not confer identity. Clipboard dispatch remains gated.
Single-agent self-review, October 2 gate waiver and protected drafts apply.
