---
id: chg-2026-09-25-connection-capacity
type: chg
title: "Prepare bounded Halcyon connection admission"
date: 2026-09-25
arc: arc-halcyon-interaction
commits: []
touched: [sub-halcyond-service-wire, sub-libhalcyon, sub-tapestryd]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-25
---
The pure service pool reserves separate 32/2/4 controller/media/handshake quotas,
with one handshake per peer and one controller per authenticated live leaf.
Monotone connection IDs defeat stale release. Retirement preserves quota until
readiness reclamation; handshake timers expire at two seconds. The existing pane
limit has one definition shared by Tapestry and HIN1 controller capacity.

All 591 affected Linux/aarch64 host tests pass, including seven new pool tests.
Three intentional mutants fail the expected duplicate-peer, deadline-equality
and early-release assertions; restored code passes. Evidence and self-review are
in docs/HALCYON-INTERACTION-STATUS.md and work/hi1-pool-evidence/. The module does
not yet admit production connections: live media retains its existing limit two.
Controller/focus admission, complete buffer/kernel ledger and clipboard clients
remain integration work. No new graphical validation or Main landing is claimed.
