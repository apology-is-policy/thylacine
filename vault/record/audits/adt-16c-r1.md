---
id: adt-16c-r1
type: adt
title: "16c round 1 (srvconn transport + attach + pivot)"
date: 2026-05-26
scope: [sub-kernel-ninep-transport, sub-kernel-ninep-attach]
reviewer: opus
model-start: "opus (2026-05 tier; exact id not recorded in the round log)"
model-end: "opus (2026-05 tier; exact id not recorded in the round log)"
verdict: dirty
counts: {p0: 0, p1: 3, p2: 4, p3: 6}
findings: [fnd-16c-r1-f1, fnd-16c-r1-f2, fnd-16c-r1-f3, fnd-16c-r1-f4, fnd-16c-r1-f5, fnd-16c-r1-f6, fnd-16c-r1-f7, fnd-16c-r1-f8, fnd-16c-r1-f9, fnd-16c-r1-f10, fnd-16c-r1-f11, fnd-16c-r1-f13]
round-of: chg-2026-05-26-16c-attach-srv
created: 2026-07-31
---
Commits in scope: b1584c4a (16c-kernel) + 457f22d9 (16c-integration) +
97f569e2 (16c-spinkill). P1+P2 = 7 >= 6 -> DIRTY; R2 mandated with focus
on the deadline-arming interaction, the kernel_attached hoist, and the
dual-destroy. Confidence gaps the prosecutor declared: the
client_handshake_done interaction, the cpio/argv path, Stratum-side code
-- all out of scope. F12 (a joey rename, P3 fixed) is recorded in the
memory roster and backfills with the boot-chain sweep (its surface has no
vault node yet). Close commits: f05bdc5e + fd706b36.
