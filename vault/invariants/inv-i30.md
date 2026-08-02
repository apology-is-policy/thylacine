---
id: inv-i30
type: inv
title: "I-30 — decide on a private copy: the submit-time pin and the ring TOCTOU"
number: I-30
guards: [sub-kernel-loom, sub-kernel-weft]
validated-by: [spec-loom, spec-weft, gate-smp]
strength: spec
created: 2026-08-02
updated: 2026-08-02
---
## Statement

When a request or its payload lives in memory another party can write, the
kernel decides against a private copy:

1. **Copy before checking.** Every field the kernel acts on is copied into kernel
   memory before it is validated, and the shared slot is never re-read
   afterwards.
2. **Resolve and pin at submit.** An operation naming an object resolves it once,
   takes its own reference, and snapshots the rights it is admitted under. The
   object and those rights are what the work runs against — never re-consulted at
   completion.
3. **A shared word may bound the work; it may never locate the memory.** Indices
   come from kernel-private counters and kernel-private masks.

## Why it is stated this way

Clause 2 is the io_uring credential-versus-work vulnerability class. The bug
shape is: an operation is admitted under one set of credentials, runs
asynchronously, and at completion the kernel looks the object up *again* — by
which time the submitter has replaced it. The work then acts on an object it was
never authorized for. Pinning at submit makes that unreachable rather than
unlikely.

Clause 3 is the sharper one because its violation is not a policy error but an
arbitrary kernel write. A completion index computed as `header.tail &
header.mask`, with both words in the caller's own mapped region, lands wherever
the caller chooses. Computed from private state and a private mask validated as a
power of two at creation, it is always inside the array. The distinction is
exactly this: *bounding* work with a hostile value costs the caller nothing but
its own throughput; *locating* memory with one is an exploit.

That is what makes the advisory reads safe. The submission tail and the
completion head are read from shared memory and acted on — but a hostile
completion head only lets a Proc overwrite its own unreaped result, in its own
region, or wait for the wrong thing.

## Enforcement

**The mirror discipline.** Each shared control block has a kernel-private twin
that is authoritative, and the shared copy is published for the other side to
read. The submission head, completion tail, entry-count masks and the whole
descriptor-ring geometry all work this way. The geometry words are written once
at creation and *never read back*.

**The one exception is bounded.** A submission ring holds indirection slots that
name entries — a caller-written word that genuinely reaches an index. It is
range-checked against the private entry count before it indexes anything, and a
bad value increments a counter instead. It is the exception that shows the rule
is deliberate.

**Snapshots outlive their checks.** An operation whose wire encoding needs more
fields than its resolved state carries keeps the whole copied entry, so a builder
running much later still decodes from the snapshot rather than from a slot the
caller may have rewritten.

**Pins are plural and balanced.** A buffer-backed operation pins its handle and
its buffer; a two-fid operation pins a third object and additionally requires
both fids in one session. Each releases exactly once, on every path — including
every rung of a failure ladder.

**The registered buffer's kernel address comes from the backing region's
direct-map base**, not the caller's virtual address, so the pin stays valid
after the caller unmaps its own view. That relies on the region being one
physically contiguous chunk, which is a property of its type — and widening that
type gate without making the address computation walk chunks yields a wrong
kernel address with no tripwire.

**On the payload side** the same discipline covers descriptors: copied out,
validated with the extent sum computed wide so a hostile pair cannot wrap back
in-bounds, and only validated snapshots reach the consumer. A descriptor's offset
is payload-relative, so it cannot address the header or the descriptor array
however it is crafted.

## Validation

[[spec-loom]] pins both halves as `ArgPinnedToSnapshot` and
`ActedUnderAdmittedRights`, each with a buggy configuration — the re-read, and
the re-resolve at completion. [[spec-weft]] pins the payload-descriptor form as
`DescPinnedToSnapshot` and `ActedDescValidated`, with the ring-TOCTOU
counterexample. [[gate-smp]] is the empirical backstop.

**blind-to:** the models reason about *whether* a value is re-read, not about the
memory ordering of the acquire loads that make a producer's publish visible
before its data. That rests on the documented atomics contract, as it does
everywhere else in the tree.

Nothing checks the discipline mechanically. A new shared field read on a hot path
is an ordinary-looking line of code; what keeps it out is that every such read in
these files carries a comment saying why it is advisory. That is a convention,
not a gate.
