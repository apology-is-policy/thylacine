---
id: spec-loom-sq
type: spec
title: "loom_sq.tla"
models: [sub-kernel-loom]
pins: [inv-i29]
cfgs:
  - "loom_sq.cfg -- clean: TypeOK, FifoFidelity, HeadNeverPassesTail, ClaimExclusive"
  - "loom_sq_liveness.cfg -- AllConsumed: every produced entry is eventually consumed"
  - "loom_sq_buggy_release_before_copy.cfg -- BUGGY_RELEASE_BEFORE_COPY: the head is published before the copy; a refill is consumed in the old position's place (FifoFidelity)"
  - "loom_sq_buggy_unlocked_claim.cfg -- BUGGY_UNLOCKED_CLAIM: two consumers claim one head; a position is consumed twice and the next skipped (FifoFidelity)"
  - "loom_sq_buggy_unlocked_claim_exclusive.cfg -- BUGGY_UNLOCKED_CLAIM: the claim exclusion itself (ClaimExclusive)"
gate: "any change to where a consumer reads the SQ index or copies the SQE relative to publishing sq_head, to the claim lock, or to the producer's reuse rule"
created: 2026-10-12
updated: 2026-10-12
---
## Abstraction

The submission ring as a handoff between one well-behaved producer and the
kernel's consumers. A position's content (its index entry and the entry it names)
is one value; the producer may overwrite a position only once the published head
has passed it. The kernel's private head and its published mirror are one
variable, because the code advances them together under the ring lock and only a
lock holder reads the private one.

Deliberately a separate module from [[spec-loom]]. That model gives every
operation its own slot for its whole life and copies atomically at consume, so a
position handed back and refilled between the publication and the copy is not a
state it can reach. Its hostile mutator is harmless by I-30; the producer here
follows the protocol and is still harmed by a consumer that releases too early.

## What it pins

- **`FifoFidelity`** — the consume sequence is a prefix of the production
  sequence, element for element. One statement covers the three ways the handoff
  can fail: a refill read in the old position's place, a position consumed twice,
  a position skipped. An out-of-range index is consumed as itself (dropped), so
  a refill cannot make it valid after the fact.
- **`HeadNeverPassesTail`** and **`ClaimExclusive`** — the claim lock's job. The
  unlocked-claim cfgs violate both the fidelity and the exclusion, so the second
  consumer is a live dimension, not decoration.
- **`AllConsumed`** (liveness) — nothing strands.

## What it cannot see

A hostile producer that ignores the head. It can damage only its own
submissions, and what the kernel does with whatever it copied is
[[spec-loom]]'s `ArgPinnedToSnapshot`. Byte-level tearing of a copy is the same
case. The memory model is abstracted to the release store on the head and the
acquire on the tail; the code's C11 orders are argued in the dossier, not here.

## Action-site map

- `Produce` — `Ring::try_submit` (`usr/lib/libthyla-rs/src/loom.rs`): acquire
  the head, refuse when full, write the entry and its index, release the tail.
- `Claim` / `Finish` — `kernel/loom.c::loom_drain_sq` under `l->lock`: the index
  read and the copy (or the dropped count), then `loom_sq_release`. The private
  consumer `kernel/loom_service_frontend.c::loom_private_submit` follows the same
  order and publishes through the same `loom_sq_release`, the only writer of the
  published head.
- The buggy order is `loom_drain_sq` through main a7353c36f.
- Runtime witnesses: `loom.sq_release_after_copy`, `_after_index`,
  `_after_copy_chain` (the hook runs inside `loom_sq_release`).
