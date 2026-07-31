---
id: adt-stalk3b-r1
type: adt
title: "stalk-3b-beta round 1 (open=connect + the 9P-unification)"
date: 2026-06-03
scope: [sub-kernel-devsrv, sub-kernel-srvconn, sub-kernel-ninep-attach]
reviewer: opus
model-start: "opus (2026-06 tier; exact id not recorded in the round log)"
model-end: "opus (2026-06 tier; exact id not recorded in the round log)"
verdict: clean
counts: {p0: 0, p1: 0, p2: 1, p3: 2}
findings: [fnd-stalk3b-r1-f1, fnd-stalk3b-r1-f2, fnd-stalk3b-r1-f3]
round-of: chg-2026-06-03-stalk3b-open-connect
created: 2026-07-31
---
## Scope

The five stalk-3b-beta commits: the STALK_OPEN replace-adopt, the
`devsrv_open_connect` core, the `SYS_ATTACH_9P_SRV` retarget, the client
migrations, the embedded-client retirement + per-Proc-cap removal. Opus
prosecutor + an in-session self-audit CONVERGED on the same SOUND set —
the prosecutor additionally found F1 (the missing `kernel_attached`
guard on the retargeted CSRVCLIENT I/O branches), which the self-audit
missed: the two-prosecutor cross-coverage working as intended.

## Convergence

SOUND set: the open=connect refcount dance across every branch; the
attach-helper failure-path ref discipline (two unreachable-by-
construction defensive branches noted); `srvconn_unref` post-retirement
(no stale members); the kernel_attached gate at both close sites; the
byte/9P fail-closed split; the `dc='s'` dup guard; corvus's serve-loop
`nfds-1` fix; the /srv-survives-pivot trace; the struct-Proc layout.
Standing coverage gap recorded: no 9p-mode connect unit test
([[seam-srv-9p-connect-unit]]). Matrix 709/709 × 3 configs at close.
