---
id: dec-2026-10-14-descriptor-slot-reservation
type: dec
title: "A descriptor is reserved, filled, then installed; a failed call never publishes one"
date: 2026-10-14
status: standing
decided-by: user-vote
affects: [sub-kernel-handle, sub-kernel-vivarium, sub-kernel-syscall-dispatch]
created: 2026-10-14
---
## Fork
Twelve syscall sites installed a descriptor and then, on a later failure,
closed it BY NUMBER (OPEN-BUGS 2026-10-13 15:48Z): pipe, the socket ctl
unwinds, accept and its address copy-out, openat's O_DIRECTORY check, mkdirat,
the dup and F_DUPFD socket-table alias, pipe2's copy-out, and chmod. A sibling
thread of the same Proc can close the number and reuse it in the window, and
the unwind then closes the sibling's file. A re-read on 2026-10-14 found more
of the same class: chmod looks the number up a second time for the wstat, so it
can change a sibling's file; a post-install `handle_set_cloexec` by number
flags whatever the number names by then; accept and socket drop their
socket-table row by number; and `handle_close_obj` (the setup-rollback fix)
compares object pointers, so it cannot tell two dups of one object apart and
does not fix the dup sites.

## Research
Plan 9 installs last: `sysopen` calls `newfd` as its final fallible step,
and `syspipe` installs both ends in one hold with `newfd2`, after
`validaddr` has checked the user array. Linux splits the descriptor's life
in three: `get_unused_fd_flags` reserves a number (the slot is marked in
`open_fds`, its file pointer still NULL), the call does its fallible work
with the number in hand, and `fd_install` publishes, which cannot fail;
`put_unused_fd` returns an unused reservation. `do_pipe2` copies the two
numbers to user memory between the reservation and the install, accept4
copies the peer address there, and `dup2`/`dup3` onto a reserved number
answer EBUSY. Tree facts: handle slots are read and written only in
kernel/handle.c (a slot is free when its magic is 0, live when it is
HANDLE_MAGIC); specs/handles.tla models handles as a set per Proc, with no
slot numbers, so the number-reuse window sits below its abstraction.

## Options
1. Slot reservation (Linux's shape): a RESERVED slot state. Every table
   operation that scans for a free slot skips it; close, get, replace, the
   flag setters, enumeration, close-on-exec and the fork copy see it as not
   live; dup3 onto it answers EBUSY. Each site reserves, does its fallible
   work, and installs last; an unwind returns the reservation, which no
   other thread can have touched.
2. Identity unwind, reordered: no new state; each site keeps its own
   reference and unwinds with `handle_close_obj`, socket-table capacity is
   reserved before the install, and pipe installs both ends in one hold.
   Leaves pipe2 publishing and unwinding, and a window in which a sibling
   can see or dup a number the call then reports failed.

## The call
Option 1, slot reservation (operator, 2026-10-14, AskUserQuestion, the
recommended option). chmod and mkdirat take no descriptor at all (they act
on the resolved Spoor); openat checks O_DIRECTORY before the install and
installs close-on-exec in the same hold.

## Rationale
A failed call must leave the guest's table as it found it, and only a
number nobody else can see gives that without an argument about what a
sibling might do in the window. It is the shape Linux converged on for
the same race, it removes the window rather than narrowing it, and it is
local: one slot state in handle.c, which every other table operation
already treats as not live. The window is modelled in a small numbering
spec (slots, reserve, install, unreserve, a sibling's close and reuse),
with the by-number unwind as its buggy configuration.
