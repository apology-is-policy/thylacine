---
id: chg-2026-10-05-signal7-list
type: chg
title: "signal(7)'s list: every kernel wait a listed Linux call reaches ends for a caught note"
date: 2026-10-05
arc: arc-boosty
commits: ["5d28b427", "94ba5327", "00484bb6", "bb2f284e"]
touched:
  - sub-kernel-notes
  - sub-kernel-rendez
  - sub-kernel-pipe
  - sub-kernel-cons
  - sub-kernel-thread
  - sub-kernel-poll
  - sub-kernel-proc
  - sub-kernel-torpor
  - sub-kernel-syscall-dispatch
  - sub-kernel-vivarium
  - sub-stratum-boot
  - spec-poll
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-05
---
The operator voted on 2026-09-29 that a caught note interrupts only the calls
Linux lets a signal interrupt, signal(7)'s list
([[dec-2026-09-29-caught-signal-slow-calls]]). That vote built the call's half:
the vivarium dispatcher marks a listed call `note_interruptible`. The wait's half
stayed where item 11 left it. Only the two 9P waits opted in to the caught-note
unwind, so a listed call blocked in a pipe, the console, `poll`, `wait4` or a
futex rode the note out, and `pause()` never returned for one.

Now every kernel wait a listed call can reach opts in ([[sub-kernel-notes]]), and
each unwinds with nothing consumed:
- a pipe's read and write, kept off an elected 9P reader's un-opted receive
  ([[sub-kernel-pipe]]);
- the console's three read waits and three write waits; a write that moved
  bytes returns its count, and the kernel writers' role wait stays out
  ([[sub-kernel-cons]]);
- `ppoll` and `pselect6`: the park, the verdict's own note check in Linux
  `do_poll`'s order (ready, then the note, then the deadline), and the
  timeout-only sleep that is musl's `pause()` ([[sub-kernel-poll]],
  [[spec-poll]]);
- `wait_pid_for`, whose `WAIT_PID_NOTEINTR` is mapped before `wait4`'s `ECHILD`
  ([[sub-kernel-proc]], [[sub-kernel-syscall-dispatch]]);
- the futex wait, `TORPOR_ERR_EINTR` ([[sub-kernel-torpor]]).

One predicate, `thread_caught_note_unwinds`, decides for the four caught arms in
the sleep primitives and for poll's verdict ([[sub-kernel-rendez]]). ARCH 8.8.3
names the three waits that stay out: the 9P send side, poll's settle and the
notes fd's read. The kernel restarts nothing, so a guest that relies on
`SA_RESTART` sees `EINTR` (the DEGRADED row of VIVARIUM 6.22).

Two harness defects surfaced on the way. With the change, a viv-pheno-probe
`wait4` for one child is interrupted when another child's `SIGCHLD` lands first,
which is Linux's behaviour; that failed probe leg L276 on one run, and every
blocking `wait4` in the probe now retries `EINTR` ([[sub-kernel-vivarium]]). And
joey printed only 7 bytes of the probe's failure report, which now names several
legs at once ([[sub-stratum-boot]]).

Verified before the audit: 14 kernel witnesses, which fail on a kernel without
the change and on nothing else; viv-pheno-probe L311-L318, all seven legs red
without it; nine single sabotages, each red exactly where predicted; and
`specs/check-poll.sh`, every configuration as claimed, the three new buggy ones
included.

The audit (Fable 5.1) ran two rounds. Round 1 found one P3: a frozen console
reader that a note sent back to EL0 lost its frozen standing, so its retry after
END could take the busy guard's `-1`. The thread now carries
`cons_frozen_unwound` across the unwind ([[sub-kernel-cons]],
[[sub-kernel-thread]]). The implementer's own passes found two P2s in the
console's write path. The process write tapped the renderer drain with the whole
chunk before pushing it, so a short count showed the tail twice on the renderer
once the caller sent it again. The first fix, a tap per push, let a peer's unit
land inside the chunk on the renderer. The write now taps once, after its
pushes, with what went out. That overturns a 2026-08-17 audit disposition, so
the operator chose it ([[dec-2026-10-05-console-mirror-tap-order]]). Round 2
found three P3s: that reversal, `pipe.h` claiming `-1` for EPIPE (so did
[[sub-kernel-pipe]]), and the frozen mark surviving exec. All are fixed, and
every code fix has a witness that failed on the code before it. A pre-existing
divergence in `viv_readv` and `viv_writev`, where each entry is its own
byte-core call, is owned in OPEN-BUGS.
