---
id: sub-kernel-alternatives
type: sub
parent: moc-kernel-boot
title: "The alternatives patcher — rewriting .text without ever making it writable"
code:
  - arch/arm64/alternatives.c
  - arch/arm64/alternatives.h
  - arch/arm64/atomic_lse.h
audit: hard
guarded-by: []
validated-by: [prose, gate-v80-floor, gate-smp]
locks: []
abis: []
design:
  - "docs/PORTABILITY.md section 4.5"
created: 2026-08-02
updated: 2026-08-02
---
## Purpose

Compile one binary to the oldest supported instruction set, and on boot rewrite
the sites where a newer core has a better instruction. The kernel's atomic
read-modify-write primitives are authored as the multi-instruction
load-exclusive/store-exclusive loop that every supported core can run, each
carrying a single-instruction replacement that is copied over it if the running
CPU implements the newer atomics.

The result is that a hot path costs what it would have cost if the kernel had
been compiled for that specific CPU — no runtime branch, no indirect call, no
function-pointer table.

## Contract

Runs once, from [[sub-kernel-boot-sequence]], after feature detection and after
the allocator and MMU are up, and **strictly before any second CPU exists**.
Idempotent. On a core with the newer atomics, every site is rewritten; on the
oldest supported core, none is. Two counters record how many sites were seen and
how many were patched, which is the direct evidence the pass ran.

**Failure is safe by construction.** The instruction sequence left in place if
patching does not happen is the correct one — the baseline is the *in-place*
form, and the replacement is what gets copied in. A patcher that skips an entry,
misidentifies a feature, or does nothing at all yields a slower kernel, never a
wrong one. This inverts the usual arrangement, where the fast path is in place
and the fallback is the patch.

## Mechanism

**Authoring.** A macro emits the baseline inline where it is used, records a
patch entry in a read-only metadata section, and stashes the replacement in a
second read-only section. Because the primitives are inline functions, each
*expansion* emits its own entry — the table's size is a link-time property of how
widely the primitives are used, not a count of how many primitives exist.

Both offsets in an entry are stored relative to their own location. That is what
makes the table free of relocations and independent of the randomized base: the
entry and its target slide together, so the recorded delta stays correct. The
same technique is used by two other tables in the same read-only region.

The replacement is assembled by temporarily re-permitting the newer instruction
encoding. Assembling it and executing it are separate gates: the assembler is
told the instruction is legal, and the patcher is what decides whether it ever
runs.

**The pass.** All of DAIF is masked for the whole loop. Only one CPU is running,
so masking is the only thing that could otherwise cause a half-rewritten site to
execute — specifically an interrupt handler taking a lock whose exchange is
mid-rewrite. Masking the non-interrupt exceptions too closes a narrower window in
the address-translation helper, where an exception taken between the translate
instruction and the result read would clobber the result register.

Each entry is length-checked — the replacement must fit inside the baseline, and
both must be instruction-aligned — and a violation stops the kernel, because it
is an authoring error in a macro use, not a runtime condition. The replacement
plus no-op padding is assembled in a bounded stack buffer and written as one
region, so a site changes once rather than instruction by instruction. An
unrecognized feature identifier patches nothing.

**The write, and why it does not violate W^X.** The canonical mapping of the
target is read-execute, and the direct-map alias is read-only and non-executable.
Neither is touched. Instead the page's physical address is resolved by hardware
translation, mapped read-write-non-executable at a dedicated scratch address, the
bytes are written through that alias, cache maintenance is performed, and the
alias is torn down. **At no instant is any page both writable and executable** —
not because a window is kept short, but because the two permissions are never
held by the same mapping.

Resolving the address through hardware translation rather than arithmetic is what
makes this independent of the randomized slide and of which alias the caller
happens to hold.

**Cache maintenance.** The write lands in the data cache under the scratch
address; it is cleaned to the point of unification from *that* address, and the
instruction cache is invalidated for the *canonical* address. Two different
virtual addresses, one physical line — which works because the caches are
physically indexed and tagged. Line sizes come from the cache-type register
rather than being assumed.

**Teardown.** The scratch entry is invalidated and its cached translation
dropped, so the next page's mapping cannot be shadowed by the previous one. One
scratch slot is claimed lazily and reused for the whole pass.

## Data structures

A packed twelve-byte table entry: two self-relative offsets, a feature
identifier, and two byte lengths. Size-pinned by a compile-time assertion.

Two counters, and one lazily-claimed scratch mapping slot with its virtual
address.

The primitives themselves are macro-generated inline functions with an explicit
constraint that the outputs cannot share registers with the inputs, because the
inputs must survive the retry loop. Subtraction is implemented as addition of the
negation — there is no single-instruction subtract-and-fetch — with the
documented precondition that the operand is never the most negative integer, and
the routed callers all pass one.

## Concurrency

**Single CPU, all exceptions masked.** Every safety property here rests on that
placement in the sequence, and on nothing else. There is no lock on the scratch
slot, no synchronization on the counters, and no protocol for a concurrent
execution of a site being rewritten — because none can happen.

Secondaries start later, with cold instruction caches, and fetch the already
rewritten bytes.

The primitives this file produces are, of course, the kernel's most
concurrency-critical code; that is a property of the primitives, not the pass.
The two forms are asserted to be per-operation equivalent — same operand width,
same acquire/release semantics — so a site's memory ordering does not depend on
whether it was patched.

## Invariants enforced

- **I-12 (W^X)** — upheld here in its sharpest form, since this is the only place
  in the kernel that deliberately writes to executable memory. The technique is
  a transient read-write, non-executable alias, so no mapping ever carries both
  permissions. Recorded as a claim this area upholds rather than one it owns —
  the enforcement home is the MMU's page-table construction, not yet swept.

## Error paths

Two stops, both for authoring errors: a malformed entry length, and a baseline
longer than the assembly buffer. One more in the mapping helper: a translation
fault on the target, which means a table entry does not point into mapped code —
a build or link error surfacing at boot.

There is no soft-failure path, and there does not need to be one: an entry that
is skipped for any legitimate reason simply leaves the correct baseline running.

## Performance

The point of the exercise. The routed sites are the lock acquire, several
reference counts, and two scheduler rotation counters — the places where a
multi-instruction exclusive loop against contention is measurably worse than one
instruction. The pass itself costs a page mapping and cache maintenance per
patched site, once, at boot.

## Prosecution

- **The write must never go through an executable mapping.** The scratch alias's
  permissions are the whole W^X argument. A change that wrote through the
  canonical address, or that made the canonical address writable "just for the
  window", breaks the invariant regardless of how brief the window is.
- **The pass must stay before the second CPU exists.** Moving it later — or
  introducing any runtime re-patch — invalidates every concurrency argument in
  this file at once, and there is no local check that would notice.
- **Full-DAIF masking, not just interrupts.** The interrupt case protects the
  half-rewritten site; the other exceptions protect the translation result
  register.
- **The baseline stays the in-place form.** If the fast form is ever made the
  in-place one with the slow form as the patch, a patcher failure stops being
  safe.
- **Feature identification must be conservative.** The gate for the newer atomics
  requires a field value of at least two, because the value one is architecturally
  reserved and a future assignment could denote something weaker than the full
  feature. The same field feeds the word published to userspace.
- **Cache maintenance addresses.** The clean is on the scratch alias and the
  invalidate is on the canonical address; swapping them is silently wrong on a
  machine where they differ.
- **The scratch mapping must be invalidated between pages**, or the next page's
  fresh translation is shadowed.

## Seams

- The cross-page path is unexercised — see Caveats.
- [[seam-hwcap-boot-cpu-only]] — the feature word that gates this pass is read
  from the boot CPU only.

## Caveats

**The cross-page loop has never run.** Every currently routed site is
instruction-aligned, at most sixteen bytes, and none lands close enough to a page
boundary to straddle one, so the per-page chunking is correct by reading and
untested by execution. The comment says so. Its first live use will be a
replacement long enough to straddle — and note that in that case the site is
rewritten in *two* separate operations with cache maintenance between them, which
is weaker than the "one change per site" property the authoring file claims.
Harmless while nothing is executing the site; worth knowing before anything is.

**No test can see a memory-ordering mistake in a replacement.** The two
registered tests prove that the expected number of sites were rewritten and that
the primitives compute the right values. A replacement that used the relaxed
form where the acquire-release form was meant would pass both — same value, wrong
ordering. The multi-boot SMP gate is the only instrument that could surface it,
and only probabilistically.

**The two CPU targets each make a different half of the assertions non-vacuous.**
On the development host the newer atomics are present, so the count assertion
takes its "everything was patched" branch and the patched forms are what the value
checks exercise. On the oldest supported core the same assertion takes its
"nothing was patched" branch and the checks exercise the baseline. Neither run
alone proves both halves, which is why [[gate-v80-floor]] is not optional
coverage for this file — it is the only gate that can see a regression in the
unpatched path.

## Provenance

Read from `arch/arm64/alternatives.c` (96 lines), `alternatives.h` (81),
`atomic_lse.h` (107), and the patching helpers in `arch/arm64/mmu.c` in full,
2026-08-02, during the boot sweep. Two registered tests. The routed call sites
were enumerated from the tree: the spin lock's exchange, the reference counts on
three object types, and two scheduler rotation counters.
