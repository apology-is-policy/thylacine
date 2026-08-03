---
id: moc-userspace-runtime
type: moc
title: "The runtime libraries — what a native program stands on"
parent: moc-userspace
created: 2026-08-03
updated: 2026-08-03
---
The libraries a native Thylacine program links rather than the programs
themselves: the runtime that turns a syscall into a typed call and a
descriptor into an owned value, the driver framework, the network-device
layer, and the transport crypto. Every program on the native side of the
ARCHITECTURE.md section 3.5 split stands on at least the first of these.

## The organizing fact

**Almost none of this is a privilege boundary, and that changes what the notes
here are for — but the exception is real, and which dossier you are in decides
whether the rest of this section applies.**

Most of these libraries are client code over a frozen ABI. The kernel validates
every argument it is handed, so a bug in one of them corrupts its own caller's
state and nothing else — which is why those dossiers are `audit: light` where
their kernel counterparts are `audit: hard`, and why their invariant sections
say *composes with* rather than *enforces*.

### The exception: a library that COMPUTES an authority

[[sub-libdriver-grant]] is not client code over a validated ABI. It computes
the hardware allowance the kernel will then enforce, and the kernel **never
re-derives it**. The I-34 machinery checks a conferred allowance against the
*conferrer's* own — and the Proc that computes grants, the warden, holds a
BROAD allowance for which that check passes unconditionally. So the kernel
faithfully enforces whatever it is handed, and never asks whether that
allowance describes the driver's own device. It cannot: it does not know which
node the bind chose.

That leaves one function in this area as the sole establishment of a real
invariant's correspondence half, where a defect moves a hardware boundary
rather than corrupting a caller. It files `audit: hard`.

The generalization worth carrying past this area: **the "not a privilege
boundary" argument holds for a library that only ever asks, and fails for one
that decides.**

What is worth reading them for instead is the **discipline they apply on their
own initiative**: ownership that closes a descriptor whether or not the caller
remembered, a single error decoding so the kernel's two return conventions are
reconciled in one place, and — the pattern this area keeps producing —
authority held by an operation's *absence* rather than by a check. A library
cannot enforce an invariant it does not implement, but it can decline to offer
the shape that would break one, and that turns out to be the stronger move: a
forgotten check is silent, while a method nobody wrote cannot be called.

The corollary is the failure mode to watch for on the client-code side. Because
nothing there is load-bearing for the kernel's guarantees, a claim can be wrong
for a long time without anything failing — the kernel goes on being right
underneath it. Both findings in the first sweep are that shape: a feature
declared impossible for a reason that expired, and an accessor documenting a
derivation its constructors never implemented. Neither has ever produced a
wrong result, because nothing has read them yet.

The grant core's findings are the same shape and a different stake: a version
field nothing compares, and three caps that mirror the kernel's by prose alone.
Both are quiet in the same way — but here the thing going wrong quietly is the
computation of an authority, so the honest reading is "fail-closed in
direction", not "harmless".

## Children

- [[sub-libthyla-rs]] — the runtime proper: the prologue that runs before
  `rs_main`, RAII over the handle table, one error type with the bare-sentinel
  ambiguity resolved in one place, the lazy heap, and the two invariants it
  makes unexpressible rather than checking.
- [[sub-libdriver-grant]] — the driver framework's authority core: the manifest
  schema, the node-intersect-needs computation that produces a grant, the
  spawn-descriptor codec, and the runtime a driver is written against. One
  grant computed once and consumed twice — the descriptor informs, the
  allowance authorizes — and the two deliberately disagree by exactly one page.
  The area's exception above.

## Cross-cutting

- The raw layer below this one — syscall numbers, register convention, the
  mirrored argument records — is [[sub-kernel-syscall-abi]] on the boundary
  plane, because both the kernel and its two userspace mirrors must agree on
  it and nothing in the build checks that they do.
- The ported half of the same job is [[moc-pouch-seam]]. A program is on one
  side or the other, never both: authored within Thylacine means this area,
  ported from elsewhere means the seam.
- Still to arrive with the rest of the userspace sweep: the driver framework's
  *other* half — discovery, supervision and the readiness protocol, which is
  where a node comes from and what happens when a driver dies — plus the
  network-device layer, and the transport crypto, which is the one native
  consumer whose own bound check is load-bearing against the kernel's
  entropy-read behaviour rather than redundant with it. The authority half of
  [[inv-i34]] has landed above; its grantor, the warden, is a program and
  sweeps with the boot chain.
