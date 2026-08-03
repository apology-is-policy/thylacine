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

**None of this is a privilege boundary, and that changes what the notes here
are for.**

These libraries are client code over a frozen ABI. The kernel validates every
argument it is handed, so a bug in a library here corrupts its own caller's
state and nothing else — which is why the dossiers in this area are
`audit: light` where their kernel counterparts are `audit: hard`, and why
their invariant sections say *composes with* rather than *enforces*.

What is worth reading them for instead is the **discipline they apply on their
own initiative**: ownership that closes a descriptor whether or not the caller
remembered, a single error decoding so the kernel's two return conventions are
reconciled in one place, and — the pattern this area keeps producing —
authority held by an operation's *absence* rather than by a check. A library
cannot enforce an invariant it does not implement, but it can decline to offer
the shape that would break one, and that turns out to be the stronger move: a
forgotten check is silent, while a method nobody wrote cannot be called.

The corollary is the failure mode to watch for here. Because nothing in this
plane is load-bearing for the kernel's guarantees, a claim in one of these
libraries can be wrong for a long time without anything failing — the kernel
goes on being right underneath it. Both findings in the first sweep are that
shape: a feature declared impossible for a reason that expired, and an
accessor documenting a derivation its constructors never implemented. Neither
has ever produced a wrong result, because nothing has read them yet.

## Children

- [[sub-libthyla-rs]] — the runtime proper: the prologue that runs before
  `rs_main`, RAII over the handle table, one error type with the bare-sentinel
  ambiguity resolved in one place, the lazy heap, and the two invariants it
  makes unexpressible rather than checking.

## Cross-cutting

- The raw layer below this one — syscall numbers, register convention, the
  mirrored argument records — is [[sub-kernel-syscall-abi]] on the boundary
  plane, because both the kernel and its two userspace mirrors must agree on
  it and nothing in the build checks that they do.
- The ported half of the same job is [[moc-pouch-seam]]. A program is on one
  side or the other, never both: authored within Thylacine means this area,
  ported from elsewhere means the seam.
- Still to arrive with the rest of the userspace sweep: the driver framework
  and the network-device layer (the userspace half of [[inv-i34]]), and the
  transport crypto — which is also the one native consumer whose own bound
  check is load-bearing against the kernel's entropy-read behaviour rather
  than redundant with it.
