---
id: chg-2026-08-15-handle-posix-fds
type: chg
title: "The handle table re-swept: POSIX descriptors arrived, and the dossier's ceiling was wrong when written"
date: 2026-08-15
arc: arc-vault
commits: ["cd1bc407"]
touched: [sub-kernel-handle]
established: []
closed: []
opened: []
mirrors-checked: [kernel/handle.c, kernel/include/thylacine/handle.h, kernel/include/thylacine/poll.h, kernel/include/thylacine/syscall.h, docs/phase7-status.md]
depth: rich
created: 2026-08-15
---
~606 lines across an audit-bearing surface, and the shape of the change is
that the handle table stopped being purely a capability structure and became
a **POSIX descriptor table as well** — fork inheritance, close-on-exec, the
`dup` family — without giving up any of the capability discipline. Five
commits, four of them Linux-compat work.

## What arrived

- **`handle_table_copy_into`** — fork copies the table (I-44). Rights
  verbatim, each slot taking its own reference, and it **cannot fail** (no
  per-slot allocation; destination known-empty and the same size), so a
  failed fork's rollback is already correct.
- **A close-on-exec bitmap** beside the slot array.
- **`handle_dup_posix`** (dup / F_DUPFD / F_DUPFD_CLOEXEC) and
  **`handle_dup_to`** (dup2 / dup3).
- **`PROC_HANDLE_MAX` 256 → 1024**, and a `hidx_t` typedef for indices.

## Two design arguments worth keeping

**The cloexec bitmap is parallel to the slots, and both halves of that are
reasoned.** POSIX close-on-exec is a property of the **descriptor**, not the
open file description: `dup(fd)` yields a second descriptor onto the same
description with the flag clear. A bit inside `struct Handle` "would be
shared by exactly the things POSIX says must differ."

The second half is better still, because a size assert did real work. `struct
Handle` has no slack — 8 + 4 + 4 + 8 is exactly the 24 its assert pins — so a
`u32` there grows it to 32 and takes the table from 24712 to 32904 bytes,
**across the order-3 `alloc_pages` boundary into order-4, doubling the
physical allocation to carry one bit per slot.** The bitmap costs 128 bytes.
The compile-time pin is what surfaced the cost; without it this is an
invisible doubling.

**Four duplication primitives, distinguished on two axes** (where the source
comes from × whether the destination may be occupied), and `handle_dup_to`'s
justification is the sharpest statement of the file's concurrency rule: the
obvious composition — close the destination, then dup with `min_idx` set to
it — is wrong because **the freed index is not reserved**. A peer thread's
fd-creating syscall can take it between the two calls, and the dup lands
elsewhere, silently, and only under concurrency. One lock hold also puts the
outgoing release *after* the new install, so the slot is never momentarily
empty.

Its destination is deliberately **ungated by kind**, and the asymmetry is
argued rather than assumed: it is being closed, and `handle_close` places no
kind restriction on closing, so refusing would invent a rule that
dup2-onto-fd-N is less permitted than close(N)-then-dup. `handle_replace`
refuses a non-Spoor outgoing for a reason the header explicitly says does not
transfer.

And the fork copy's most interesting property is what it **declines** to
copy: hardware handles are not inherited, so a child sees `EBADF` at that
index and its next open lands there where Linux's would not — "the honest
report of an authority the child was never eligible to hold."

## The dossier was wrong when it was written, not stale

It said `PROC_HANDLE_MAX` is 64, twice. The constant went 64 → 256 on
2026-06-24 and 256 → 1024 on 2026-08-13; [[sub-kernel-handle]] was written
**2026-08-02**, six weeks after the first lift.

The source of the error is still live and is the reason this is worth
recording rather than quietly fixing: **`poll.h` and `syscall.h` both still
say `PROC_HANDLE_MAX = 64` today.** A sweep that reads the surrounding prose
instead of the `#define` inherits whatever the prose last believed. That is
[[chg-2026-08-15-build-targets]]'s lesson arriving from the other direction —
there the dossier *recommended* the shortest of three lists; here it *quoted*
the stale one.

It also moved a number the dossier states as a property: the free-slot scan
is linear and unchanged in shape, so worst-case fd creation went from O(64)
to **O(1024)** under the table lock. Not a defect (the scan stops at the
first free slot, so a Proc holding few handles pays little), but it is the
one property the lift made materially worse rather than merely larger — and
the lift's own rationale is a throughput argument.

## The finding, and it has the file's own history in it

`poll.h` states the poll bound twice and disagrees with itself. `:29` says
`nfds <= PROC_HANDLE_MAX = 64` — wrong constant (it is `POLL_MAX_NFDS`) and
wrong value, landing on the right *number* only because `POLL_MAX_NFDS`
happens to be 64. `:286` is the corrected copy, names the right constant and
the right relationship — and **records its own repair**: it says so in as
many words, that it carried the same conflation until a named sub-chunk, and
that the identical conflation copied into pouch's `select()` became a real
`EBADF` bug.

Both halves of that have since decayed. The repair reached one of two copies
in one file, leaving the original error 257 lines above it — at the top,
where the hook-lifetime contract lives and where someone reasoning about
stack frames would start. And the correction's own parenthetical, `(256)`, is
now stale for the same reason it was written. Task #184; #166 covers only the
`syscall.h` sibling.

Same shape as [[chg-2026-08-15-syscall-dispatch-lineage]]'s proc.h finding: a
comment that documents having been repaired is not thereby protected from
needing repair again. Nothing about writing the history down makes the next
lift notice it.

## A correction to my own memory, caught by this file

`handle.h` credits the ceiling lift with taking GLQuake from 0.6 to 44.7 fps
— and states it precisely, **"(with the session-fid and tapestryd-fid
lifts)"**, because the A/B ladder's second rung shows raising this constant
*alone* was byte-identical.

A pin of mine said that same 0.6 fps was a "~300x per-submit serialization
collapse." Checking the provenance rather than quoting it found the pin
superseded: the four-table fid chain carried 75x of the gap, and the
serialization residual was re-scoped to ~4x the next day.

The pin was not careless — a native microbench had correctly exonerated the
silicon and correctly localized the loss to the virt path. It then named the
mechanism already in view as *the* mechanism, one step past what the evidence
supported, because nothing in a GPU microbench can see a fid ceiling. **A
measurement can be right about where a loss is NOT and still name the wrong
layer inside the region it localized.**

The part I want on the record: the index held that pin *and* the #198 pin —
"a refusal below both instrumented endpoints is invisible to both" — side by
side, the second being the correction to the first, filed a day later, with
nothing connecting them. Two pins disagreeing about one number, in the
artifact used to check the tree for exactly that defect. The code was the
better record, because it named its combination and the memory named a cause.
