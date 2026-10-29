---
id: spec-loom-service-buffers
type: spec
title: "loom_service_buffers.tla: payload return is independent of CQ acknowledgement"
models: [sub-kernel-loom, sub-kernel-loom-pools]
pins: [inv-i29, inv-i30, inv-i32]
cfgs:
  - "check-loom-service-buffers.py clean-one: 464 states; one member/two leases/two generations"
  - "check-loom-service-buffers.py clean-two: 6416 states; two members/three leases/two generations"
  - "ackfree/recycle: PayloadIntact"
  - "stale: ExactReturn"
  - "pair: PairedPublication"
  - "cqfull: CqBounded"
  - "late: NoLateSuccess"
  - "order: MoreBeforeFinal"
  - "double: NoDuplicate"
  - "peerwait/returnwait/cqwait: RetirementProgress"
gate: "private pooled READ, payload return, receipt publication or scope retirement changes"
created: 2026-10-04
updated: 2026-10-04
---
The focused model distinguishes AVAILABLE/BUSY/PENDING/LEASED, successful reply
commit, paired CQ publication, consumer acknowledgement and explicit payload
return. Nonces represent distinguishable payload bytes. A one-entry CQ wraps
while leases persist; the pool may be re-created only after complete release.
Finite nonce exhaustion stops further admissions rather than wrapping.

Only local Stop/Finalize/Retire actions are weakly fair. Peer replies/closure,
CQ delivery/acknowledgement and payload return have NO fairness premise. Thus
source retirement is checked independently of both kinds of consumer progress.
Pending successful completions remain pool-owned across local retirement.
The eleven deliberately broken variants fail their named properties, including
three temporal counterexamples for introducing a peer/CQ/return dependency.

Run specs/check-loom-service-buffers.py with --logs while holding a host lease.
It pins two clean state fingerprints, checks named mutant verdicts, bounds each
run to120seconds/768MiB and removes transient TLC trees even on failure.
Actual evidence: work/oct4-async-service/buffer-pools/model-passed.json.

One source stream and one pool, no physical pointers, weak memory, parser bytes,
DAC, alias/range validation, simultaneous streams or whole-ring destruction.
Raw user mutation and forged receipts remain kernel tests. Pool re-creation is
represented; independent slot-versus-lease validation needs actual-source tests.
There is no throughput/fairness claim among multiple streams, and no theorem
about unbounded configurations. No private runtime binding exists yet; AS-2 must
map the model actions to its implementation before activation. Existing Loom
core/service models are preserved; their CQ fairness cannot substitute for this
model's independent payload-retirement result.
