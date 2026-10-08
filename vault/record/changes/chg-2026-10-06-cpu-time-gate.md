---
id: chg-2026-10-06-cpu-time-gate
type: chg
title: "CPU time goes owner-only, and the scheduler's counters become the system principal's"
date: 2026-10-06
arc: arc-identity-detour
commits: ["61ec2488d", "614a94c3b"]
touched:
  - sub-kernel-devctl
  - sub-kernel-devproc
  - sub-coreutils-presenters
  - sub-prowl
  - sub-diorama
  - sub-imperium
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-06
---
The trusted episode's key cadence showed in counters any session could read.
Every key typed into the attached authority wakes it, so its CPU time, the
per-CPU idle time, context switches and interrupts, and the scheduler's
runnable and park counts each moved once per key on a quiet machine
(IMPERIUM Fable pass F3). The operator voted to gate CPU time to its owner and
to restrict idle time too ([[dec-2026-10-06-cpu-time-gate]]; IMPERIUM-DESIGN
11.3 item 10, PROWL-DESIGN 3.5's amendment).

The kernel now renders `-` where a reader may not see the number. A Proc's
`cpu_ns`, in `/ctl/procs` and in `/proc/<pid>/status`, is shown to its owner or
a hostowner. The machine-wide counters in `/ctl/cpu` and `/ctl/sched` are shown
to the system principal or a hostowner ([[sub-kernel-devctl]],
[[sub-kernel-devproc]]). ps draws `-` instead of falling back to the raw text
([[sub-coreutils-presenters]]). prowl carries a withheld figure as absent: `-`
in the table, a dashed meter, an `own` total ([[sub-prowl]]). The shared boot
diorama runs as SYSTEM, so it withholds the counters from every client rather
than relay what the kernel showed it ([[sub-diorama]]). The episode's cadence
is now off the counters ([[sub-imperium]]).
