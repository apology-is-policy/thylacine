---
id: sub-kernel-halls
type: sub
title: "Halls of Extinction — the crash dump and the live-thread backtrace"
parent: moc-kernel-entry
code: ["arch/arm64/halls.c", "arch/arm64/halls.h", "arch/arm64/halls_symtab.h", "arch/arm64/halls_symtab.stub.c"]
audit: hard
guarded-by: []
validated-by: [gate-smp]
locks: []
hazards: []
abis: []
design: ["docs/HALLS-OF-EXTINCTION.md", "docs/TOOLING.md section 10"]
created: 2026-08-03
updated: 2026-08-03
---
## Purpose

When the kernel dies, capture what cannot be recovered afterwards — registers,
a frame-pointer backtrace, a stack window, the address-space randomization
slide — and push it out the UART before halting. Everything here runs on a
machine already known to be broken, so the governing rule is not *be complete*
but **never loop, never recurse, and survive your own faults**.

A second, later consumer reuses the same frame-pointer walk on a *live* thread
for the debugger's kernel backtrace. The two look alike and their safety
arguments are opposites; see Caveats.

## Contract

`halls_dump(ctx)` — the whole dump. Called from the extinction path *after* the
`EXTINCTION:` line and *before* the halt. A non-NULL `ctx` dumps that saved
exception frame; NULL means "consult the per-CPU live frame, and if that is not
trustworthy, synthesize one from the current stack".

`halls_enter_frame` / `halls_leave_frame` — the per-CPU live-frame slot, driven
by the exception entry wrappers in [[sub-kernel-exception]]. Enter returns the
previous value; leave restores it, so a nested exception does not clobber an
outer frame.

Four pure helpers, exposed because they are the testable part:
`halls_fp_is_sane` (the frame-pointer gate), `halls_frame_is_live` (the
plausibility gate on the per-CPU slot), `halls_link_addr` (remove the slide),
and `halls_symbolize_table` (the symbol lookup over an explicit table).

`halls_walk_kernel_frames` — the live-thread twin of the backtrace, for the
debugger's kernel-stack read in [[sub-kernel-devproc]]. Takes **explicit**
bounds and returns canonical return addresses.

## Mechanism

**Output order is a decision.** The register block is emitted first, because it
is pure field reads off the saved frame; the backtrace and stack hexdump come
after, because both touch live and possibly corrupt memory. If the dump dies
partway, the data most likely to matter has already left the machine.

**Three sources for the frame, in order.** An explicit `ctx` from the caller; else
the per-CPU slot; else a synthetic frame built from the current stack pointer,
`__builtin_frame_address`, `__builtin_return_address`, and a best-effort read of
the syndrome registers. The synthetic case is honest about what it lost — it
prints `x0..x28 not captured` and labels the resume address stale, because at a
bare assertion the general registers are gone and only the backtrace survives.

**The plausibility gate is what makes the per-CPU slot usable.** A live exception
frame sits just above the current stack pointer — the chain from handler to
extinction to dump is a few hundred bytes. A slot stranded by a Proc that has
since exited points at a freed stack, so the gap is enormous or negative. The
gate accepts only a frame at-or-above the current stack pointer and within
16 KiB of it, which is far above the real chain depth and far below the distance
between two stacks. A rejected slot falls through to capture-current rather than
producing a fabricated dump.

**The frame-pointer walk** follows the saved-frame chain, reading the return
address at a fixed offset from each frame pointer. Three things bound it: a
depth cap, a sanity gate (16-byte aligned, strictly increasing, within range),
and — for the dying-machine path only — a span ceiling above the starting frame.
The strict-increase requirement is what kills cycles; the cap and the range
handle everything else. A read of an address that is sane but unmapped still
faults, and that is caught by the re-entrancy guard.

**A frame sourced from EL0 is not walked.** Its frame pointer is a user address,
so walking it would read user memory from kernel mode and let a faulting Proc
plant fabricated "kernel" frames in the crash dump. The register block and the
syndrome registers still carry the signal.

**Symbolization** is a binary search over a sorted table of link-relative
offsets, generated per build directory from the linked image and living in
read-only data. It reads no stack, takes no lock, allocates nothing, and is
bounded by the log of the entry count — the properties that let it run on the
dying path at all.

**Return addresses are stripped, not authenticated.** On a core with pointer
authentication the spilled return address carries a signature in the unused high
bits; stripping restores the canonical address. The check reads the identification
register directly rather than the boot-populated feature block, because the dump
can fire before feature detection has run — a rare and correct instinct: the crash
path must not depend on initialized global state.

## Data structures

Two per-CPU arrays: the live-frame slot and the in-dump guard byte. Both plain,
both indexed by the CPU's affinity field.

`struct halls_sym` is a pair of 32-bit values — a link-relative code offset and a
byte offset into a NUL-separated name blob. Storing **offsets rather than
absolute addresses is load-bearing and the reason is written down**: an absolute
address in initialized data draws a relative relocation that the boot stub
slides, which would both bloat the relocation section by one entry per symbol
*and* turn each stored value into a runtime address — defeating the whole
link-relative design. A 32-bit offset is a plain constant.

The table is generated; a committed stub with a zero count seeds a bare build so
the kernel always links and runs, degrading to raw addresses.

## Concurrency

**No atomics, and the argument is per-CPU exclusivity**: each CPU's slot is
written only by that CPU's own exception wrappers and read only by that CPU's
dump. Two CPUs never touch one slot.

The slot can nonetheless go stale two ways, both named in the source. A handler
that departs by a path that never returns — an EL0 fault terminating the Proc —
skips its leave and strands the slot at a stack that will later be freed. And a
*blocking* syscall handler can resume on a different CPU, so its leave runs
there: it writes a stale value into the second CPU's slot and strands the first.
That is not a data race — the writer moved sequentially, it did not become
concurrent — and the stale value is exactly what the plausibility gate exists to
reject.

The in-dump guard is per-CPU and one-way within a dump. The contract it rests on
is stated: **every caller halts afterwards**, so a dump that faults leaves the
guard set on a CPU that is about to stop.

## Invariants enforced

Three local invariants, numbered in the design scripture and carried in the
source:

- **A fault during the dump does not loop.** The guard is set before any
  potentially-faulting read; a recursive extinction sees it, prints a marker, and
  returns, so the caller reaches the halt. The marker matters: a suppressed dump
  is never silent.
- **The frame-pointer walk is bounded.** Depth cap plus sanity gate; a wild frame
  pointer can neither spin nor read unboundedly.
- **The per-CPU slot is trusted only when plausible.** The gate above.

A fourth invariant in the same family is about output rather than code: the
`EXTINCTION:` line is a tooling ABI and stays first and unchanged, with the dump
following under its own prefix. That one is enforced by the extinction path and
the harness, not by this file.

## Error paths

The design is deliberately short of error returns — nothing here can fail
usefully, so every degradation is a *fallback that still prints something*:

- Re-entered dump → one marker line, return.
- Symbol lookup misses (empty table, address below the table base, offset past
  the 32-bit window) → the raw and link addresses print alone, which is the
  pre-symbolization behaviour.
- An address below the slide → left unchanged rather than underflowed, so the
  operator is not shown a nonsense translation.
- No live frame → the synthetic frame, explicitly labelled.

## Performance

Irrelevant by construction: this runs once, on a machine that is about to stop.
The bounds exist for termination, not speed. The live-thread walk is the one
place cost is real, and it is a capped loop over at most a few dozen frames plus
one logarithmic symbol lookup per frame, under the process table lock.

## Prosecution

On any change, re-establish:

- The guard is still set **before** the first faulting read, and every caller
  still halts. A caller added that survives the dump breaks the guard's contract
  — the source says so, and says the fix would be save/restore.
- The walk still terminates on a hostile frame pointer: alignment, strict
  increase, range, depth.
- EL0-sourced frames are still not walked, and their link addresses still
  suppressed.
- The plausibility gate still rejects a stranded slot without rejecting a real
  one — the fault-to-extinction chain must keep passing it.
- The symbol table stays relocation-free. A change that stores an absolute
  address reintroduces a per-symbol relocation *and* slides the stored values.
- The live-thread walk's callers still pass genuinely mapped bounds. Its
  fault-safety is entirely a property of its **caller**, not of the function.

## Seams

**A device-mapped address would stall, not fault.** On real hardware a read into
strongly-ordered device space can hang the interconnect instead of faulting, and
the re-entrancy guard only catches faults. Dormant on the emulated target, whose
memory is all RAM-backed, and largely unreachable now that only a plausible live
frame is dumped. Recorded against the bare-metal arc; the fix there is a
normal-memory predicate on the peek targets.

**An out-of-range CPU index aliases onto CPU 0.** Documented as dormant — the
supported topologies use a dense affinity field within the maximum. Worth noting
that the sibling file in this tier handles the identical condition by returning
early instead, and a third site clears the hardware but skips the bookkeeping:
three dispositions for one impossible input.

**The symbol table has no end sentinel.** See Caveats — this is the one seam with
a live consumer.

## Caveats

**The symbolizer has no upper bound, and one of its two consumers depends on it
having one.**

The lookup returns the greatest entry whose offset is at or below the query. Its
documented failure modes are all on the *low* side — an empty table, an address
below the table base, an address below the first symbol — and each is guarded. On
the high side there is nothing: the generator collects only text symbols and
emits no end marker, so **any address from the last function up to 4 GiB above
the table base resolves to that last function with a large offset.** The unit
test pins this deliberately, in as many words: *past the last symbol, still the
last, no upper bound at v1.0*.

The kernel image is a few megabytes and read-only data follows text, so the
window that misresolves is not exotic. **A pointer to any kernel global, string
literal, or the symbol table itself symbolizes as the last text function.** Stack
words holding such pointers are ordinary.

For the crash dump this is tolerable, and that is *why it was acceptable when it
was written*: every symbolized line prints the raw address and the link address
beside the name, so a reader who sees a five-digit offset discounts it, and the
whole artifact is a best-effort snapshot of a dead machine.

The live debugger path removed exactly that. The kernel-stack read splits its
output on capability, because raw kernel addresses disclose the randomization
slide: the capability tier sees raw, link, and symbol; the **owner tier sees the
symbol alone**, on the argument that a link-relative `name+offset` reveals no
slide. That argument is sound about the slide and silent about honesty — with no
address to check against and no upper bound in the lookup, **a garbage frame is
presented to the owner as a named function, indistinguishable from a real one.**
The `<unknown>` branch that exists for precisely this purpose can only fire for
the low-side failures, so it never fires for anything above the last symbol.

What makes this worth recording rather than filing and forgetting: the same
sub-chunk that introduced the live walk was *scrupulous* about not sharing the
frame-pointer walker, and said why — the dying-machine path is audited, its
invariants must not be perturbed, so the live path got its own function with its
own bounds. That judgment was exactly right about fault-safety. The symbolizer
was shared without the same question being asked, and the property that made it
safe in the first consumer was the raw address printed next to it — which the
second consumer's own security fix then removed.

**The guard-clear comment and the guard-contract comment disagree about a future
caller.** One says a caller that survives the dump would require converting the
guard to save/restore; the other says the tail clear already keeps the slot
honest for such a caller. Both are half right — the tail clear covers the clean
path and not the faulting one — but a reader who finds the second comment first
will conclude the case is handled.

## Provenance

Design scripture is the Halls document; the output prefix ordering is a tooling
ABI. The dump is an audit-trigger surface as part of exception entry, so the
per-CPU frame slot and the walk carry named invariants rather than prose. The
live-thread walk arrived with the debugger's settled-thread inspect and is
governed by [[inv-i39]]; its capability split on raw addresses is an [[inv-i16]]
consequence and came from a review finding, not from the original design.

Unit coverage is the pure helpers — the frame-pointer gate in four cases, the
slide translation both ways, the enter/leave nesting, the plausibility gate, and
the symbol lookup. The dump itself has no unit test and cannot easily have one;
its witness is that the machine keeps booting and that the fault harness produces
a dump end to end.
