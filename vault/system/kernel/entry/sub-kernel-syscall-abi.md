---
id: sub-kernel-syscall-abi
type: sub
parent: moc-kernel-entry
title: "The syscall ABI — the number space, the argument records, and the three copies of both"
code:
  - kernel/include/thylacine/syscall.h
  - usr/lib/libt/include/thyla/syscall.h
  - usr/lib/libt/include/thyla/poll.h
  - usr/lib/libt/src/start.S
  - usr/lib/libthyla-rs/src/lib.rs
audit: hard
guarded-by: [inv-i5, inv-i13, inv-i32]
validated-by: [prose, gate-smp, gate-interactive]
locks: []
abis: [abi-t-stat, abi-handle-rights, abi-errno]
design:
  - "docs/ARCHITECTURE.md section 13"
created: 2026-08-03
updated: 2026-08-03
---
## Purpose

The contract half of the userspace boundary. [[sub-kernel-exception]] defines
*where* a crossing happens; this defines *what* crosses — the syscall numbers,
the register convention, the argument records copied across, and the bounds
every one of them is checked against.

It is three files saying the same thing in three languages, and nothing in the
build checks that they agree.

## Contract

`x8` carries the syscall number, `x0..x5` the arguments, `x0` the result —
deliberately Linux's AArch64 convention, so a ported libc's syscall stub needs
renumbering and nothing else. Userspace issues `svc #0`; the immediate is
ignored at v1.0 and reserved as a future class selector.

Numbers are **append-only and never reused**. Four of the 104 allocated slots
are holes: three retired when `/srv` moved from dedicated syscalls to ordinary
namespace operations, one reserved for an unbuilt device-class query. Each hole
carries a comment naming what used to be there or what is coming. An unknown
number returns `-1` rather than terminating the caller.

Return values follow two conventions and the split is per-syscall, not
per-family: older calls return a bare `-1` for every failure; newer ones return
a negative errno. The errno registry deliberately keeps `T_E_PERM` at 1 so that
`-T_E_PERM` and the bare sentinel are the same value — which means an errno-
returning syscall can never distinguish "permission denied" from "generic
failure", and the header says so, steering POSIX-EPERM contours to
`T_E_ACCES` instead.

## Mechanism

### The number space is coherent, and that is verifiable

100 numbers are live; `syscall_dispatch`'s switch has exactly 100 arms; the two
sets are equal with no gaps in either direction. Every mirrored number agrees
across all three copies — there is no case where a name means one number to the
kernel and another to a library.

That is worth stating precisely because it is *not* guaranteed by anything. It
is the current state, maintained by hand.

### The libraries are subsets, not copies

Neither mirror carries the whole set, and neither is expected to: the C library
exposes 74 numbers, the Rust library 92, and each omits what its consumers do
not call. So the invariant that matters is not "the mirrors are complete" but
"where they overlap, they agree" — which holds.

### Nothing pins a mirror to the kernel

The kernel header carries 100 compile-time assertions; the C mirror 46; the Rust
mirror 33. Every one of them constrains **its own file**. Across the two mirrors
the word "mirror" appears on 73 lines, 22 of them as "MUST mirror". Neither
phrase is a mechanism.

There is no generator, no shared header, and no build step that reads one file
and checks the other — `tools/build.sh` never mentions either mirror. The
enforcement is that a human wrote MUST in a comment.

The clearest statement of this is the poll ABI's slim header, which is worth
quoting because it is entirely correct and draws no conclusion:

> MUST mirror the kernel side; the kernel's `_Static_assert`s pin the layout
> **there**. Drift here would surface as a SYS_POLL ABI mismatch at runtime —
> keep them in lockstep.

It names the asymmetry ("there"), names the consequence (runtime, not build),
and names the remedy as an instruction to a person. The file contains no
assertion of its own. The Rust mirror's `pollfd` says the same thing in the same
shape — the kernel's asserts "pin the kernel side" — and also carries none. Both
happen to be correct today because both spell the fields with types that give
the same C layout.

### Growth is by appended field into a reserved slot, and it has worked

The spawn argument record has grown three times — an identity block, a hardware
allowance block, a page budget — from 56 bytes to 96, and every existing caller
kept working, because each growth either appended past the end or claimed a
field that was already reserved and required to be zero. The page budget is the
best case: it took over the tail padding slot, so the struct did **not** grow,
and every caller that zero-fills the struct gets the historical behaviour by
construction rather than by a compatibility branch. Sixteen offset assertions
pin the result, and the one on the reused slot spends its message explaining
that reuse rather than restating the offset.

### The all-or-nothing rule, stated twice and broken once

Oversize arguments get one of two dispositions, and the file is explicit about
which is right for which kind of call.

**Clamp** is correct for byte I/O: read, write, pread and pwrite clamp to the
128 KiB transfer maximum and return the count, because a short transfer is what
POSIX promises and the caller loops.

**Refuse** is correct where a short result is meaningless or dangerous, and the
header argues this twice, in two separate chunks, about two different fields.
The secret-scrub primitive rejects an oversize length with a comment that says
"do NOT silently cap — for a secret-scrub primitive, capping and returning
success would silently retain the tail of the buffer", and names its precedent
set: the console write and the directory read, which reject the same way. The
page budget refuses an over-cap request "never clamped — a silent clamp would
hand back a budget the caller did not ask for and hide the misconfiguration".

The CSPRNG read clamps. See Caveats.

## Data structures

Thirteen argument and result records cross the boundary, each pinned by size and
per-field offset assertions on the kernel side: the spawn arguments (18
assertions, the most-grown record), the stat result, the hardware allowance
descriptor, the PCI info block and its two sub-records, the debug register
frames, the peer identity record, a timespec, and a JIT region descriptor.

One is pinned only transitively. The hardware window — a base/size pair — has no
assertion naming it, but the descriptor that contains an array of eight of them
asserts that the following field sits at offset 128, which forces each window to
be exactly 16 bytes. That is real pinning, just indirect.

The bounds constants are the other half of the ABI: a 128 KiB transfer maximum,
a 4 KiB stack-scratch bound that three calls are deliberately held at, a 1 KiB
path maximum, a 256-byte binary name, 512 argv entries in 64 KiB of data, 16
inheritable descriptors.

## Concurrency

None owned. These are declarations; the file defines no state and takes no
locks. Concurrency lives in the handlers.

The one ABI-level concurrency statement is negative and worth keeping: the
argument records are copied out of user memory before validation, so a
concurrent peer thread scribbling the same buffer cannot make a checked field
change afterwards. That property belongs to the handlers, but it is what makes
these records safe to describe as records rather than as pointers.

## Invariants enforced

None directly — a header enforces nothing. It *declares* the shapes through
which four invariants are enforced elsewhere:

**I-2** (capability monotonic reduction — still unminted as a note, cited bare
here as [[sub-kernel-caps]] cites it) — the spawn record's capability mask is
documented as advisory: the kernel ANDs it with the parent's own set, so a mask
requesting more than the parent holds is clamped rather than refused, and
monotonic reduction holds structurally rather than by a check. Note that this is
a *deliberate* clamp, in a file that argues twice against silent clamping: it is
sound because the clamp is toward less authority, which is the direction the
invariant wants, and because the caller cannot have been relying on the excess.

**[[inv-i5]]** — the descriptor-inheritance list is documented as accepting only
file handles, precisely because hardware handles must not cross a process
boundary.

**[[inv-i13]]** — every user pointer in every record is a value to be validated,
never dereferenced from the declaration.

**[[inv-i32]]** — the page budget field is the per-process page cap's spawn-time
entry point.

## Error paths

Two conventions, per-syscall. The bare `-1` set predates the errno set and has
not been converted; the header documents which each call uses, individually, in
its enum comment. A caller cannot tell from the number which convention applies.

The unknown-number arm returns `-1` and lets userspace decide — the header notes
that a signal-equivalent note is the eventual behaviour and was not built.

## Performance

Not a runtime surface. The one performance-relevant ABI decision is the 128 KiB
transfer maximum, raised from 4 KiB when the measured read ceiling turned out to
be one 4 KiB staging buffer per round trip.

## Prosecution

- **A new syscall appends. It never fills a hole.** The three retired numbers
  and the one reserved number stay unallocated; reuse would silently redirect a
  stale binary's call.
- **A new argument record field appends, or claims a reserved slot that is
  already required to be zero.** Both are proven patterns here; anything else
  breaks a caller that zero-fills.
- **Every appended field gets an offset assertion, not just a size assertion.**
  A size assertion alone passes on a field reorder.
- **A mirror change must be made in all three files in the same commit**, because
  nothing else will catch it. The mirrors are subsets, so "not present" is
  legitimate and indistinguishable from "forgotten".
- **A new oversize bound must choose clamp or refuse deliberately**, and the
  choice is semantic: clamp iff a short result is a complete answer the caller
  can loop on. Refuse otherwise.
- **A bound documented in a mirror must be cited by the right constant name.**
  See Caveats — one is not, and correcting the mirror by looking up the name it
  cites would change the value by 32x.

## Seams

- **The two return conventions are not converging.** New syscalls use errno, old
  ones use `-1`, and there is no migration underway. A caller handling both must
  know which is which per call.
- **The `svc` immediate is unused.** Reserved as a class selector; nothing reads
  it.
- **Environment pass-through at spawn was reserved and never built.** The
  reserved slot is still there, still required to be zero — the environment
  arrived instead as a per-process filesystem, so the slot now guards a design
  that was superseded rather than deferred.
- **The blocking mode of the CSPRNG read does not exist.** See Caveats.

## Caveats

- **The CSPRNG read is the one call that silently caps, and four documents say
  otherwise.** An oversize request is clamped to the 4 KiB scratch bound and
  returns 4096; it is not refused. The header states the bound as a precondition
  without saying what violating it does. The C mirror lists the failure causes
  and omits oversize — accidentally the most accurate of the four. The Rust raw
  wrapper states outright that the kernel returns -1 on an oversized length, and
  the safe wrapper repeats that claim in a comment while separately enforcing the
  bound itself before the call. So the guarantee the documentation describes is
  real — it is just implemented in the library, one layer above the kernel that
  is credited with it.

  What makes this more than a documentation slip is the neighbourhood. The
  secret-scrub primitive twenty-eight lines earlier refuses an oversize length
  and carries a comment explaining that capping a security primitive and
  returning success is the wrong disposition — a comment that cites *the Rust
  wrapper's documented behaviour* as part of its justification, and enumerates
  the sibling calls that reject the same way. The CSPRNG read is not in that
  enumeration, and it is the file's other secret-handling call. The page-budget
  field, added by a much later chunk, argues the same principle again from
  scratch. The rule is stated twice in this one file and the exception is the
  entropy source.

  Nothing is broken today. The only native consumer that calls the raw wrapper
  is the TLS entropy backend, and it checks that the returned count equals the
  requested length — a check that is redundant *against the documentation* and
  load-bearing *against the kernel*. It is the only thing between a large request
  and a partially-randomized buffer, and to a reader who trusts the wrapper's
  documentation it looks like dead code.

- **A constant is correct and its stated source is wrong, in the direction that
  punishes checking.** The Rust CSPRNG module's per-call limit is 4096, which is
  right, and is documented twice as mirroring the transfer maximum, which is
  128 KiB. The bound it actually mirrors is the stack-scratch constant. Anyone
  verifying the mirror against the constant it names would raise it to 131072 and
  land squarely in the silent-cap path above.

- **The blocking mode of the CSPRNG read is documented and unimplemented.** The
  header describes a flags argument where zero blocks until the generator is
  seeded and one returns immediately. The handler range-checks the argument and
  never reads it; an unseeded generator always fails. The handler's own comment
  is honest about this; the header, which is what a userspace author reads, is
  not. The non-blocking constant is defined in two headers, described three
  incompatible ways across four files, and read by nothing.

- **Three enum comments state a name bound of 64 bytes; it has been 256 since
  the on-device toolchain needed absolute paths.** Both mirrors carry 256
  correctly. The stale copies are in the kernel header's own prose, describing
  the calls the bound applies to.

## Provenance

[[chg-2026-08-03-syscall-abi-sweep]].
