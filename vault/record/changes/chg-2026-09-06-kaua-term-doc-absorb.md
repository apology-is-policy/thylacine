---
id: chg-2026-09-06-kaua-term-doc-absorb
type: chg
title: "absorb docs/reference/152-kaua-term (the per-tile terminal + record stream): clean redirect to sub-kaua-term + companions"
date: 2026-09-06
arc: arc-vault
commits: ["PENDING"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The crash-isolated per-tile terminal (KT-1, an audit-trigger surface). A dedicated
dossier (sub-kaua-term, audit:hard, updated 2026-09-06 -- the same day, from the
same KT-1 work) LAPS the doc; verified the subtle atoms atom-by-atom.

ALREADY COVERED (spot-checked, not assumed):
- The per-record-CLASS bound ("the bounds are per record CLASS, not per read --
  this is the security core", sub-kaua-term:77; the feed_into sink triggering on
  cells_in after EVERY boundary; the alt-screen-toggle amplifier round-3 F1).
- The span serial (the 17-byte wire cell carrying the OSC-1936 serial; a dropped/
  oversize Beacon frame can never shift cells onto the wrong span = the anti-
  clickjack property, sub-kaua-term:51-53).
- The resize ordering (drain_pending rows-only before resized's full diff; the
  80x24->96x20 equal-cell-count case, :103-110).
- The bounds (32 MiB heap, MAX_TITLE, 200x1ms master-write back-pressure, :124/137)
  + the concurrency (master-write futex mutex, lock-free reads, 2 relaxed atomics).
- The --beacon tier advertisement (write to the tile's own /env/BEACON before the
  slave spawn; env_beacon_tier reads it, :56-61).

Zero-fold. Companions: sub-lib-vt (parser), sub-ptyhold (pts), sub-halcyond
(consumer), sub-utopia-interactive (env_beacon_tier). The adt-kt1-r1/r2 audit
records are in the vault. Redirect stub.
