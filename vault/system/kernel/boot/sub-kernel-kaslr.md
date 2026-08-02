---
id: sub-kernel-kaslr
type: sub
parent: moc-kernel-boot
title: "KASLR — choosing the kernel base, and the addresses that are not yet true"
code:
  - arch/arm64/kaslr.c
  - arch/arm64/kaslr.h
audit: hard
guarded-by: [inv-i16]
validated-by: [prose, gate-smp]
locks: []
abis: []
design:
  - "docs/ARCHITECTURE.md section 5.3"
created: 2026-08-02
updated: 2026-08-02
---
## Purpose

Pick a randomized offset for the kernel's high virtual base, apply any
relocations that offset invalidates, and hand the result back to the stub so the
MMU can be programmed and the long branch taken. It also seeds the stack-canary
cookie, because this is the earliest point where entropy exists.

## Contract

Called once, from [[sub-kernel-boot-entry]], with the MMU off and the program
counter at the load physical address. Returns the chosen slide. After it returns,
the accessors reporting the offset, the seed source, the runtime high base, and
the cached load-PA bounds are all valid, and the canary cookie is live.

The slide is **always non-zero** and always 8 MiB-aligned. Non-zero is a
contract, not an accident: if the entropy mixes to zero in the masked bits the
result is forced to the minimum alignment rather than left at zero, so there is
no input under which the kernel runs unslid.

## Mechanism

**Entropy, in priority order.** A firmware-supplied dedicated seed is preferred;
then a general random seed, which is what QEMU publishes; then the architectural
counter as a last resort. Which one was used is recorded and printed, because the
counter path is materially weaker — a few milliseconds of boot variance in the
low bits — and a developer needs to be able to tell at a glance whether the
randomization is real.

**Mixing.** A standard multiply-xorshift avalanche spreads structured low-bit
entropy across all 64 bits. It matters most on the counter path, where the raw
value's high bits are nearly constant across boots. The mix is exposed for
testing precisely because it is the one part of this file that is a pure function
— everything else is entangled with the machine state.

**The slide.** Masking preserves eleven bits at 8 MiB alignment, giving 2048
distinct positions inside a 16 GiB window. That is deliberately narrower than the
address space allows. The alignment has been widened twice, each time because a
sanitizer build's image outgrew the fixed page-grain mapping, and each widening
costs a bit of entropy — the current figure is the accumulated result of image
growth, not a security calculation. Bounding the window also keeps the slide
inside a single top-level table entry, which is what lets the table builder stay
simple.

**Relocations.** The relocation section is walked and every relative entry gets
the slide added to its addend. Target addresses are stored as link-time VAs, so
each is converted to a physical address using the runtime-derived delta — the
writes happen with the MMU still off. Under the current build the section is
empty, because every reference the compiler emits is PC-relative; the walker
terminates immediately. It exists for the code that will eventually take the
address of something in static data.

**The canary.** The cookie is seeded from the same mixed entropy, mid-function,
which forces this function to opt out of stack-protection: its prologue would
save the pre-init magic and its epilogue would compare against the post-init
cookie. Functions called before the seeding see the old value at both ends;
functions called after see the new one at both ends. The single function that
straddles the change is the one exempted.

## Data structures

Four file-scope values: the offset, the seed source, and the load-PA bounds of
the image. The last two are `volatile`, and that is **load-bearing rather than
stylistic**.

Under PIE at optimization, clang observes that the bounds are only ever assigned
from the address of a linker symbol — which it is entitled to treat as a
link-time constant — and rewrites the storage as a one-byte "was it set" flag,
returning the link-time address from the accessor. That is correct everywhere
except here: before the long branch, the PC-relative computation at the
assignment site yields the *load physical address*, and the link-time VA is a
different number. Forcing real eight-byte memory traffic is what makes the
accessor return the address that was actually computed.

The same expression evaluates differently before and after the long branch, which
is exactly why the values are cached at all: later consumers that need the load
PA cannot recompute it, because by then the identical source line yields a high
VA.

## Concurrency

None. Single CPU, MMU off, interrupts masked. Everything written here is
read-only for the remainder of the boot and the system's life.

## Invariants enforced

- **[[inv-i16]]** — a randomized, never-zero kernel base, with the entropy source
  reported rather than assumed.

## Error paths

There is no failure return. Absent entropy sources degrade to the next source
and, in the limit, to a counter read that always yields something. An
unsupported relocation type halts at a breakpoint instruction rather than
proceeding — the reasoning being that a relocation the walker cannot apply leaves
a wrong address in the image, which becomes a fault somewhere unrelated much
later; stopping at the point of detection is the only way that ever gets
diagnosed. The comment is candid that this is a build misconfiguration surfacing
at runtime because there is nowhere better to catch it.

## Performance

Irrelevant. One counter read, one hash, a walk over an empty section.

## Prosecution

- **The never-zero guarantee** must survive any change to the mask or the mixing.
  A slide of zero is not a crash; it is a silently unrandomized kernel.
- **The seed-source reporting must stay honest.** The value of the diagnostic is
  entirely that it distinguishes real entropy from the counter fallback; a change
  that reports the intended source rather than the used one removes the only
  signal that randomization degraded.
- **The `volatile` on the cached PA bounds is not removable.** It looks like
  redundant defensive typing and is the fix for a specific observed
  miscompilation.
- **The exemption from stack protection is bounded to exactly the function that
  changes the cookie.** Widening it, or moving the seeding into a callee, breaks
  the prologue/epilogue pairing argument in a way nothing would detect.
- **The relocation walk writes through physical addresses derived at runtime.** A
  change that used link-time addresses directly would write to unmapped memory
  before the MMU is on.

## Seams

- [[seam-kaslr-link-va-unchecked]] — the link-time base is duplicated between
  this header and the linker script, and both documents claim a build-time
  cross-check that does not exist.

## Caveats

**The relocation infrastructure is exercised by nothing.** The section is empty
in every current build, so the walker's loop body — the part that computes a
target address and writes to it — has never run. It is correct by reading, and
its first live use will be whatever change introduces the first absolute
reference. The header says as much.

**Widening the slide is not free in the way it looks.** The alignment is coupled
to the fixed page-grain kernel mapping, and the coupling is recorded in three
places (this header, the linker assertions, and the table builder) that must move
together. The header's comment enumerates them.

## Provenance

Read from `arch/arm64/kaslr.c` (275 lines) and `kaslr.h` (115) in full,
2026-08-02, during the boot sweep. One registered test covers the avalanche
property of the mix function; the slide choice, the relocation walk, and the
canary seeding have no direct test — they are covered by the boot succeeding and
by the banner reporting a plausible base, which is the area's general condition.
