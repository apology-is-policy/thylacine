---
id: chg-2026-10-04-async-transport-qualified
type: chg
title: "Async transport and TLS pass the complete boot matrix"
date: 2026-10-04
arc: arc-halcyon-interaction
commits: [c2f462ba6, 8542dbb4b]
touched: [sub-kernel-ninep-transport, sub-tls]
established: []
closed: []
opened: []
depth: skeletal
---
The fresh AS-1/TLS matrix on8542dbb4b completed50/50 clean boots: ten each
at default CPU1/4/8 and kernel-UBSan CPU4/8. Every row records zero corruption,
external-kill, inject-miss, timing and other classifications. The wrapper exited0,
restored all four protected drafts, released Mac, and the separate log/pin
check verified every individual boot, five summaries, original source hashes
and empty index. Evidence: work/oct4-async-service/as-r3/matrix-fixed/verified.json
and smp.log. This qualifies the transport helpers and TLS correction, not private
Loom runtime, clipboard activation or a fresh graphical/Pi/min-display run.
