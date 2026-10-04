---
id: chg-2026-10-04-private-protocol-storage
type: chg
title: "Accept owner-provided private protocol buffers"
date: 2026-10-04
arc: arc-halcyon-interaction
commits: []
touched: [sub-kernel-ninep-client]
established: []
closed: []
opened: []
mirrors-checked:
  - "kernel/include/thylacine/9p_client.h: internal client ownership only; 9P and syscall wire unchanged"
depth: skeletal
---
The preallocated initializer avoids hidden bulk TX allocation; destroy distinguishes
external from heap-owned storage. Actual-source13 mutants and sanitizer fixture,
existing9p_client model/five counterexamples and native1830/1830 pass. Charges,
private ring lifetime/retirement and broad qualification remain; no activation.
