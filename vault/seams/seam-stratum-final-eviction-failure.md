---
id: seam-stratum-final-eviction-failure
type: seam
title: "Stratum disconnect can retain a key when the final dirty-buffer drain fails"
status: open
surface: [sub-stratum-session]
opened-by: chg-2026-10-01-session-service-registries
created: 2026-10-01
updated: 2026-10-01
---
## Boundary

D7 gives each ctl connection an independently proven dataset lease. Explicit
final eviction drains dirty buffers under the filesystem exclusive lock before
removing the key; a failed drain preserves the key and explicit lease for retry.
The pre-existing connection destructor remains best-effort: it clears the dying
connection's identity even if final eviction fails. A storage failure at that
point can leave a DEK resident without a live connection lease.

## What closes it

A separately designed teardown/retry policy must give failed final eviction an
owner, bound its lifetime and retained memory, and define recovery without
losing dirty data or falsely reporting prompt key erasure. Injected storage
failures must qualify that policy. Normal logout and the successful drain
regression do not discharge this obligation. D7 introduces no disk-format or
commit-protocol change and does not claim storage-failure recovery.
