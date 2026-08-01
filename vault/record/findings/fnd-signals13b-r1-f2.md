---
id: fnd-signals13b-r1-f2
type: fnd
round: adt-signals13b-r1
severity: P1
status: fixed
title: "A multi-thread Proc's SIG_DFL bootstrap wedged forever after the kernel refused NDFLT"
surface: [sub-pouch-signal]
threatens: [inv-i24]
fixed-by: chg-2026-05-24-p6-signals-b
created: 2026-08-01
---
## Prosecution

1. At 13a the kernel's `SYS_NOTED(NDFLT)` arm REFUSED delivery in a
   multi-thread Proc (`live_peers > 0`), because `exits()` with live
   peers extincted the kernel — cross-thread shootdown was still v1.x.
2. pouch's bootstrap SIG_DFL branch issued `NDFLT` and dropped into
   `for(;;)` on the assumption it never returns.
3. On the refusal it DOES return. The kernel keeps `t->in_handler` true,
   so per N-3 no further note is delivered to that Thread, ever.
4. The Thread is permanently wedged. Reachable by an ordinary program: a
   multi-thread Proc whose worker writes to a closed pipe, since a
   `pthread_create` child starts with `note_mask = 0`.

## Fix

Retry `NCONT` after an `NDFLT` refusal, which resumes the interrupted
code instead of wedging. Since #809 and the RW-8 fix the kernel's `NDFLT`
cascades through `proc_group_terminate` rather than refusing, so the
fallback no longer fires on the normal path — and relying on it to
swallow a terminating signal would now be the bug, which the code says in
as many words.
