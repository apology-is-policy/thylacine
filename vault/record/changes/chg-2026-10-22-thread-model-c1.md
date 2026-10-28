---
id: chg-2026-10-22-thread-model-c1
type: chg
title: "Thread-model C1: an exited user thread is freed by the resume path of its last switch-out"
date: 2026-10-22
arc: arc-phase6-pouch
commits: ["1f7b2c03d", "71f624fad", "3d1f051e1", "49fdbb134", "a096bbedb", "6d3ac7ff7", "e119b1cf1", "b312b5bda", "5cf567528", "5be2cb5d0", "8c6f17639", "674704e27", "e4326a488", "a22cbfda7", "d61aed343"]
touched:
  - sub-kernel-thread
  - sub-kernel-proc
  - sub-kernel-death
  - sub-kernel-sched
  - sub-kernel-sched-smp
  - sub-kernel-rendez
  - sub-kernel-syscall-dispatch
  - sub-kernel-devproc
  - sub-kernel-addrspace
  - sub-kernel-asid
  - sub-kernel-mmu
  - sub-pouch-thread
  - sub-pouch-seam
established: [inv-i48]
closed: [seam-exited-threads-wait-for-a-create]
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-22
---
A chunk of the thread-model overhaul (`docs/thread-model-review.html`, chunk
C1). An exited thread used to wait for its Proc's next thread create, SQPOLL
setup, exec or `wait_pid` to be freed (the 2026-10-20 interim fix,
[[chg-2026-10-20-in-proc-reap]]). Now the CPU that switches away from an
exited user thread frees it, in the resume half of that switch
(`sched_resume_tail` -> `thread_reap_dead`, ARCH 7.3.2, [[inv-i48]]): it reads
EXITING while `on_cpu` still pins the thread, clears `on_cpu`, releases the
run-queue lock, then takes [[lock-proc-threads]] once to fold the run time and
stack watermark, clear a debug focus that names the thread, and unlink it;
the unlock is its last touch of the Proc. Nothing else frees a user thread:
the spawn-time sweep and its four callers are gone, exec waits until it is the
only thread on the list, and `wait_pid` waits for the zombie's count to read 0
under the lock and frees only the Proc. At most one exited thread per CPU is
ever counted. Spec first by operator vote: [[spec-thread-reap]]'s C1 actions
and five buggy cfgs landed before the code. Pulled in: the run counts behind
`/ctl` walked the run trees without their lock (OPEN-BUGS 2026-10-22 08:15Z);
they now sum per-band counts kept under each lock, and the runnable dump
copies under each lock. Self-found before audit: `/pouch-hello-threads`
asserted the interim timing (four joined workers still counted); its legs now
pin the free with nothing created since.
