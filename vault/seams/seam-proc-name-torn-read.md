---
id: seam-proc-name-torn-read
type: seam
title: "A cross-Proc telemetry read can observe a half-written process name"
status: open
surface: [sub-kernel-devproc, sub-kernel-devctl, sub-kernel-proc]
opened-by: chg-2026-08-02-introspection-sweep
tracker: "task #17"
created: 2026-08-02
updated: 2026-08-02
---
## Owed

Either serialize the name stamp against the telemetry readers, or record the
real reason the race is harmless — because the reason currently written down is
not the reason it holds.

## The gap

`proc_set_name` stamps `Proc.name` from the resolved binary path at exec, byte
by byte, terminator last. It runs without `g_proc_table_lock`, and its comment
justifies that:

> Runs in the CHILD's context (p == the execing Proc), before it reaches EL0 —
> no concurrent reader observes a torn stamp.

That is true of the execing Proc's *own* threads, and false of everything else.
Three readers take `Proc.name` cross-Proc, from another CPU, under
`g_proc_table_lock` — which the writer does not hold:

- `/ctl/procs`, the process-list NAME column
- `/proc/<pid>/status`
- `/proc/<pid>/sched`

The process-list reader is polled continuously by the monitor tool, so "a
concurrent exec on another CPU" is the ordinary case, not a contrived one.

## Why it is nonetheless safe today

Not for the reason the comment gives. It is safe because of an **unstated bound
in the writer's loop**: the copy continues only while the index is strictly below
`PROC_NAME_MAX - 1`, so the final byte of the array is *only ever written as the
terminator*, and is otherwise zero from the allocator. The array therefore always
terminates within itself, at any instant, mid-stamp or not.

That matters because the reader is `fmt_str`, which walks until it finds a zero
and is bounded by its *output* buffer, not by the source array. Without that
final-byte property a mid-stamp read could walk out of `name[]` into adjacent
`Proc` fields and copy them into a file userspace reads — an isolation defect
rather than a cosmetic one.

The bound is pinned by a test, but framed as string hygiene ("long name stays
NUL-terminated") rather than as the property that keeps a concurrent reader in
bounds. Nothing connects the test to the readers that depend on it, so a future
tightening of the loop reads as a cleanup.

Byte stores on aarch64 are single-copy atomic, so no individual byte tears. The
observable effect is a name mixing old and new bytes for the few hundred
nanoseconds of the stamp, in a telemetry column.

## Why it is still worth closing

It is a plain C11 data race — concurrent plain reads and writes of the same
bytes, therefore undefined behaviour in principle and visible to a thread
sanitizer. And the surrounding code knows better: every per-thread telemetry
field in the same functions is read with an explicit relaxed atomic load,
carrying a comment that a plain read *would* be a data race because the writer
is lock-free. The exact same argument applies to the name and was not made,
because the writer lives in a different file and its comment asserts the race
away.

So the defect is really about where the reasoning lives: a safety property held
by an accident of a loop bound, an inaccurate justification at the writer, and a
reader whose own careful atomic discipline stops one field short.

## Risk while open

Low, and it does not grow with load or thread count — the window is one stamp at
one exec. It grows only if `fmt_str` is ever replaced by a length-bounded copy
that trusts a separately-stored length, or if the writer's loop bound is
"simplified".

The cheap fix is documentary: correct the writer's comment to name the real
invariant, and note at the readers that they depend on it. The thorough fix is
to make the stamp and the reads explicitly atomic, or to take the lock for the
stamp — the stamp is a handful of bytes on a cold path.
