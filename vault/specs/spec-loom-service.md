---
id: spec-loom-service
type: spec
title: "loom_service.tla: private retirement without peer progress"
models: [sub-kernel-loom]
pins: [inv-i29, inv-i30, inv-i32]
cfgs:
  - "loom_service.cfg: 5828 states; safety and retirement liveness"
  - "loom_service_buggy_earlyfree.cfg: NoEarlyFree"
  - "loom_service_buggy_late.cfg: NoLateSuccess"
  - "loom_service_buggy_double.cfg: NoDoubleTerminal"
  - "loom_service_buggy_reuse.cfg: NoStaleSlot"
  - "loom_service_buggy_cqfull.cfg: CqBounded"
  - "loom_service_buggy_refund.cfg: CreditsRetained"
  - "loom_service_buggy_peerwait.cfg: RetirementProgress"
gate: "private-service admission, cancellation, slot reuse or retirement changes"
created: 2026-10-04
updated: 2026-10-04
---
One slot, two incarnations, connect/data requests and a one-entry CQ distinguish
terminal-result commit from delivery. Local borrows/pins and the peer endpoint
are separate holders. Abort closes admission; retained peer ownership keeps
credits charged. Peer closure has no fairness assumption. Local cleanup and CQ
drain are weakly fair. Ring closure discards delivery obligations, not borrows.

`specs/check-loom-service.py` checks the clean5,828-state fingerprint and seven
named counterexamples, including temporal retirement independent of peer work.
State directories are temporary. Logs can be retained through --logs.

Blind to actual protocol bytes, usercopy, authority/DAC checks, locks and C
pointer lifetimes. AS-0 is model-before-runtime: the action-to-code binding is
owed by AS-1/2 rather than asserted from abstract success.
