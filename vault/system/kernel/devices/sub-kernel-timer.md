---
id: sub-kernel-timer
type: sub
parent: moc-kernel-devices
title: "The timebase — one counter, three clocks, and no seqlock"
code:
  - arch/arm64/timer.c
  - arch/arm64/timer.h
  - arch/arm64/rtc.c
  - arch/arm64/rtc.h
audit: hard
guarded-by: [inv-i15, inv-i17]
validated-by: [prose, gate-smp]
locks: []
abis: []
design:
  - "docs/ARCHITECTURE.md section 22.6"
  - "docs/PORTABILITY.md section 5"
  - "docs/TICKLESS-IDLE.md"
created: 2026-08-02
updated: 2026-08-02
---
## Purpose

Provide the machine's sense of time: a free-running counter, a periodic
interrupt that drives preemption, a single-shot arm that lets an idle CPU stop
taking that interrupt, and the offset that turns the counter into a wall-clock
date.

The real-time clock chip is folded in here because that is all it does — it is
read exactly once, at boot, to seed that offset, and never touched again.

## Contract

The counter is monotonic, per-machine, and readable from userspace once
bring-up has enabled that. The periodic interrupt is per-CPU: every CPU must arm
its own, and does so in its own handler on every fire. Two derived clocks are
published — one that only ever moves forward, and one that a privileged caller
may step.

**Which timer.** The architecture provides two, and this uses the *virtual*
one. Not a preference: under the hypervisor this is developed on, the physical
timer is reserved by the host, and a guest write to its control register comes
back as an undefined instruction. The virtual timer works on the emulator, under
the hypervisor, and on bare metal alike, so choosing it keeps one timebase
everywhere rather than a per-substrate branch — and userspace is pointed at the
matching virtual counter, because under a hypervisor the two counters differ by
an offset and a kernel and a program disagreeing about which they read would be
worse than either choice alone.

The interrupt number is fixed by the architecture; a compile-time assertion
pins it, so a regression in the number-mapping helper fails the build rather
than silently arming an interrupt nobody handles.

## Mechanism

**Bring-up.** Read the counter frequency; ending the world if firmware left it
unset, because everything downstream would divide by zero. Reject a requested
tick rate faster than the counter itself. Compute the reload count once and
cache it. Then arm the boot CPU.

**Per-CPU arming.** The countdown and control registers are banked per-CPU, so
arming on the boot CPU does nothing for anyone else. Each secondary arms itself
during bring-up. A CPU that never arms takes no timer interrupt, so a
compute-bound thread there is never preempted and its co-runnable peers starve —
which is a scheduling-fairness failure with no error path, exactly the
observerless class this area is built around.

**The handler** re-arms this CPU's own countdown, increments the tick counter
*only if it is the boot CPU*, and calls the scheduler tick. The boot-CPU
restriction is deliberate: once every CPU began ticking, a shared counter
incremented by all of them would be a multi-writer read-modify-write race, and
the counter's consumers want a single-writer timebase at the original rate more
than they want a total across CPUs.

**The one-shot.** An idle CPU does not need a periodic interrupt; it arms a
single shot at the nearest thing it is waiting for and parks, so a genuinely
idle CPU takes no timer interrupts at all. This closes a pathology in which the
never-stopping tick caused an exit to the hypervisor a thousand times a second
on an idle machine. The periodic path is byte-unchanged for a CPU that is
running something, so the scheduling slice model and every tick-coupled test are
untouched.

The delta computation is **split out as a pure function** so the clamping can be
unit-tested without touching a live timer — the arming wrapper is three lines
around it.

**The clamp** bounds every reload, periodic and single-shot alike. A floor,
because a reload of one fires every counter tick and no handler can keep up — so
the floor caps the interrupt rate. A ceiling at the register's signed width,
because a larger value truncates and fires *sooner* than asked rather than
later. Both bounds are public, not private, precisely because they are a
contract a caller relies on: a target already in the past clamps up to the floor
and fires immediately, and a target beyond the horizon clamps down and the idle
loop simply re-arms.

**Counter to nanoseconds.** Done as separate whole-units and remainder terms
rather than multiplying and then dividing. A flat multiply would overflow within
a few minutes of uptime at the reference counter frequency. The split keeps the
whole-units term small for centuries and bounds the remainder term well below
the overflow point.

**The wall clock is one number.** A single offset, added to the monotonic clock.
Not a pair of anchor values, and that is the design's whole point: **there is no
second field to tear against, so no lock and no seqlock is needed.** A concurrent
reader gets either the old offset or the new one, each internally consistent,
and since stepping the clock is a deliberate discontinuity, sub-microsecond skew
at the instant of the step is meaningless. This is what makes a *runtime* clock
setter safe on a multiprocessor by construction rather than by argument. Every
publish also mirrors the offset into the page userspace reads directly, so a
program's syscall-free clock tracks the same value.

Both publishing paths — the boot anchor and the runtime step — fail soft to
zero on an implausible epoch, which leaves the wall clock equal to the monotonic
clock: 1970 plus uptime, the honest "there is no wall clock here" signal rather
than a fabricated plausible date.

**The clock chip.** Found in the tree, or at the reference platform's documented
fixed address if absent. Mapped, read once, and the mapping simply kept — there
is no unmap and the kernel never re-reads it. The value is accepted only inside
a plausibility window of about eighty years. The window has *two* sides on
purpose: a floor rejects the zero an absent device reads as, and a ceiling
rejects the all-ones an undriven bus floats to, which would otherwise decode as
a date early in the twenty-second century and look plausible enough to keep.

**Userspace counter access** is enabled for both counters, though the kernel and
userspace agree to use the virtual one.

## Data structures

Four file-scope values, no structures. The frequency and the reload count are
written once on the boot CPU before any secondary exists, and read
unsynchronized everywhere after — safe because the bring-up barrier orders the
writes before any secondary's first read. The tick counter is `volatile` and
boot-CPU-only. The wall-clock offset is a single aligned machine word accessed
with atomic load and store.

## Concurrency

No locks anywhere. Three different justifications, each stated at its site:

- Frequency and reload: written once before secondaries exist; ordered by the
  bring-up barrier. A future dynamic reprogrammer would need to add ordering.
- Tick counter: single-writer by restriction to the boot CPU.
- Wall-clock offset: a single aligned word, so a store is atomic and a reader
  sees one value or the other.

The countdown and control registers are banked per-CPU, so every arm is local
by construction.

## Invariants enforced

**[[inv-i15]]** — the frequency comes from a system register, the clock chip's
address from the tree with a documented reference-platform fallback.

**[[inv-i17]]** — the timer interrupt is what makes preemption happen at all.
This area supplies the mechanism; the scheduling bound itself is
[[sub-kernel-sched]]'s.

## Error paths

Ending the world: a zero counter frequency at bring-up; arming before bring-up
has computed the reload.

Returning false: a zero or too-fast requested tick rate; a computed reload
outside the clamp.

Failing soft to zero: no clock chip in the tree and no mapping at the fallback
address; a clock reading outside the plausibility window; an implausible epoch
at either publish path; any time conversion called before bring-up.

## Performance

The counter read carries an instruction barrier so a prior store has retired
before the timestamp is taken. Everything else is a register write or two.

The one-shot's purpose is entirely performance: it took an idle machine under
the hypervisor from a third of a CPU spent on nothing to approximately none.

## Prosecution

- The periodic path must stay byte-unchanged for a running CPU — the slice model
  and every tick-coupled test depend on the rate.
- Every CPU must arm its own timer, and re-arm in its own handler. A missing arm
  produces silent unfairness, not an error.
- The tick counter must stay single-writer; widening it to all CPUs
  reintroduces a read-modify-write race and changes its rate.
- The wall-clock offset must stay one word. A second field makes the lock-free
  read wrong, and the runtime setter is what makes that reachable.
- The nanosecond conversions must keep the split form; a flat multiply overflows
  within minutes.
- The clamp bounds are a public contract; the single-shot arm relies on them.
- The plausibility window needs both sides — the ceiling is the one that is easy
  to drop and hard to notice missing.

## Seams

None open. The wall clock's settability, once a recorded gap, is closed by the
runtime setter.

## Caveats

- **Two accessors publish the same frequency, one lossy.** A narrower one
  truncates the value to half a machine word; a wider one returns it whole. The
  distinction is recorded only in a comment at the one call site that must have
  the wide form — the page userspace reads, where the two disagreeing would make
  a program's clock differ from the kernel's. The narrow one survives in a boot
  banner, a benchmark and two tests, all measurement rather than correctness,
  but nothing at its definition says so. Truncation is unreachable below a
  counter frequency of about four gigahertz.
- The clock chip's mapping is never released; one page of address space for a
  device read once.
- The counter is thirty-two bits at the source, so the wall clock read at boot
  cannot represent dates past the early twenty-second century. The plausibility
  ceiling sits at that boundary for a different reason, and the two are easy to
  confuse.
- The file header describes the reload arithmetic in terms of the *physical*
  timer's registers in one place, a leftover from before the switch; the
  surrounding text and all the code are virtual.

## Provenance

Read from `arch/arm64/timer.c` (321 lines), `arch/arm64/timer.h` (193),
`arch/arm64/rtc.c` (52) and `arch/arm64/rtc.h` (43), 2026-08-02, at `f109477e`.
Cross-checked: both frequency accessors' call sites, the interrupt-number
assertion, the reservation entry for the clock chip, and the registered tests.

Absorbed `docs/reference/11-timer.md`.
