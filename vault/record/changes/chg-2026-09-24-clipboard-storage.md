---
id: chg-2026-09-24-clipboard-storage
type: chg
title: "Bounded clipboard storage and deferred publication"
date: 2026-09-24
arc: arc-halcyon-interaction
commits: ["*(pending)*"]
touched: [sub-halcyond]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-24
---
Adds the pure session clipboard store: two staged writes, two immutable read
pins and the current value, bounded to 5 MiB payload. Commit validation and
generation comparison precede a single-use pending admission ticket; publication
moves the preallocated value. Exact owner/session/context checks, monotonic
expiry, identifier exhaustion and cancellation prevent stale completion. Nine
storage lifecycle/bounds tests are included. The broker, endpoint, focus proof
and full connection/metadata ledger remain pending. No runtime clipboard or
independent audit claim; review is single-agent as directed.

Verification: 335/335 Halcyon library tests pass on Linux/aarch64, including nine
storage tests (`work/hi1b-pi-deadline-fixed.log`). A missing font fixture blocked
the first isolated compile; a later deadline test supplied regressing time and
correctly expired a transfer. Both fixture corrections and failed logs are
recorded in the phase status. No guest or integration result is claimed.
