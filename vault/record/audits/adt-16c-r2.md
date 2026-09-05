---
id: adt-16c-r2
type: adt
title: "16c round 2 (on the R1 fixes)"
date: 2026-05-26
scope: [sub-kernel-ninep-transport, sub-kernel-ninep-attach]
reviewer: opus
model-start: "opus (2026-05 tier; exact id not recorded in the round log)"
model-end: "opus (2026-05 tier; exact id not recorded in the round log)"
verdict: clean
counts: {p0: 0, p1: 1, p2: 1, p3: 4}
findings: [fnd-16c-r2-f1, fnd-16c-r2-f2, fnd-16c-r2-f3, fnd-16c-r2-f4, fnd-16c-r2-f5, fnd-16c-r2-f6]
round-of: chg-2026-05-26-16c-attach-srv
prior-round: adt-16c-r1
created: 2026-07-31
---
The dirty-close recursion doing its job: R2 prosecuted the R1 fixes and
found the F1+F2 interaction bug (a stale 5s deadline wedging every
post-attach op after the handshake window) both prosecutor and self-audit
caught independently. CLEAN per threshold (P1+P2 = 2 < 6). Close commits:
bd97a78c + 218feb0c. 16c arc CLOSED at this round.
