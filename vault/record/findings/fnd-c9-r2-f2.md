---
id: fnd-c9-r2-f2
type: fnd
round: adt-c9-r2
severity: P3
status: fixed
title: "The kernel-thread FP tests can read a preempting thread's V state"
surface: [sub-kernel-thread]
threatens: []
fixed-by: chg-2026-11-05-c9-r2
regression: "`fp.kernel_thread_holds_no_v`, `fp.user_v_survives_a_kernel_thread`"
created: 2026-11-05
---
## Prosecution

A kernel thread loads no V, so a tick that runs a thread on a space between the arrival and the read leaves that thread's registers live. Pinning stops a steal, not this. Red against a correct kernel, never green against a broken one.

## Disposition

The kernel-thread entries mask IRQs as their first statement. `thread_trampoline` unmasks before they run, so a few instructions stay open, and only to a false red: that residue is accepted and stated in the fixture's comment.
