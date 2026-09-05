---
id: fnd-net6b-r2-f2
type: fnd
title: "Narrower-live-op under sustained OOM makes no progress"
round: adt-net6b-r2
severity: P3
status: fixed
surface: [sub-kernel-ninep-dev9p-poll]
threatens: []
fixed-by: chg-2026-06-18-net6b4-close
created: 2026-07-31
---
## Prosecution

With a live op narrower than the poller's mask and allocation failing,
the poller spurious-wakes on the uncovering completion and re-parks
forever.

## Disposition

Fixed: folded into the F2 degrade condition (not just a comment) -- the
degrade fires when the live op does not cover this poller's events.
