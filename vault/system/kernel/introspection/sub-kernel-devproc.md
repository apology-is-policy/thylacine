---
id: sub-kernel-devproc
type: sub
parent: moc-kernel-introspection
title: "/proc — per-process state and the debug control surface"
code:
  - kernel/devproc.c
audit: hard
guarded-by: [inv-i26, inv-i39]
validated-by: [spec-debug-stop, spec-pty-stop, prose, gate-smp]
locks: [lock-proc-table, lock-vma, lock-territory-ns-lock, lock-territory-dot-lock, lock-env, lock-wait]
abis: []
design:
  - "docs/DEBUG-FS-DESIGN.md"
  - "docs/IDENTITY-DESIGN.md section 9.8"
  - "docs/PROWL-DESIGN.md OQ-4"
  - "docs/VIVARIUM.md section 6.2"
created: 2026-08-02
updated: 2026-08-16
---
## Purpose

The `/proc` Dev (`dc='p'`): a synthetic per-pid directory tree rendering process
state as text, and — since the Go-IDE debug arc — the *entire* control surface a
debugger drives. Fifteen files per pid, from `status` (Plan 9 parity telemetry)
through `mem` and `regs` (cross-Proc inspection of a stopped target) to `ctl`,
whose verb grammar carries kill, job-control stop, debugger attach, single-step,
and hardware breakpoints.

The largest single file in the kernel tree. Its size is almost entirely the debug
surface: the original P4-C Dev was `status`/`cmdline`/`ctl`/`ns`.

## Contract

Walk `/proc/<pid>/<file>`, read text; write verbs to `ctl`.

A qid encodes the target in its path: `(pid << 32) | subkind`, with path 0 the
apex. Every operation re-derives `(pid, kind)` from the qid it was handed — the
Dev keeps no per-Spoor state except the debug-attach flag.

`create`, `wstat`, `bread`, `bwrite`, `power` and the 9P `stat` encoding all
refuse. `.seekable = true`, because `mem` is VA-addressed and the register files
are struct-offset-addressed; without it the positioned-I/O gate rejects them
before the Dev is reached.

## Mechanism

### The mode bits are documentation; the gates are at the read site

`perm_enforced` is false, so nothing consults the modes `stat_native` reports.
This is not an oversight and the file says why: the shared open chokepoint
hard-rejects before `devproc.open` runs, so a `CAP_KILL` axis *could not* live at
open. Authority therefore lives at each read and write. The four gates and their
deliberate differences are tabulated in [[moc-kernel-introspection]].

### The read dispatch is a strict partition

Two mutually exclusive machineries serve reads:

- **format-and-slice** — the whole file is generated into a 2 KiB stack buffer
  under `g_proc_table_lock`, then the `[off, off+n)` slice is copied out.
  Eight kinds: `status`, `cmdline`, `ctl`, `ns`, `sched`, `exe`, `cwd`, `maps`.
- **own read path** — a purpose-built function, because the content is not a
  formatted snapshot. Seven kinds: `mem` (VA-addressed), `regs`/`fpregs`/`kregs`
  (struct-offset), `kstack`, `wait` (blocks), `environ` (offset-aware over an
  unbounded file).

A census of the four registration points confirms the partition holds today:
every one of the fifteen kinds appears in the file-name table and the mode
table, and in exactly one of {format dispatch + read whitelist} or {own path} —
none in both, none in neither.

That census matters because **adding a file means registering it in four
places**, and the code flags that only one of them fails *silently*: omit the
read whitelist and the file resolves fine and reads `-1` forever. The prior
`maps` chunk shipped with exactly that omission.

### The debug attach slot is a bare pointer, and that is sound

`Proc.debug_owner` holds the `/proc/<pid>/ctl` Spoor that claimed the debugger
slot. It is an **identity token, never dereferenced** — attach claims it, detach
and the ctl-fd close hook release it, and every comparison is pointer equality.

Two properties make that safe, and both are external to this file:

- **Pointer identity survives pid reuse.** A reused pid carrying a different
  `debug_owner` simply does not match.
- **The pointer can never dangle.** `spoor_clunk` runs the Dev's close hook
  exactly once, on the last ref drop, *before* the storage is freed. So the slot
  is always cleared before the Spoor it names can be reallocated. If the close
  hook were ever made conditional, or the free reordered ahead of it, this whole
  design becomes a stale-pointer match.

The close hook runs **outside** `g_proc_table_lock` — a precondition, not an
observation. It is reached from `handle_release_obj` and from the last-thread-out
close at exit, and both have dropped the lock; the hook then takes it via
`proc_for_each`. A future close path that still held the lock would deadlock.

### Two stop owners, one park

A thread parks on its own `debug_rendez` for either of two independent reasons:
the debugger's `debug_stop_req`, or job control's `job_stop_req`. The park
predicate is their disjunction; each owner clears only its own flag.

The debug-fs surface deliberately reads **`debug_stop_req` alone**. A Ctrl-Z'd
process is parked on the same rendez, but it is not debugger-stopped, and must
not become debugger-*readable* to whoever holds a ctl fd without having issued
the debug stop. The code carries an explicit "do NOT generalize this read"
marker on that predicate.

The `suspend`/`resume` verbs are the job-control side, gated by the *kill* gate
rather than the debug one — stopping is strictly weaker than the killing that
gate already permits, so they add no authority. Unlike the debug `stop`, they are
unconditional and non-blocking: the target cannot catch a `/proc` suspend, exactly
as it cannot catch a `/proc` kill.

### Fully-stopped is a conjunction, and death wins

Cross-Proc reads of memory and registers require the target to be *fully
stopped*: ALIVE, with a debug stop pending, with no pending group-exit message,
and with every non-EXITING thread parked on its own `debug_rendez` **and**
off-CPU. Each conjunct closed a real failure:

- the off-CPU spin, because a thread mid-context-switch still reads on-CPU while
  its saved frame is being written;
- the parked check under each peer's own wait lock, because that is the lock the
  park's register-then-observe takes — so it can never confirm a thread that is
  about to proceed to EL0;
- the group-exit check, because a dying target's threads go EXITING (which the
  parked scan *skips*) and then write their context outside the global lock. Death
  wins over a stop, everywhere.

### The park predicate lied about a thread on its way out

Two of those conjuncts — registered on the debug rendezvous, and off-CPU — both
hold on a thread that is **leaving** the park, and for a reason that lives in
another subsystem: the waker sets the thread runnable and queues it, but
deliberately does **not** clear the blocked-on registration, because only the
owning thread may clear it, so the group-terminate cascade can read it under the
wait lock.

So between a resume's wake and the thread actually being dispatched, the
registration is stale and the predicate says *parked* about a thread that is
about to run.

The consequence is worth following all the way down, because the shape of the
damage is not where the defect is. A stop issued while a prior start's wake was
still undispatched finds the stale park, the blocking wait returns immediately,
and the read that follows lands after the thread has been dispatched and cleared
its registration but before it has re-parked. The fully-stopped conjunction is
then false, and the caller receives **a bare denial it cannot distinguish from a
real authorization refusal** — so the debugger treats a transient as fatal, and
the process supervising it exits non-zero.

**A single return value for "not permitted" and "not yet" is what turned a race
into a fatal.** The race is a one-in-eighty event; the ambiguity is what made it
unrecoverable. That is this surface's error-path posture — everything is one
value — colliding with a state that is genuinely temporary, and it is the
strongest argument the file offers for eventually distinguishing them.

It surfaced only under hardware-accelerated virtualization, because the faster
processor widens the undispatched window. Substrate again deciding whether a
latent race is observable.

**The fix is a third conjunct — the thread's own state — and its three
properties are worth separating**, because any one of them alone would have been
a working fix rather than the right one:

- **It only narrows.** The added term can turn a true into a false and never the
  reverse, so nothing becomes newly readable. On a privilege surface that is the
  only direction a change can move and still be safe by construction rather than
  by argument.
- **It converges.** A runnable peer is dispatched, re-checks its condition, and
  re-registers as sleeping — so the poll terminates rather than spinning on a
  state that never settles.
- **It composes.** A job-parked thread is also sleeping, so a debugger stopping
  an already-job-stopped target still reads fully-stopped immediately, which the
  design elsewhere depends on.

**The stepping path needed no change, and why not is the interesting part.** It
polls the full stopped-conjunction, whose stop-requested term already rejects the
stale window. A stop caller has no such discriminator **because it sets that
flag itself before waiting** — so the one term that serves both callers had to be
the thread state. A discriminator that one caller establishes cannot discriminate
for that caller.

**No model changed, and the reason generalizes.** The formal model has an
*abstract* parked state; the defect lived in the implementation's **two-field
encoding** of that state, which admitted a configuration the model does not have.
The fix moved the implementation toward the model rather than the model toward
the implementation, so the model stayed the gate. **A correct model does not
guarantee a correct encoding of its states** — the gap between an abstract
predicate and the fields standing in for it is a place defects live, and it is
invisible from the model's side.

The decision is extracted as a **pure function** of the three inputs, so it is
assertable without constructing a thread. Both layers are probed separately, and
the reason is exact: they are blind to each other in **both** directions —
dropping the term fails only the pure assertion, while hardcoding the argument at
the call site fails only the walk, with every pure assertion still green.

### The register write has a privilege guard

Applying an edited `regs` struct writes x0–x30, SP_EL0 and ELR_EL1 — and
**never SPSR_EL1**. An arbitrary saved program-status register would let the
target `eret` to EL1. The read reports it; the write drops it, at any offset,
because the write path rebuilds the whole struct and overlays the caller's slice
before applying.

### kstack is the one relaxed gate — and it splits by capability

The settled-thread kernel backtrace deliberately drops the fully-stopped
requirement: any I-39-authorized caller may read a thread's kernel stack whenever
that thread is off-CPU, which is what lets it show a thread blocked *deep* inside
a syscall — something a debug stop, which parks only at the EL0-return tail,
structurally cannot.

Its output then splits on capability: raw slid kernel addresses reveal the KASLR
slide (an I-16 secret gated elsewhere behind `CAP_HOSTOWNER`), so the raw
columns go only to the `CAP_DEBUG`/`CAP_HOSTOWNER` tier. The owner axis gets the
symbolic `name+offset` form, which is link-relative and therefore
slide-independent — and which *is* the "why is it hung" diagnostic.

## Data structures

The Dev owns none. Every piece of state it manipulates lives on `struct Proc`
(`debug_owner`, `debug_exitkill`, `debug_stop_req`, `debug_focus_thread`,
`debug_hw`) or `struct Thread` (`debug_trapframe`, `debug_ss_armed`,
`debug_stepover_va`, `debug_rendez`).

What the file does own is a family of ~10 `proc_for_each` context structs, one
per operation, each carrying `target_pid` + the caller + operation arguments +
a `result` field. They are stack-local to the syscall, so they need no lifetime
discipline of their own.

The hardware-debug table is lazily allocated and **pre-allocated outside the
lock**, then installed by the callback if the target has none — because the
allocator may not run under a spinlock. A spare not consumed (the target already
had a table, or the gate refused) is freed by the caller.

## Concurrency

**The file never holds a `struct Proc *` across a lock drop.** Sixteen
`proc_for_each` sites; every operation is *resolve by pid under the global lock,
authorize, act, return a status code*. The blocking waits re-resolve on every
poll round rather than caching a pointer, so a target reaped mid-wait is simply
"not found" instead of a use-after-free. This is the single discipline that makes
a cross-Proc control surface safe here, and it should not be traded away for the
convenience of a cached pointer.

Lock order, all nested *under* `g_proc_table_lock` and all acyclic on the same
ground (each is a leaf; nothing held under it takes the global lock):

- `vma_lock` — the `maps` walk and the cross-Proc memory copy
- `ns_lock` — the mount-list render
- `dot_lock` — the cwd render
- `env_lock` — the environ render
- each peer's `wait_lock` — the parked-thread scan and the resume wake

Per-thread telemetry fields are read with relaxed atomic loads, because the
scheduler mutates a running peer's state and band **lock-free**; a plain read
there would be a C11 data race. The file is scrupulous about this everywhere it
touches a `Thread` — see Caveats for the one field where the same reasoning was
not applied.

The blocking verbs sleep on a **caller-private stack rendez** woken only by its
own deadline, so there is no shared wait structure to get the wake discipline
wrong on; the poll is death-interruptible, so a debugger killed mid-wait unwinds
and its ctl-fd close then resumes the target.

## Invariants enforced

[[inv-i26]] (cross-process control is explicitly two-axis) — enforced here and
nowhere else, by the kill gate, for both `kill`/`killgrp` and `suspend`/`resume`.

[[inv-i39]] (debug authority is namespace-plus-two-axis, stopped-only, never
stranding the quarry) — this file *is* its enforcement surface: the gate, the
stopped-only conjunction, the SPSR guard, the slot lifetime, and the resume-on-
release that discharges NoStrand. Modelled by [[spec-debug-stop]]; its
composition with the second stop owner by [[spec-pty-stop]].

**I-16** (the KASLR slide is a secret) — by the kstack raw/symbolic split.

**I-22** (no identity carries ambient authority) — negatively, and deliberately:
every gate is computed directly rather than through `perm_check`, so no identity
short-circuits and the capability axes stay separable per gate.

[[inv-i1]] — containment is namespace visibility: a Proc that cannot name
`/proc` cannot reach any of this.

## Error paths

Everything is `-1`. There is no errno on this surface: a denial, a
not-found, a not-stopped target, a malformed verb and an unknown file all return
the same value, and the debugger distinguishes them by which operation it
attempted. The blocking verbs return `-1` only when the *caller* was
death-interrupted — a target that exits or releases the slot ends the wait
successfully.

A denial formats **nothing** — the gated reads return zero bytes rather than a
truncated render, so there is no partial-disclosure path.

## Performance

The lock hold is the thing being managed throughout, since `g_proc_table_lock`
is global and taken with IRQs off:

- the cross-Proc memory copy is clamped to one page per call; the debugger loops
- the environ read is clamped to 8 KiB per call, and short reads are POSIX-legal
- every row-formatting loop commits a row only once it wholly fits, so a walk is
  bounded by the **output buffer** rather than by the number of VMAs or threads.
  A process at the VMA maximum therefore holds `vma_lock` for tens of rows, not
  tens of thousands — do not "fix" the truncation by continuing past a full
  buffer without re-deriving that bound.

The pid lookup is a linear walk of the process tree, acceptable while the live
process count is bounded; the general fix is the per-Proc-lock/RCU work on the
performance backlog.

## Prosecution

- **The four gates must not converge.** Each near-miss is a decision:
  `CAP_DAC_OVERRIDE` on none, `CAP_KILL` on kill only, `CAP_DEBUG` on debug only,
  slot ownership stricter than I-39. Widening any gate to "reuse" another is the
  bug.
- **kproc and NOTRACE are refused before the authority axes**, so no capability
  holder reaches them. A gate that checks capability first is wrong even if it
  reaches the same verdict.
- **The stopped-only predicate must stay a conjunction**, and must keep reading
  `debug_stop_req` alone. Generalizing it to the job-stop flag makes a Ctrl-Z'd
  process debugger-readable.
- **The park predicate keeps all three terms.** Registration alone is stale
  between a wake and its dispatch, because the waker cannot clear it — only the
  owning thread may, and the death cascade reads it. Dropping the state term
  restores a predicate that reports *parked* about a thread that is about to run.
- **Any new term on a privilege predicate must only narrow.** It may turn a true
  into a false and never the reverse; that is what makes it safe by construction
  instead of by argument, and it is checkable without reasoning about the race.
- **A gate extracted as a pure function needs its own probe, and so does its
  caller.** They are blind to each other in both directions: removing the logic
  fails only the pure assertion, and hardcoding the argument at the call site
  fails only the walk. One probe passing proves nothing about the other layer.
- **Death must keep winning.** The group-exit check inside the fully-stopped
  predicate is what keeps a dying target's EXITING threads out of the register and
  stack readers.
- **SPSR must never be written.** Any new register-write path re-inherits this.
- **No Proc pointer may be held across a lock drop.** The re-resolve-by-pid
  discipline is the lifetime argument for the entire file.
- **The close hook must keep running outside `g_proc_table_lock`**, and must keep
  running on every last-ref drop — the attach slot's pointer identity depends on
  the second, and the first is a deadlock if broken.
- **The debug-hw table must stay pre-allocated outside the lock**, with the
  unconsumed spare freed on every path.
- **A new per-pid file needs all four registrations.** The read whitelist is the
  one that fails silently.
- **A new gated file must format nothing on denial.**

## Seams

- **Per-thread files.** Registers, stack and step all operate on a single
  *focus* thread — the thread that trapped, or the head thread absent one. A
  `/proc/<pid>/thread/<tid>/` layer is unbuilt, and until it exists a
  multi-threaded target is inspected one thread at a time.
- **Single-step resumes the whole process.** Peers briefly run during a step.
- **`wait` is level-triggered**: an already-stopped target returns immediately
  rather than delivering one message per stop edge.
- **Deep renders truncate.** `ns` with long mount paths, `maps` with many
  regions, and `kstack` past 2 KiB all truncate at a line boundary; the fix is
  an offset-aware multi-read.
- **`cmdline` is a placeholder** that reports a fixed string rather than argv.
- **Proxying `environ` needs a deputy that acts with its client's authority.**
  The gate keys on the *reader*, so a server proxying this file is authorized as
  itself — which cuts both ways, and the dangerous direction is a system-principal
  proxy handing a system Proc's environment to a client of any principal. Until
  the mandate mechanism exists, a proxy must serve only its own peer.
- **`maps` is ungated on the argument that Thylacine has no userspace ASLR.**
  If user ASLR ever lands, this posture must be revisited in the same chunk —
  the file would then disclose exactly what the mitigation randomizes.

## Caveats

- **A cross-Proc reader can observe a torn process name.** See
  [[seam-proc-name-torn-read]]. Memory-safe by an unstated bound, cosmetic in
  effect, but a genuine data race that the surrounding code's own atomic
  discipline would otherwise have caught.
- **The `exe` clamp's comment has drifted from its numbers.** It justifies
  clamping the returned length by describing an out-of-bounds read "at offset >=
  512" against a 512-byte buffer; the buffer has since been raised to 2 KiB and
  the maximum path is 1 KiB, so the clamp is currently inert. The clamp is still
  correct defence — it is the arithmetic in the comment that no longer holds, and
  a reader who checks it will conclude the guard is unnecessary.
- **The focus-thread selector cites a test case that does not exist under that
  name.** The coverage is real and load-bearing — four assertions, including the
  foreign-focus fallback the comment insists must not be deleted — but it lives
  inside the stop/start/resume test rather than as the separately-registered case
  the citation names. Grep the symbol, not the cited name.
- **`ctl` reads are self-scoped by content, not by a gate.** A cross-Proc read
  returns zero bytes because the only readable content is the reader's own
  hardware-verify result. There is no authority check on that path; the emptiness
  is the containment.
- **The hardware-verify verb is boot-window-only.** It uses a single global slot
  whose interception keys on an address match rather than on the arming process,
  so post-boot it is refused outright rather than gated.

## Provenance

[[chg-2026-08-02-introspection-sweep]], [[chg-2026-08-16-devproc-park-predicate]].
