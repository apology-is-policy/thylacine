---
id: chg-2026-10-06-nanosleep-rows
type: chg
title: "The sleep rows: a Linux guest's nanosleep and clock_nanosleep sleep, and the deadline beats a pending note"
date: 2026-10-06
arc: arc-boosty
commits: ["fa8387d6", "9f187623"]
touched:
  - sub-kernel-vivarium
  - sub-kernel-syscall-dispatch
  - sub-kernel-timer
  - sub-stratum-boot
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-06
---
Until these rows a Linux guest could not sleep. `nanosleep` (101) and
`clock_nanosleep` (115) had no vivarium row, so both forwarded, and with no
supervisor FORWARD is ENOSYS. musl's `sleep()`, `usleep()` and `nanosleep()`
returned at once, so `busybox sleep 1` did not sleep and a guest that paced
itself with a sleep spun (OPEN-BUGS 2026-10-05 18:32Z).

Both are now Tier-2 shells over one sleep core, `vivarium_clock_sleep`
([[sub-kernel-vivarium]]), with the uaccess in the shells
([[sub-kernel-syscall-dispatch]]). The arguments are judged in Linux's order:
the clock, then the copy-in, then the validity rule. The clock set is derived
from `clock_gettime`'s map, which now reads the clock id's low 32 bits as Linux
does. A relative sleep counts on the monotonic clock whatever its clock. At the
deadline the expiry wins, as in Linux's `do_nanosleep`: a past or zero deadline
returns 0 even with a note pending. Only a note or a death before the deadline
is `EINTR`, with the time left in `rem`. The sleep never ends short, because
the clock, not the wait's outcome, decides each 0.

An absolute sleep on `CLOCK_REALTIME` follows the wall clock when it is set
(POSIX). The sleeper hooks the wall clock's new step list before it reads the
offset, and the re-anchor walks the list after it publishes
([[sub-kernel-timer]]; I-9, register-then-observe). The counter conversion now
saturates rather than wraps above a 1 GHz counter, so a saturated request stays
the farthest deadline.

The audit (Fable 5.1, one round, 0/0/1/3) found that a death mapped to 0 could
reach a surviving thread. The terminate latch that wakes the sleeper stays
revocable until the thread's tail, because a peer can install a handler or
ignore the note. A death before the deadline is therefore `EINTR` too. Two harness
defects surfaced on the way. The debug-probe caught-step leg merged from aux-3
failed about half the time: a child that stopped at its loop's top took no
step, so it was parked by the IRQ tail, which delivers no notes
(seam-el0-irq-tail-no-notes). joey's V-1b marker buffer cut the probe's sleep
report short ([[sub-stratum-boot]]).
