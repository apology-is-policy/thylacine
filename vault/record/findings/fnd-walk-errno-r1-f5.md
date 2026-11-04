---
id: fnd-walk-errno-r1-f5
type: fnd
round: adt-walk-errno-r1
severity: P3
status: fixed
title: "Lictor answered a later name's miss with an Rlerror"
surface: [sub-lictor]
threatens: []
fixed-by: chg-2026-11-03-walk-errno
regression: "NONE, by reading: no test drives the broker's Twalk"
created: 2026-11-03
---
## Prosecution

9P2000.L says a short Rwalk. dev9p_walk_attrs installs a negative dentry for name[0] on Rlerror(ENOENT), so a server speaking the fused op that copied this shape would poison an existing name; the broker answers ENOSYS to Twalkgetattr, so harmless here. Also .. from the root read ENOENT. Fixed in the broker.
