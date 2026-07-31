---
id: fnd-b1-r1-f3
type: fnd
title: "p9_client_init did not explicitly zero the loose/cacheable/wga latches"
round: adt-b1-r1
severity: P3
status: fixed
surface: [sub-kernel-ninep-client]
threatens: [inv-i38]
fixed-by: chg-2026-07-11-b1-loose
regression: ""
created: 2026-07-31
---
## Prosecution

An in-place re-init of a RECYCLED client struct could carry a stale
`loose`/`cacheable`/`wga_unsupported` latch from the prior tenant — a
stale `cacheable=true` on a stream server would admit the caches the
gate exists to refuse. Production paths were KP_ZERO-allocated (safe
today); the hazard was latent for any future re-init caller.

## Disposition

Fixed: the three flags explicitly zeroed in `p9_client_init` — init
functions own their invariants rather than inheriting them from the
allocator's zero-fill.
