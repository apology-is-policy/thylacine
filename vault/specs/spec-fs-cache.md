---
id: spec-fs-cache
type: spec
title: "fs_cache.tla"
models: [sub-kernel-ninep-dev9p, sub-kernel-larder]
pins: [inv-i38]
cfgs:
  - "fs_cache.cfg -- clean (Invariants: the Open/Read/OwnWrite discipline over content tokens)"
  - "fs_cache_wb.cfg -- clean (InvariantsWb: the StageWrite/FlushClose write-behind legs; EnableStaging => ~EnableExternalWriter)"
  - "fs_cache_external.cfg -- clean (SafetyCore under an external writer -- the strict-mount envelope)"
  - "fs_cache_liveness.cfg -- clean (staged bytes eventually durable)"
  - "fs_cache_buggy_stale_serve.cfg -- buggy: a hit serving what a fresh RPC would not (the core I-38 counterexample)"
  - "fs_cache_buggy_no_invalidate.cfg -- buggy: OwnWrite without the invalidate"
  - "fs_cache_buggy_skip_staged.cfg -- buggy: a same-priv read missing the staged run (the overlay-miss class)"
  - "fs_cache_buggy_lost_stage.cfg -- buggy: a path out of the priv that neither flushes nor deliberately discards (the lost-write class)"
  - "fs_cache_buggy_populate_unflushed.cfg -- buggy: own-page install decoupled from the err==0 full-land arm (the G1 coupling counterexample)"
  - "fs_cache_buggy_peer_skips_staged.cfg -- buggy: a read through another open serves the cache while one open holds a stage (vote M23's observing legs; audit round 4 of M15, F1)"
  - "fs_cache_buggy_peer_unordered.cfg -- buggy: a write through another open lands ahead of the holder's older stage (vote M23's changing legs)"
gate: "Pre-commit re-run for ANY change to the Larder serve/populate/invalidate discipline or the write-behind staging/flush protocol (spec-first re-enabled for this surface at L1b)."
created: 2026-07-31
updated: 2026-11-15
---
## Abstraction

Files are content tokens with versions; reads/writes move tokens, not
bytes. Opens are named (vote M23): a stage belongs to one open and holds the
token it staged, and the model's `Read` by another open stands for every
observing leg (open, read, fstat, fsync, stat by name). Deliberately beneath
the model: page granularity and alignment,
the byte-exact overlay split, budgets, gen-guard scoping (G4), the
downgrade refinement (G3 — removed from the model sketch as unsound when
ground truth showed Stratum bumps no parent cvers on child mutations),
and the dir-fid cache (fids are identity, not content).

## Action-site map

| Spec action | Impl |
|---|---|
| `Read` | `larder_attr_serve` / `larder_page_serve` / `larder_walk_serve` call sites in dev9p read/stat/walk_attrs |
| `Open` (revalidate) | `dev9p_walk_attrs`'s populate-by-overwrite; `dev9p_open_cached`'s forced-wire query (strict) |
| `OwnWrite` | the per-mutation invalidate sets (write/create/wstat/rename/unlink/OTRUNC) |
| `Refetch` | the gen-guarded installs after each RPC |
| `StageWrite` / `FlushClose` | `wb_write_prepare` / `wb_flush_locked` (+ the G1 own-install on FlushClose) |

The dev9p rows are the POLICY sites; the mechanism they drive —
`larder_attr_serve`/`_install`/`_invalidate`, `larder_page_serve`/
`_install`/`_install_own`, `larder_pages_snapshot` (the Open
linearization for the fidless cached-open) — lives on
[[sub-kernel-larder]], whose gen ring is the impl realization of the
model's ATOMIC Open across the RPC-shaped populate window.
