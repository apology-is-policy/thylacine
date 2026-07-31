---
id: chg-2026-06-21-weft7-close
type: chg
title: "Weft-7: the arc close — the focused audit, the CAP_HW_CREATE share gate, the #290 bench correction"
date: 2026-06-21
arc: arc-weft
commits: ["c9bafd02"]
touched: [sub-netd-server]
established: []
closed: [fnd-weft7-r1-f4]
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
The whole-Weft-EL0-surface prosecution ([[adt-weft7-r1]]): 0/0/1/3, not
dirty. The P2 (an ungated SYS_WEFT_SHARE = an unprivileged
registry-squat DoS) and the kernel P3s live kernel-side (that sweep's
backfill); the netd finding ([[fnd-weft7-r1-f4]] — the raw-pointer ring
sites' single-threadedness preconditions made explicit INVARIANT
notes) closed here. Also carried in the close: the #290 bench
CORRECTION — the original "weft ~4–10× slower" claim was a
measurement artifact (unmatched sizes; a transport-independent
readiness stall attributed to weft); the instrumented head-to-head
shows a dead heat aggregate with weft's data-move ~2× faster, and the
real levers are #221/#288. The #289 seam (a transient SYS_WEFT_MAP
failure pins a flow byte-copy) is recorded at [[sub-netd-server]]
until the kernel weft sweep mints its own note.
