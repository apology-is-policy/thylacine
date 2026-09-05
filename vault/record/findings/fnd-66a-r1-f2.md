---
id: fnd-66a-r1-f2
type: fnd
title: "#66a F2: the open=connect adoption installed the endpoint's born-'/' name"
round: adt-66a-r1
severity: P3
status: fixed
surface: [sub-kernel-stalk]
threatens: []
fixed-by: chg-2026-06-12-66a-spoor-path
regression: "stalk.path_adopt_transplant (delivered at #66b; NON-VACUOUS — the fixture returns a NAMELESS replacement)"
created: 2026-08-01
---
## Prosecution

The devsrv open=connect adoption arms (stalk + walk-open) clunked the
named quarry and adopted the connection endpoint — born "/" for a
9P-mode conn root, nameless for a byte conn — with NO transplant, so
fd2path on an `open("/srv/corvus")` connection fd reported "/", not
"/srv/corvus", falsifying the attach-seed comment. The one LIVE wart of
the round (cosmetic — inside the I-33 envelope; no resolution or
permission impact).

## Disposition

Fixed: `spoor_path_transplant(opened, quarry)` at BOTH adoption arms —
`opened` is thread-local pre-install, so the write stays within the
set-before-publish discipline; the same mechanism as the already-tested
mount-cross transplant. The owed regression was recorded at this close
and DELIVERED at #66b (`stalk.path_adopt_transplant` covers the
multi-hop arm; the single-hop syscall arm stays the tracked user-VA
harness gap — #66b F1's explicit justification).
