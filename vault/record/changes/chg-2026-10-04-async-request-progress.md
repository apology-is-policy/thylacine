---
id: chg-2026-10-04-async-request-progress
type: chg
title: "Drive private native requests through bounded partial I/O"
date: 2026-10-04
arc: arc-halcyon-interaction
commits: []
touched: [sub-kernel-ninep-client, sub-kernel-ninep-transport]
established: []
closed: []
opened: []
depth: skeletal
---
# AS-2d private request engine prerequisite

Existing p9_client builders and demux now support an exclusive progress cursor:
immutable queued TX, alternating TX/RX, terminal local abort and no blocking
entry through legacy APIs. Unknown/premature replies fail closed. Callbacks may
free RPC storage after the driver detaches its borrows. AS-R6 restores approved
zero-deadline semantics without changing nonzero absolute deadlines.

Actual-source ASan/UBSan, eight client/twelve framing mutations, native shared
fixture in CPU1 boot1830/1830, three ARM64 units and9p_client197states/five buggy
configs pass. Evidence and initial fixture mistakes are retained under
work/oct4-async-service/as2d. Single-agent self-review; four drafts unchanged.
No private Loom activation, new broad/graphics result or completed clipboard.
