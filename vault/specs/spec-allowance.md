---
id: spec-allowance
type: spec
title: "allowance.tla"
models: [sub-kernel-allowance, sub-kernel-hwcap]
pins: [inv-i34]
cfgs:
  - "allowance.cfg -- clean (Safety: the four I-34 legs, plus EventuallyResolves, the liveness witness that the re-check gate cannot wedge an in-flight create)"
  - "allowance_buggy_revoke_race.cfg -- buggy: THE central counterexample. The commit installs the handle unconditionally, so a revocation interleaving between the gate check and the install leaves a live handle over an emptied allowance (violates HandlesWithinAllowance)"
  - "allowance_buggy_revoke_leak.cfg -- buggy: removal empties the allowance but fails to drop the driver's already-minted handles -- revoked yet still holding live authority over a gone device (violates RevokedFullyCleared)"
  - "allowance_buggy_confer_widen.cfg -- buggy: a grant exceeding what the granter holds (violates AllowanceWithinConferred)"
  - "allowance_buggy_self_widen.cfg -- buggy: a Proc enlarging its own conferred set"
gate: "Pre-commit re-run for ANY change to the two-step create, the confer gate, or the revoke path (spec-first re-enabled for this surface, user-voted 2026-06-15)."
created: 2026-08-02
updated: 2026-08-02
---
## Abstraction

Resources are opaque set members; a driver holds an allowance (a set) and a
collection of minted handles. Creation is deliberately **two actions, not one**,
because the whole point is what can interleave between them — that split is the
model's reason to exist rather than an implementation detail leaking upward.

Deliberately beneath the model: address arithmetic (window containment, overflow
rejection), the per-buffer size ceiling as a *scalar* rather than a set, the
lock that makes the second step atomic with revocation, and the memory ordering
on the conferred set's publication.

That first omission matters for reading the model honestly: because a resource
is a set member, the transfer axis's real bound — one maximum size, with no
representation for a sum — **has no image here at all**. The gap recorded on
[[inv-i34]] and [[inv-i32]] is invisible to TLC by construction.

## Action-site map

| Spec action | Impl |
|---|---|
| `CreateBegin` | `allowance_permits` — the lock-free gate, sound because the conferred set is immutable |
| `CreateCommit` | `allowance_handle_alloc` — re-checks the revoked flag under the lock the revoke takes, then installs |
| `Confer` | `proc_confer_allowance`, gated by `allowance_confer_within_parent` (narrowing only) |
| `Revoke` | `proc_revoke_allowance`, folded into the group terminate so the gate closing and the authority ending are one event |

## What the model does not carry

`ConferredWithinNode` — that a grant matches the device actually bound — is
checked as an invariant *in the model*, but nothing in the kernel enforces it:
the supervisor computes the grant and the kernel copies it. So one of the four
legs TLC proves is, in the built system, a policy obligation rather than a
mechanism. The model is stronger than the implementation on exactly that leg,
and the implementation's own header says so.
