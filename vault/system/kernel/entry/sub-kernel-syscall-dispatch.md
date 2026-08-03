---
id: sub-kernel-syscall-dispatch
type: sub
parent: moc-kernel-entry
title: "The syscall dispatcher — argument marshalling, the staging tiers, and where a gate is allowed to live"
code:
  - kernel/syscall.c
audit: hard
guarded-by: [inv-i13, inv-i32, inv-i22, inv-i27]
validated-by: [prose, gate-smp, gate-interactive]
locks: []
abis: []
design:
  - "docs/ARCHITECTURE.md section 13"
created: 2026-08-03
updated: 2026-08-03
---
## Purpose

The single chokepoint where untrusted register values become typed C arguments.
[[sub-kernel-syscall-abi]] declares what crosses; this is the machinery that
takes it across — the dispatch switch, the user-pointer validator, the staging
buffers, and the split between the layer that talks to userspace and the layer
that does the work.

**What this dossier covers, precisely.** The file is 8178 lines and holds all
100 handlers. Most of a handler is *policy belonging to its own subsystem* — a
Burrow handler is described by [[sub-kernel-burrow]], a pts handler by
[[sub-kernel-pts]], and so on across roughly thirty dossiers. The subject here
is what surrounds them: dispatch, marshalling, validation, staging, error
convention, and the layering rule that decides where an authority check may sit.
A handler's own semantics are its subsystem's; a handler's *shape* is this
dossier's.

## Contract

`syscall_dispatch` receives the interrupted register frame, reads the number
from `x8`, and switches. Each arm reads its arguments from `x0..x5`, calls a
handler, writes the result to `x0`, and returns. Three arms never return: the
two exit calls and the group-exit call, which abandon the frame on the exiting
thread's kernel stack until the reaper collects it.

An unknown number writes `-1` and returns. The dispatcher itself performs no
validation, no capability check and no bookkeeping — every gate is a handler's.

## Mechanism

### The dispatch is exactly as wide as the ABI says

100 syscall numbers are live and the switch has exactly 100 arms; neither set
has a member the other lacks. No arm falls through to its neighbour. Across all
100 arms the only registers read are `x0` through `x5` and `x8` — the argument
window the ABI declares, with nothing reaching past it. Three syscalls use the
sixth argument; most use one or two.

None of that is enforced by a mechanism. It is the current state, and it is
checkable, which is why it is stated here as a measurement rather than an
assumption.

### Two layers, and the rule about which one may hold a gate

Forty-five syscalls are split in two. A `_handler` takes raw register values,
resolves the current thread, validates user pointers, stages buffers, and calls
a `_for_proc` inner that takes an explicit process and kernel-side buffers. The
inner is the testable half — most are called from the kernel's own test suite,
and eight are called from production kernel code that needs the operation
without a syscall frame.

The layering rule follows from that: **the handler may own only what is about
userspace.** Thread resolution, pointer validation, staging, register
narrowing. Everything else — and every authority gate in particular — belongs in
the inner, because an inner is separately callable and a gate above it is a gate
some caller does not pass.

The rule holds nearly everywhere. Where a syscall has no inner, the gate is in
the handler by default and nothing can bypass it. Where a syscall has an inner,
the gates are in the inner. One exception is in Caveats.

### The capability is checked once, at mint, and the object type carries it after

The JIT surface is the clearest instance of the pattern that makes the layering
rule tractable. Emitting executable code requires a capability — but only the
create call checks it. Neither the publish call nor the teardown call re-checks
anything, and that is deliberate rather than an omission: publish requires its
range to lie inside a live VMA whose Burrow is of the code type, and only the
capability-gated create mints one. Teardown is explicitly reasoned about in the
source — releasing memory you already own is not an exercise of the emit
authority, and gating it would turn a capability expiry into a leak.

So the authority is enforced by *kernel-minted object type*, not by repeating a
capability check at every touch. A re-check would be the weaker design: it has
to be added to each new operation, and forgetting one is silent.

### Staging is two-tier, and the tiers exist for opposite reasons

Byte I/O bounces through kernel memory. Operations up to the 4 KiB stack scratch
use a stack buffer; larger ones take a transient heap allocation so a single
call can move up to the 128 KiB transfer maximum. The stack tier exists to make
the metadata-storm path free; the heap tier exists because the stack tier was
the bulk-read ceiling — two thirds of a compiler build's reads were exactly one
4 KiB chunk against a much larger negotiated message size.

The heap tier is user-drivable kernel memory held across a potentially
indefinite device operation, so it is budgeted per process. Charge precedes the
allocation, uncharge accompanies every free, and an over-budget request degrades
to the stack tier rather than failing — a short transfer is a correct answer.
The trusted boot chain is exempt.

The balance is worth stating because it reads wrong at a glance: the length
variable is *reassigned* to the stack bound when the heap tier is not taken, so
charge and uncharge appear to use different values. They cannot — the
reassignment happens only when no allocation is held, and every uncharge is
guarded on holding one. All four staging sites are structurally identical.

### The copy happens before the lock, on purpose

The console write stages its bytes into kernel memory *before* claiming the
console writer role. Faulting a user page can sleep, and holding the console
across an unbounded page-in would stall every other writer behind it. The same
shape appears wherever a handler stages then acts: validate, copy, then engage
the subsystem.

### Two error conventions, and the collision that forced the steering

Older syscalls return a bare `-1`. Newer ones return a negative errno. The
errno registry keeps its permission code at 1, so `-T_E_PERM` is bit-identical
to the bare sentinel — a deliberate collision, documented, which is why handlers
on the errno convention steer permission-shaped failures to the access code
instead.

Where a device's error is forwarded, an out-of-window negative is clamped to a
generic I/O error, so a device cannot punch a value through the boundary
library's error window and have it read as an enormous success.

## Data structures

None owned. The dispatcher operates on the exception frame
([[sub-kernel-exception]]) and on per-process state it does not define. Its own
state is the two staging buffers, both stack-local, and a per-process atomic
byte counter for the heap tier.

## Concurrency

Each syscall runs on its caller's own thread with its own stack frame, so the
staging buffers are private by construction. Three shared concerns:

- **The heap budget counter** is a per-process atomic updated by
  compare-and-swap. Sibling threads of one process contend on it; the loop is
  the standard bounded retry.
- **Handle lookups return a held reference.** The lookup helpers acquire the
  object's reference under the handle-table lock and transfer it to the caller,
  who must release it on every exit path. This is what makes a blocking device
  operation safe against a sibling thread closing the same descriptor
  mid-call — the pre-existing contract returned a bare pointer into the live
  table, which was exactly that race.
- **A reference is taken across a lock drop where the work is long.** The
  instruction-cache publish takes a Burrow reference under the address-space
  lock, drops the lock, does the maintenance, and releases — so a sibling
  tearing the region down concurrently frees it there instead of underneath.

## Invariants enforced

**[[inv-i13]]** — the user-pointer validator is the boundary. It rejects null,
rejects anything at or above the user-VA top, and rejects a length that would
wrap or cross that top. Forty-nine call sites use it. It validates a *range*,
not a pointer: a zero length passes unconditionally, which is correct because
nothing is dereferenced, and is why every caller pairs it with the length it
will actually touch.

**[[inv-i32]]** — the staging budget is this file's contribution to the
per-process resource floor.

**[[inv-i22]]** and **[[inv-i27]]** — several handlers carry identity and
console-trust gates. They are enforced here in the sense that this is where the
check is written; the authority model itself is [[moc-kernel-security]]'s.

## Error paths

Every handler returns a negative on failure and the dispatcher writes it back
unexamined — there is no central error translation. The unknown-number arm
returns `-1`; the source notes that a signal-equivalent notification is the
eventual behaviour and was not built.

The device-error clamp is the one piece of central error handling, and it lives
in the shared read and write bodies rather than in the dispatcher.

## Performance

The staging tiers are the measured surface: raising the transfer maximum from
4 KiB to 128 KiB was a throughput fix, not a cleanup, and the stack tier is
preserved precisely so the small-operation path pays nothing for it.

The zero-copy network fast path bypasses staging entirely when a large
transfer's buffer lies inside a flow's shared ring, and is gated on a size
threshold so small transfers never pay the extra handle lookup.

## Prosecution

- **A new syscall's authority gate goes in the inner, not the handler**, unless
  the syscall has no inner. An inner is separately callable; a gate above it is
  advisory to anyone who does not come through the handler.
- **A new handler reads only `x0..x5`.** The frame has more registers; they are
  not arguments.
- **Every arm returns.** The switch has no fallthrough and gains none.
- **A new staging site charges before allocating and uncharges on every exit**,
  including the fault and error paths, and must not reassign the length variable
  between the two while an allocation is held.
- **A new user-pointer use is validated with the length actually touched**, not
  with the caller's buffer size — see Caveats for the sibling pair that
  disagrees about this.
- **A lookup's reference is released on every path.** The helpers transfer
  ownership; a missed release is a leak that outlives the call.
- **A capability is checked at mint.** Prefer gating later operations on the
  kernel-minted object type over re-checking the capability, which must be
  remembered at each new site.

## Seams

- **The two error conventions are not converging.** New syscalls use errno, old
  ones the bare sentinel, and a caller cannot tell which from the number alone.
- **The unknown-syscall arm has no notification.** Documented as owed.
- **A debugger cannot publish a target's instruction cache.** The publish call
  resolves the *caller's* process, so a cross-process write into a JIT region —
  which the debug surface permits — cannot be followed by the maintenance that
  makes it visible. The source names this as a missing kernel mechanism rather
  than a caller error, which is the correct reading: it is the composition of
  two surfaces that were each sound alone.

## Caveats

- **Two syscalls of identical shape disagree about oversized buffers, and the
  disagreement is a fix applied to one of them.** Both the working-directory
  read and the descriptor-name read copy a bounded kernel string into a user
  buffer and return its length. The working-directory one computes the string
  first, then validates and copies *exactly* the bytes it will write, and
  carries a comment explaining why: the earlier version rejected any buffer
  larger than the path maximum, which broke every caller using the
  near-universal idiom of passing a page-sized buffer — the comment names the
  build tools it broke and the error they emitted.

  The descriptor-name read still has both halves of that rule. It rejects a
  buffer larger than the path maximum, and it validates the caller's *whole*
  buffer rather than the handful of bytes it writes. Neither is load-bearing:
  the real fit check happens twenty lines later against the actual string
  length, and the validator is overflow-safe without an upper bound. All three
  copies of the ABI document only the too-*small* failure; none mentions
  too-large.

  It is unreached today — the sole caller is a boot probe with a 64-byte
  buffer — and the caller class the fix's own comment enumerates is exactly the
  one that would meet it. The rule was written down as two coupled corrections
  with their rationale, and landed on one of the two places that had it.

- **One authority gate sits in the handler above a separately-callable inner.**
  The console-open gate — the check that only the console-trust anchor may take
  the single-reader console — is in the handler, while its inner is non-static
  and reachable. No production caller uses the inner today; the kernel test
  suite does, which is legitimate and is also the demonstration that the bypass
  exists. A future kernel-side console consumer would acquire the descriptor
  without the check.

- **The validator's zero-length pass is a range property, not a pointer
  property.** A null pointer with zero length validates successfully. Every
  current caller pairs it with the length it will touch, so this is correct; a
  caller that validated with zero and then dereferenced would be unprotected,
  and nothing prevents that shape.

## Provenance

[[chg-2026-08-03-syscall-dispatch-sweep]].
