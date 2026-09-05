---
id: moc-kernel-introspection
type: moc
title: "Kernel introspection Devs"
parent: moc-kernel
created: 2026-08-02
updated: 2026-08-03
---
The two synthetic Devs that render kernel state as readable text: `/proc`
(per-process state, plus the debugger's whole run-control surface) and `/ctl`
(machine-wide stats). Plan 9's idiom — walk to a file, read text — carrying
Thylacine's process-control and debug authority.

## The organizing fact

**These Devs widen VISIBILITY, never AUTHORITY — and the mechanism that makes
that true is that neither enforces its own mode bits.**

Both set `perm_enforced = false`. So the modes rendered by `stat_native` (0600
on ctl, 0400 on environ/sched, 0444 on the info files) are *documentation of the
policy*, and every real gate is a check at the **read or write site**. devproc
records why it had to be built that way rather than at open: the shared open
chokepoint hard-rejects before `devproc.open` ever runs, so the `CAP_KILL` axis
could not live there.

The consequence to hold onto: **a mode bit here is a comment. Grep the read
site.**

## Five gates, and the differences are the design

| Gate | Axes | Guards |
|---|---|---|
| kill (I-26) | owner OR `CAP_HOSTOWNER` OR `CAP_KILL` | ctl `kill`/`killgrp`, and `suspend`/`resume` |
| owner-or-host-owner | owner OR `CAP_HOSTOWNER` | `sched`, `environ` |
| debug (I-39) | kproc-refuse, NOTRACE-refuse, then owner OR `CAP_HOSTOWNER` OR `CAP_DEBUG` | `mem`, `regs`, `fpregs`, `kregs`, `kstack`, `wait`, the hw verbs |
| slot ownership | the writing ctl Spoor *is* `debug_owner` | `stop`/`start`/`waitstop`/`step`/`exitkill`/`hwbreak`/`hwwatch` |
| kernel-base | `CAP_HOSTOWNER` only — **no owner axis at all** | `/ctl/kernel-base` |

They are near-misses of one another on purpose, and the near-misses are load-bearing:

- **`CAP_DAC_OVERRIDE` is on none of them.** The filesystem admin capability is
  deliberately not a process-control axis — fs-admin stays orthogonal to kill and
  to debug, Linux's `CAP_DAC_OVERRIDE`-vs-`CAP_KILL` split. Stated twice in the
  code, at both gates that could plausibly have taken it.
- **`CAP_KILL` guards only killing.** Reading a process's scheduler internals is
  not killing it, so the telemetry gate is strictly narrower than the kill gate
  that sits beside it.
- **`CAP_DEBUG` guards only debugging.** Reading internals is not stopping and
  single-stepping, so it is absent from the info gates.
- **Slot ownership is stricter than I-39.** A stranger who *could* attach but has
  not cannot drive a target another debugger owns.
- **kernel-base has no owner axis** because the kernel has no owner-principal.
  The capability axis is the only one that could exist there.

## Children

- [[sub-kernel-devproc]] — `/proc`: the per-pid tree, the I-26 kill and
  job-control verbs, and the entire I-39 debug surface (attach, run control,
  registers, cross-Proc memory, hardware breakpoints).
- [[sub-kernel-devctl]] — `/ctl`: machine-wide stats, and the one gated leaf.
- [[sub-kernel-hwdebug]] — the architectural debug registers beneath the debug
  surface: per-CPU breakpoint isolation, single-step, watchpoints, and the three
  exception routes that turn a fire into a stop. Not a Dev — it has no namespace
  presence at all, and `/proc` is its only control surface.

## Cross-cutting

- Authority substrate: both Devs are consumers of
  [[moc-kernel-security]] — they read `Proc.principal_id` and `Proc.caps`
  directly rather than routing through `perm_check`, precisely so the
  capability axes stay separable per gate.
- Invariants: [[inv-i26]] (cross-process control is two-axis) and [[inv-i39]]
  (debug authority) are enforced here and nowhere else — and are **minted by
  this sweep**, which is what it was for: an invariant note's `guards` edge
  needs a swept enforcement home, and the absence of this one is where the
  batch-13 registry pass stalled. See [[chg-2026-08-02-introspection-sweep]].
- Specs: [[spec-debug-stop]] (the stop/resume/NoStrand machine) and
  [[spec-pty-stop]] (the two stop owners composing) both model protocol this
  area drives.
- The stop machinery itself lives in [[sub-kernel-proc]] and
  [[sub-kernel-death]]; this area is its *control surface*, not its
  implementation.
