---
id: sub-kernel-dtb
type: sub
parent: moc-kernel-boot
title: "The device tree — where the hardware view comes from"
code:
  - lib/dtb.c
  - kernel/include/thylacine/dtb.h
audit: hard
guarded-by: [inv-i15]
validated-by: [prose, gate-smp]
locks: []
abis: []
design:
  - "docs/ARCHITECTURE.md section 5"
created: 2026-08-02
updated: 2026-08-02
---
## Purpose

Parse the flattened device tree the bootloader placed in RAM, and answer every
question the rest of the kernel asks about the machine: where RAM is, where the
console is, how many CPUs there are and how to start them, where the interrupt
controller and the timer live, how PCI interrupts are routed, and what entropy
the firmware left behind.

This file is the sole source of hardware knowledge — [[inv-i15]] in one
sentence. Nothing else in the tree is permitted to know a hardware address by
having it compiled in, with two argued exceptions the invariant names.

## Contract

Initialized once from the physical address the bootloader passed, before anything
else in boot. Every accessor is read-only, returns a boolean or a sentinel for
absent, and never faults on a tree that lacks what was asked for — a missing
property is an ordinary answer, not an error, because the same kernel binary must
boot on machines that differ.

There is a second, later contract: after relocation, the parser reads from a
kernel-owned buffer rather than the bootloader's memory, and the original blob's
mapping may be withdrawn.

## Mechanism

**Reads are constrained to four aligned bytes, through `volatile`.** Before the
MMU, kernel data accesses are Device-nGnRnE, which mandates natural alignment for
the access width; the structure block is only guaranteed four-byte aligned. Clang
was observed fusing two adjacent four-byte reads into one eight-byte load, which
faults. The `volatile` is what prevents the fusion. This is the second place in
the boot path where a correct compiler optimization is wrong because the machine
is not the one being modelled.

**The walker is a token loop**, and the entry points are built on it in two
distinct styles, for a reason that was learned the hard way.

The simple style tracks "am I inside the node I want" with a flag and reads the
property when it appears. It works for nodes matched by *name* — memory, chosen,
the power-management node — because the name arrives at the node's opening token,
before any property.

The other style cannot use a flag, because **property order within a node is not
guaranteed**. A node's `compatible` property may appear *after* its `reg`, and it
does for the console on the reference platform. So node-matching lookups keep a
per-depth accumulator stack, collect both the match and the payload wherever they
appear, and emit at the node's closing token. The comment records the earlier
single-flag version as the bug this replaced.

**Depth is capped**, and exceeding it degrades to not-found rather than
misbehaving: the stack index advances unconditionally but every access is guarded,
so a node nested deeper than the cap is simply never matched, and a parent's
accumulators are not disturbed by descending past it.

**Relocation to a kernel buffer.** The blob is copied to buddy-allocated pages
once the allocator exists, and the parser switches to reading through the kernel
direct map. This is what allows the identity map to be retired later in the boot
sequence. The comment is honest that reading the original through the direct map
*before* this point empirically fails, and attributes it to cache state inherited
from the pre-MMU window — an observation, not a proof.

**The tree-walk API** is a separate, later surface: offset-addressed node and
property accessors that let a device expose the tree to userspace. Its discipline
is different from the boot accessors' — every caller-supplied offset is
bounds-checked against the block size before a pointer is formed, and name scans
are length-bounded, so a forged or stale offset from userspace is rejected rather
than followed.

## Data structures

One file-scope record: readiness, the original base, the block offsets and sizes
taken from the header, and — after relocation — kernel virtual addresses for the
structure and strings blocks. A single cached CPU count, computed on first ask.

The walker itself is three fields: a cursor, an end, and a depth. The
accumulator stacks are per-call locals sized to the depth cap.

## Concurrency

None by construction. Initialization and relocation both happen on the boot CPU
before secondaries exist; everything afterwards is read-only. The cached CPU
count is written once during that window.

## Invariants enforced

- **[[inv-i15]]** — every hardware address, interrupt number, CPU identity and
  capability the kernel acts on is derived from the tree by this file.

## Error paths

Uniformly soft. An absent tree, a bad magic, an unsupported version, a missing
node, a missing property, a property too short to hold what was asked for — all
return false or zero, and callers degrade. The boot survives a tree that
describes less than expected; the banner reports which lookups fell back.

The one hard failure is relocation running out of memory, which the sequence
treats as fatal because the identity map is about to be withdrawn.

## Performance

Every lookup is a full walk of the structure block; there is no index. On a tree
of a few hundred tokens, called a few dozen times during boot, this is
immaterial, and the simplicity buys the absence of a cache that could go stale
across the relocation.

## Prosecution

- **Property order independence.** Any new node-matching lookup must accumulate
  and emit at the closing token, not act on first sight. The flag style is
  correct only for name matches.
- **Alignment.** Every read of blob bytes must go through the four-byte
  `volatile` accessor. A widened or fused access is a fault before the MMU and a
  silent misread of a misaligned field after it.
- **Bounds on the userspace-facing surface.** The tree-walk accessors are reached
  with an offset that crossed a trust boundary; each must validate before forming
  a pointer, and name scans must be length-bounded.
- **Cap behaviour.** Exceeding the depth cap must stay a clean not-found. The
  guards are on every stack access, not on the index advance.
- **Relocation ordering.** The copy must happen after the allocator and before
  the identity map is retired, and both halves of that sandwich live in another
  file.

## Seams

- [[seam-dtb-blob-internally-trusted]] — the parser validates the header's magic
  and version, and validates caller-supplied offsets, but takes the blob's own
  internal offsets and lengths at face value.

## Caveats

**The depth cap's comment says it would panic; it does not.** The stated
behaviour on an over-deep tree is "we'd panic, not silently corrupt". The safety
half is right — there is no out-of-bounds access — but the mechanism is a silent
degradation to not-found. Arguably the better behaviour of the two; the comment
simply describes a different one.

**The two userspace-facing accessors validate different things.** The property
accessor checks that the name offset lies within the strings block and that the
name is terminated inside it. The iterator returns a name pointer formed from the
same untrusted offset with neither check, then measures it with an unbounded
scan. Both are reachable from the same device. The asymmetry is invisible today
because the blob is firmware-supplied; it is the concrete shape of
[[seam-dtb-blob-internally-trusted]].

## Provenance

Read from `lib/dtb.c` (1205 lines) in full, 2026-08-02, during the boot sweep.
Three registered tests: the presence of a chosen-node seed, PCI interrupt
routing, and the PCI memory window. The parser's structural behaviours — property
ordering, the depth cap, relocation — have no direct test and are covered by the
boot, which consults this file for every hardware fact it prints.
