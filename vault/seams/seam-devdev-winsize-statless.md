---
id: seam-devdev-winsize-statless
type: seam
title: "/dev/winsize answers no fstat, in a Dev whose stated rule is that every leaf does"
status: open
surface: [sub-kernel-devdev, sub-kernel-cons]
opened-by: chg-2026-08-02-console-sweep
tracker: "task #19"
created: 2026-08-02
updated: 2026-08-02
---
## Owed

Add the geometry leaf to `devdev_stat_native`, add it to the all-leaf stat test,
and reconcile the two comments that currently disagree about whether its absence
is a rule or an exception.

## The gap

`devdev_stat_native` switches on the leaf kind. It enumerates the root and the
two mount-point stubs, the console, the control file, the renderer pair, and the
five trivial leaves — twelve of the thirteen kinds. `DEV_KIND_WINSIZE` is absent,
so it falls to the default arm and `fstat("/dev/winsize")` returns `-1`.

Three things in the tree say that is wrong:

- The Dev's own vtable comment states the intent as *every* leaf answering — it
  names the console fix and then "every other leaf too, so `clang++ < /dev/null`
  can fstat it."
- The rationale beside the switch is that a stat failure with an errno other
  than bad-fd is **fatal** to a real toolchain: the compiler's standard-fd
  fixup stats its own fds and dies on failure, which is the defect that put the
  slot in this Dev in the first place.
- The stat test's header says it covers "every leaf shape" — while enumerating
  a hardcoded list of five trivial names that predates this leaf.

And one thing says it is right: the geometry leaf's own test comment records
"stat_native stays cons-scoped (the leaf itself is statless)", as though the
absence were the design.

Both cannot be true. Nothing in the code distinguishes this leaf from the
trivial ones it was explicitly added alongside — its own leaf-table comment
files it under "trivial-leaf class (ungated)".

## How it got in

The all-leaf stat coverage and the geometry leaf arrived in different arcs. The
stat switch was completed for the leaves that existed; the geometry leaf was
added later, wired into the four registrations that fail *loudly* — the kind,
the name table, the read dispatch, the write dispatch — and missed the one that
fails **quietly**.

That is the same shape as the introspection Dev's read whitelist, where a
missing registration leaves a file that resolves fine and reads `-1` forever
([[sub-kernel-devproc]]). A leaf missing from read or write is dead on arrival;
a leaf missing from stat works perfectly until someone stats it.

## Risk while open

Low today and bounded: no current consumer stats this leaf, and it is newer
than the toolchain defect that motivated the slot.

But the population it is *for* is the one most likely to trip it. The geometry
leaf exists precisely so that an ordinary unprivileged program — one that
cannot mint a control fd — can read the terminal size. Ordinary programs stat
what they open, and the standard-library behaviour that made this class fatal
twice before is exactly that reflex. The prior two instances in this tree, a
pipe and a notes fd, were both silent until a workload happened to put one on a
standard descriptor, and one of them killed concurrent build jobs with no
diagnostic at all.

## Fix

Add the case. It is a three-line arm: character device, world-readable,
system-owned — matching the trivial leaves it belongs with, and deliberately
*without* the is-a-console marker, for the same reason the control file goes
without it (a program must not read the geometry file as a terminal).

Then add it to the all-leaf test's list, and delete or correct the comment
asserting it is statless.
