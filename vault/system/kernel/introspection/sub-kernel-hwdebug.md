---
id: sub-kernel-hwdebug
type: sub
title: "Hardware debug — breakpoints, watchpoints, single-step"
parent: moc-kernel-introspection
code: ["arch/arm64/hwdebug.c", "arch/arm64/hwdebug.h"]
audit: hard
guarded-by: [inv-i39]
validated-by: [spec-debug-stop, spec-debug-step, gate-smp]
locks: []
hazards: []
abis: []
design: ["docs/DEBUG-FS-DESIGN.md section 5"]
created: 2026-08-03
updated: 2026-08-03
---
## Purpose

The hardware half of [[inv-i39]]. A debugger arms breakpoints and watchpoints at
user addresses in a stopped target and single-steps it; this file owns the
architectural debug registers that make that happen, and the exception routes
that turn a fire into a whole-Proc stop. The control surface — the verbs, the
authorization, the stop protocol — is [[sub-kernel-devproc]]; this is the
mechanism beneath it.

It also owns a small boot-time probe: a self-scoped breakpoint whose only job
was to establish, empirically, that a guest-programmed EL0 breakpoint actually
delivers its debug exception to guest kernel mode on this substrate before the
real machinery was built on that assumption.

## Contract

`hwdebug_init_cpu` — per-CPU bring-up. Clears the OS lock and double-lock (both
locked at reset, and both suppress debug exceptions), then idles the debug
control register and every implemented breakpoint and watchpoint slot. Runs on
every CPU because these registers are banked per processing element.

`hwdebug_enumerate` — reads the debug feature register once at boot and records
the implemented breakpoint and watchpoint counts.

Table mutation — add, remove, clear-all for each of breakpoints and watchpoints,
plus the teardown free. Callers hold the process table lock and the target is
fully stopped.

`hwdebug_switch_in` — the context-switch hook. Loads the incoming thread's Proc
debug state onto this CPU, or clears it if the incoming thread is not being
debugged and this CPU still carries someone's.

Three exception handlers, one per debug exception class — breakpoint, software
step, watchpoint. Each returns true if it handled the exception; false means the
Proc was never debugged, which the caller treats as fatal.

`hwdebug_wp_encode` — the watchpoint register encoding, exposed so the register
arithmetic can be unit-tested separately from delivery.

Four verify functions — arm, on-exception, result, disarm — the boot probe.

## Mechanism

**Per-CPU isolation is the central idea, and it is not what the architecture
gives you.** Programming a breakpoint to fire only in user mode does *not*
isolate two Procs that happen to share a user address — both would trap. So the
breakpoint set is loaded on switch-in to a debugged thread and cleared on
switch-out, and the master enable bit follows: **a breakpoint fires only while
the debugged Proc is actually running.** The per-CPU "we have debug registers
loaded" flag exists so the common case — the incoming thread is not debugged and
this CPU carries nothing — costs one boolean read and a not-taken branch rather
than a dozen system-register writes.

**Single-step state is loaded per-thread, not held per-CPU**, and that is
deliberate: it must follow the thread across an interrupt-driven migration
mid-step. This is the Linux per-task model.

**Two interactions between the three features are handled explicitly.** During a
step, watchpoints are loaded *disabled*, so the stepped instruction's own data
access cannot trap a watchpoint and derail the "exactly one instruction" property.
And a step from a breakpointed address loads that one breakpoint disabled — the
step-over — so the instruction being stepped does not immediately re-trap.

**A fire routes through the attach-gated stop, not the raw one.** All three
handlers call the variant that takes the process table lock and delivers the stop
only while a debugger still owns the slot. This is a fix, not an original design:
the exception path formerly set the stop flag ungated, so a fire racing a detach
parked a target with no debugger left to resume it. All three arms are now
symmetric, and the model carries a dedicated invariant and a counterexample
configuration for it.

**Every unmatched fire is benign, and the reason is structural**: only the kernel
can program a debug register — user mode cannot reach the debug control register
or the breakpoint registers at all — so an unmatched exception is never an
attack, only a stale arm that a table change has outrun. The handler disables
this CPU's debug registers and resumes; the instruction re-executes untrapped.

## Data structures

`struct debug_hw` — the per-Proc table, lazily allocated on the first arm and
freed at process teardown. A breakpoint count and address array; a watchpoint
count, address, length and access-flags array. Both counts are atomic; the arrays
are not, and the discipline that makes that sound is in Concurrency.

Two file-scope counts hold the *usable* slot counts — the minimum of what the
hardware implements and what the tables size. A per-CPU boolean tracks whether
this CPU currently carries someone's debug registers.

The verify probe has one global slot under a leaf lock: armed, valid, fired, the
arming pid, the address, and the trapped resume address.

Writing a numbered system register needs a compile-time register name, so both
slot writers are a bounded switch over all sixteen architectural slots, generated
by a macro that is immediately undefined again — confined, rather than an
abstraction.

## Concurrency

**Table mutation is quiescent, not locked.** The tables are mutated only when the
target is fully stopped — every thread parked, no CPU running one — so the
switch-in reader is not running and needs no lock. What the atomics buy is the
one case that is *not* quiescent: a detach clears the count while the target
runs, and that must race cleanly with a switch-in or a match.

The publication order carries it. Add writes the slot then releases the count;
remove compacts the last entry into the hole then releases the reduced count. A
concurrent reader therefore sees a consistent prefix — never a count that
promises a slot the writer has not filled.

Debug-register writes are done with interrupts masked so the CPU is pinned across
them: the switch path already runs masked, and the exception benign paths mask
explicitly.

The verify slot's lock is a leaf — no nested locks, no sleeping — and is touched
only on arm, disarm, and an actual fire, never on the common exception path.

## Invariants enforced

This file is where [[inv-i39]]'s **no-escape** clause is literally true rather
than argued: breakpoints are hardware registers, so arming one never writes the
target's text and cannot violate the write-xor-execute rule. Nothing here maps,
patches, or allocates in the target's address space.

It also carries the clause that a fire cannot strand its quarry — the attach gate
on all three exception arms, which the stop model states as a named invariant.

The **exactly-one-instruction** property of a step is enforced jointly here and
at the return tail: the step is disarmed before the re-stop, so a racing
switch-in cannot re-arm it.

## Error paths

Every path degrades to *resume the target*, never to a fault:

- Add refused — table full, duplicate address, or a rejected shape (zero or
  oversized length, a region crossing the doubleword the encoding can cover,
  empty access flags). The verb write fails.
- An exception on a Proc that was never debugged — the only false return; the
  caller treats it as a genuinely stray exception and it is fatal to the Proc.
- An exception with no matching entry, or with the count already cleared — this
  CPU's debug registers are disabled and the instruction resumes.
- A fire that finds no debugger owning the slot — the same disable-and-resume, so
  a detached target runs free. The step arm disables *all* debug registers here
  rather than just the step bit, because a step loads the master enable and the
  breakpoint table too; clearing only the step bit would leave a stale breakpoint
  armed until the next switch-out. That symmetry across the three arms was itself
  a review finding.

## Performance

The switch-in hook is on the context-switch path, which is why the common case is
a single boolean. When debug state *is* loaded, the cost is one register pair per
usable slot plus one control-register write and a single instruction barrier —
batched deliberately, with no barrier per slot.

## Prosecution

On any change, re-establish:

- **The quiescence premise.** Table mutation is unlocked *because* the target is
  fully stopped. A verb that mutates a running target breaks the reader, and the
  atomic counts do not save the arrays.
- **Publication order.** The count is released last on add and on remove; a
  reader must never see a count covering an unwritten slot.
- **The attach gate on all three exception arms.** A fire racing a detach must
  not park a target with no owner.
- **Slot indices stay within what the hardware implements.** The usable counts are
  the minimum of implemented and table size, and bring-up clears every
  *implemented* slot — not merely the used ones.
- **The step's disarm still precedes the re-stop**, and watchpoints are still
  loaded disabled during a step.
- The verify's exception match still keys on its own address, so a real
  breakpoint is never swallowed by the probe.

## Seams

**The verify probe is a single global slot.** One arm at a time, boot-only,
self-scoped; its lingering-armed-on-a-migrated-CPU corner is closed properly by
the per-thread install rather than in the probe. It is additionally confined to
the boot window by an explicit gate, added after a review found it could
otherwise swallow another Proc's real breakpoint.

**The watchpoint address is not surfaced.** The architecture reports the faulting
address imprecisely — anywhere within the access block — so delivery deliberately
does *not* gate on an exact match, on the argument that a missed stop is worse
than an imprecise one. The debugger reads the program counter instead. A closest-
match heuristic is named in the design as the eventual refinement.

**A still-armed watchpoint re-traps on plain resume**, because a watchpoint fires
with the resume address pointing at the accessing instruction. The debugger works
around it by stepping — watchpoints are off during a step — or by removing the
watchpoint first.

## Caveats

**The watchpoint table is capped at four slots, and the reasoning that lifted the
breakpoint cap applies to it verbatim.**

The breakpoint table is sized to the architectural maximum of sixteen, and the
constant carries a full account of why: it was previously four, which starved the
Go debugger's step-over — that arms one temporary breakpoint per successor
address plus the return address, so a small step needs four or five slots at once
alongside the user's own breakpoint — and the overflow surfaced as a failed
control write reading as a permission error. The stated principle is explicit:
**the software table never caps below the hardware, so a debugger gets every
breakpoint the CPU implements.**

The watchpoint constant beside it is four, and its comment is a parenthetical:
the table size, clamped to what is implemented. The feature register field for
watchpoints is four bits wide exactly as the breakpoint one is, so a CPU may
implement up to sixteen; bring-up already clears all sixteen. On any part with
more than four watchpoints, the extra slots are unreachable to a debugger, and
the failure mode is the same failed control write that took a debugging session
to diagnose the first time.

The sharp part is that **the same file contains the reasoning and its
non-application**. Bring-up clears every *implemented* slot rather than only the
used ones, and says why — a stale enable bit in any slot would fire the moment
the master enable is set for a breakpoint, because that one bit gates breakpoints
and watchpoints alike. That is the implemented-versus-used gap reasoned about
carefully, one function away from the constant that ignores it.

**One impossible input, three dispositions.** An out-of-range CPU index is
clamped to zero in the crash-dump sibling, causes an early return here that
silently skips both loading and clearing, and in a third path clears the hardware
but skips the per-CPU bookkeeping. All three are dormant on supported topologies,
and only the first says so.

## Provenance

Design scripture is the debug-fs document's hardware section. The bring-up and
enumeration landed with the empirical verify, whose result was the gate on
building anything further — and it passed on the first attempt on both the
emulated and the hypervisor-accelerated substrate, which is why the real
machinery followed immediately rather than through a software-breakpoint detour.

The per-Proc install, single-step and watchpoints followed. Two of this file's
current shapes are review outcomes rather than original design: the attach gate
on the exception arms, and the step arm's full disable on the detached path.
[[spec-debug-step]] models the step machine; [[spec-debug-stop]] carries the
attach gate as a named invariant with its own counterexample.

Unit coverage is the enumeration floor, the arm-disarm round trip, both tables'
add/remove/duplicate/full/clear behaviour, the benign spurious step, and the
watchpoint register encoding. Delivery itself — that the hardware actually traps —
is proven by the in-guest probes, because no unit test can raise a debug
exception.
