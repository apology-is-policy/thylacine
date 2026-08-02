---
id: spec-weft
type: spec
title: "weft.tla"
models: [sub-kernel-weft]
pins: [inv-i37, inv-i30]
cfgs:
  - "weft.cfg -- clean: the thirteen-conjunct safety set (1412 distinct states, depth 22)"
  - "weft_liveness.cfg -- EventuallyReleased: an in-flight page always eventually drops its pin"
  - "weft_buggy_premature_release.cfg -- BUGGY_PREMATURE_RELEASE: drop the pin at operation-terminal (PinHeldWhileInFlight)"
  - "weft_buggy_recheck_per_op.cfg -- BUGGY_RECHECK_PER_OP: mediate each operation (NoPerOpMediation)"
  - "weft_buggy_ring_toctou.cfg -- BUGGY_RING_TOCTOU: act on the shared slot rather than the snapshot (DescPinnedToSnapshot)"
  - "weft_buggy_share_outlives_flow.cfg -- BUGGY_SHARE_OUTLIVES_FLOW: the mapping survives the flow (ShareBoundedByFlow)"
gate: "any change to the pin lifetime, the descriptor snapshot, the share teardown, or a move toward per-operation mediation"
created: 2026-08-02
updated: 2026-08-02
---
## Abstraction

A flow, a shared region, a descriptor ring, and a send whose page stays live
after the operation completes. Written before the implementation and gating it.

The unusual thing this model proves is a **negative**: that no per-operation
check happens. Most invariants say a check is performed; `NoPerOpMediation` says
one is *not*, because the mediation is the cost the whole design exists to
remove. Putting that in the safety set — with a buggy configuration where a
reviewer's instinctive "just re-check each packet" is the counterexample — makes
the absence a property rather than an oversight.

## What it pins

- **`PinHeldWhileInFlight` and `NoInFlightReuse`** — the buffer lifetime. A
  zero-copy send completing means "queued", not "done"; the page may still be
  read by the device and, for a reliable stream, until the peer acknowledges.
  Releasing at operation-terminal is the io_uring buffer-notification
  use-after-free, and `weft_buggy_premature_release` is that bug, executable.
- **`NoPerOpMediation`** — the authority is established at grant and not
  re-established per operation.
- **`DescPinnedToSnapshot` and `ActedDescValidated`** — [[inv-i30]]'s discipline
  applied to the payload descriptor: copy it out, validate the copy, act on the
  copy. The guest may rewrite its slot the instant after posting it, and the
  snapshot is the only reason that is harmless.
- **`ShareBoundedByFlow` and `NoStaleShareAccess`** — the mapping does not
  outlive the flow, and a claim after teardown fails closed. The first of those
  is the cross-Proc dual-refcount argument restated as a temporal property.

## What it cannot see

**The most important gap is that the buffer-lifetime clause is currently
vacuous in the implementation.** The holder-set mechanism exists, is unit-tested,
and matches the model — but nothing arms it, because the network daemon copies
the ring into its own socket buffer, so no page is ever in flight past its reply.
So the property holds by *avoidance*, not by the modelled defence. See
[[seam-f-notif-unwired]]. The model is a correct description of a mechanism
waiting for its caller.

The delivery mechanism is abstracted away as initialization. That was checked
deliberately when the delivery design was chosen: mapping-at-grant realizes the
model's initial state without introducing a new invariant-bearing step, so the
model written model-first still stands and did not need reopening.

Below the model: the framebuffer kind and its admission gate, the orphan
reaper, the readiness poke (its own module, [[spec-weft-readiness]]), and the
per-Proc budget that bounds a client's pin.

## Binding

`specs/SPEC-TO-CODE.md::weft.tla`. Consume ↔ the drain's per-slot copy and
bounds validation; the pin ↔ the registration reference transferred at claim;
holder-release ↔ the holder-set clear returning its exactly-once release; the
share teardown ↔ the registry removal before the reference drop.
