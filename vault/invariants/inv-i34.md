---
id: inv-i34
type: inv
title: "I-34 — a driver's hardware authority is exactly what it was granted, and no more"
number: I-34
guards: [sub-kernel-allowance, sub-kernel-hwcap, sub-kernel-discovery]
validated-by: [spec-allowance, prose, gate-smp]
strength: spec
created: 2026-08-02
updated: 2026-08-02
---
## Statement

A Proc may be narrowed to an **allowance**: a fixed set of physical address
windows, interrupt numbers, bus functions, and a per-buffer transfer ceiling.
While narrowed, it can mint a hardware handle only over something in that set.
The set never widens, a fork inherits it unchanged, and revoking it closes the
gate for good.

## Why it is stated this way

The capability to create hardware handles is a single bit. Held, it lets a Proc
claim *any* unreserved register range in the machine. That is the right shape
for the boot chain, which owns everything, and the wrong shape for a driver,
which owns one device — so the allowance is the second axis: the capability
says *may you*, the allowance says *over what*.

It is the hardware-side analogue of the rule that a fork can only ever hold
fewer capabilities than its parent, and of the scope that bounds an elevated
identity. Same idea, different currency.

## Enforcement

**Absence means unlimited.** A Proc with no allowance is *broad* — bounded only
by the create capability and by the ranges the kernel reserved for itself
([[inv-i5]]). This is the as-built path: the boot chain and the trusted servers
run broad, and narrowing is something a supervisor does to a driver it spawns.
So the invariant is opt-in, and its blast radius is exactly the set of Procs
someone chose to confine.

**Creation is two steps, because revocation races it.** The gate is checked
once without a lock — the conferred set is immutable, so reading it needs none —
and then **re-checked while installing the handle, under the same lock the
revoke takes**. A device removed concurrently with a create in flight therefore
cannot leak a handle through a gate that was open when the request started and
closed before it finished. The losing create aborts rather than installing.
This is the invariant's central hazard and the reason the two-step exists.

**The set is immutable once conferred**, which is what makes the lock-free read
sound: the only field that ever changes is the revoked flag. Conferring is
itself gated — a supervisor may confer only what it holds, checked resource by
resource through the same predicate the gate uses — so a confer is always a
narrowing and never a widening, including from an already-narrowed parent.

**A narrowed Proc cannot have children.** Drivers are leaves by construction:
the fork path refuses a narrowed parent a child, so there is no way to produce a
hardware-capable grandchild whose allowance would survive a revoke aimed at the
parent's thread group. Where inheritance *is* possible the copy is equally
narrow, and a child forked after its parent was revoked is born revoked.

**Revocation is folded into termination.** Removing a device revokes and then
kills, atomically from the caller's perspective, and the death cascade drops the
live handles at reap. So the gate closing and the existing authority ending are
one event, not two.

**Three of the four legs are kernel-enforced; the fourth is not.** The kernel
guarantees that handles stay within the allowance, that the allowance stays
within what was conferred, and that revocation clears everything. It does *not*
check that what was conferred corresponds to the device the driver was actually
bound to — that grant is computed by the supervisor, and the kernel copies
whatever it is handed. **The invariant is three-quarters a kernel property and
one-quarter a policy one**, and the code says so in its own header rather than
implying otherwise.

## Validation

The formal model is [[spec-allowance]], with counterexample configurations for
the four ways it can fail: the revoke-versus-create race, a revoke that leaves
authority behind, a confer that widens, and a Proc widening its own set.
At runtime the evidence is a create that aborts because a revoke won the race,
plus the fork-refusal and membership checks. The lock order it introduces —
process table above the allowance, allowance above the handle table — is
acyclic because nothing takes them the other way round.

**blind-to:** the fourth leg entirely. If the supervisor computes a grant
larger than the device it bound, every kernel check passes and the driver is
legitimately over-authorized. Nothing in the kernel can detect this, by design.

**blind-to:** the transfer ceiling is per-buffer, not cumulative. The data model
has a single maximum size, so there is nowhere in it to express a *sum* — which
is why the corresponding gap in the per-Proc resource floor ([[inv-i32]]) is
structural here rather than an oversight there. A narrowed driver's total
device memory is bounded only by how many buffers it is willing to allocate.
