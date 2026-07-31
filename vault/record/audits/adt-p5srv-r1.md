---
id: adt-p5srv-r1
type: adt
title: "P5-corvus-srv-impl arc audit (#525)"
date: 2026-05-20
scope: [sub-kernel-devsrv, sub-kernel-srvconn]
reviewer: opus
model-start: "opus (2026-05 tier; exact id not recorded in the round log)"
model-end: "opus (2026-05 tier; exact id not recorded in the round log)"
verdict: clean
counts: {p0: 0, p1: 1, p2: 5, p3: 4}
findings: [fnd-p5srv-r1-f1, fnd-p5srv-r1-f2, fnd-p5srv-r1-f6, fnd-p5srv-r1-f7, fnd-p5srv-r1-f8, fnd-p5srv-r1-f10, fnd-p5srv-r1-f12, fnd-p5srv-r1-f14]
round-of: chg-2026-05-19-srv-birth
created: 2026-07-31
---
## Scope

A single opus prosecutor over the COMPLETE P5-corvus-srv-impl arc —
kernel `/srv` (devsrv + srvconn + handle + syscall + poll + proc),
corvus userspace (the 9P2000.L server + codec), joey orchestration, and
`corvus.tla`. 0 P0 / 1 P1 / 5 P2 / 4 P3 + 4 withdrawn; every fix landed
in the audit-close commit (`232c89b9`). Not dirty by the letter of the
rule at the time; the fixes were localized (deadline arming, asserts,
corvus loop hygiene, a spec extension).

## Out-of-surface findings (recorded here, not as fnd notes — their
surfaces have no vault node yet)

- **F3 [P2, corvus]**: Q11 BadFormat did not tear down corvus's conn —
  fixed with the `tear_down_after_drain` reply-before-EOF flag.
- **F4 [P2, corvus]**: corvus discarded a `t_srv_peer` failure — a
  future admin verb would have read zero-stripes identity; fixed
  (close-and-continue on peer-read failure).
- **F5 [P2, spec]**: `corvus.tla`'s `connections` was append-only —
  reconnect-after-teardown was unmodeled; fixed with `ConnTeardown` +
  the append-only `connections_history` ledger (and the clean cfg's
  state space grew ~3×, triggering the corvus spec-to-code suspension —
  [[spec-corvus]] records the posture).
- **F9 [P3, corvus]**: the `pending_request` accumulator wedged at its
  cap — fixed (clear-then-Rlerror, recoverable).
- **F11 (withdrawn, corvus)**: in_buf growth — bounded by construction.
- **F13 (withdrawn, proc)**: test Procs invisible to
  `proc_caps_by_stripes` — by-design (the `proc_test_link` harness
  exists for tests that need visibility).

## Convergence

Deeply audited: the deadline production path (F1), the KObj_Srv
discriminator across all consumer sites (F2), the ref flows, the spec
gate locations. Declared thinner: corvus's codec arithmetic paths, the
allocator crate, SMP contention (single-threaded era). Posture at close:
511/511 default + UBSan; the 8 buggy corvus cfgs re-verified; the clean
cfg partial-run recorded.
