---
id: dec-2026-11-04-switch-from-ttbr0
type: dec
title: "The switch decides from TTBR0: no V state on ASID 0, no MSR on an equal value, one ISB"
date: 2026-11-04
status: standing
decided-by: autonomous
affects: [sub-kernel-sched-smp]
created: 2026-11-04
---
## Fork

The thread-model review's E-4 and E-5, with the plan's gate on a measured cost. Measured by
`context.switch_cost` under HVF on the Mac (`11c/runs-2`, accel=hvf cpu=host): direct 107 ns, sched
168 ns, an FP save and restore 50 ns, mrs and msr TTBR0 with an ISB 24 ns, an ISB 17 ns, a `dsb ish`
9 ns.

## Taken

- One ISB, not two. Only TTBR1 memory is touched between the MSR and the FPCR write's ISB.
- No MSR when next's value equals the live one. The compare includes the ASID, so a rollover that
  gives the space a new one still writes.
- No V save or load for a thread on ASID 0. The value tested is the hardware's for prev and the
  composed one for next, so no flag can go stale, and ASID 0 is never handed out.

## Kept

- `asid_resolve` on a same-space switch: it is where this CPU takes a rollover's pending flush.
- `sched_record_space`'s DSB on a same-space switch: a stolen thread's EL0 cache maintenance needs it
  on the CPU it left, and Linux's `__switch_to` keeps one for the same reason.
- The ISB on a switch that wrote nothing: it is the second reason a first touch of a stack a peer
  mapped cannot race its map. 17 ns against a hazard no test here can show.

## Not built

The per-CPU FP owner and lazy restore. It would skip one load, 25 ns, when a thread resumes on a CPU
no other EL0 thread used since, and that path has already paid for an idle or a kernel thread. It
makes every writer of a Context's V state (`devproc`'s fpregs, exec) an owner invalidation, where a
miss hands one thread another's registers. A trap on first use is worse here: musl's memcpy uses V
registers, so most threads would trap at once.
