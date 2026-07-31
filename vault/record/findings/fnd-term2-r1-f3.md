---
id: fnd-term2-r1-f3
type: fnd
title: "The uncommitted DIAG23 working tree re-added the dead whole-parent scan + a stale header comment"
round: adt-term2-r1
severity: P3
status: fixed
surface: [sub-kernel-larder]
threatens: []
regression: ""
created: 2026-07-31
---
## Prosecution

The instrumented (uncommitted) working tree carried a re-added dead
`invalidate_parent` and a stale header comment alongside the landed
name-specific invalidation — instrument drift that would have confused
the next reader (and diffed dirty against the audited commit).

## Disposition

Fixed by stripping the instrument to clean HEAD before the close — the
committed tree carries neither. Hygiene class; no chg (nothing landed —
the fix was removing what was never meant to land).
