---
id: chg-2026-10-13-icache-sync-space
type: chg
title: "The address space's CPUs synchronized on request: SYS_ICACHE_SYNC_SPACE at 128, a per-CPU space record, the barrier IPI acknowledged by ticket, 128 MiB code regions"
date: 2026-10-13
arc: arc-boosty
commits: ["e06bdd38f", "297f641b5", "d2c31edbe", "c5342df5f"]
touched:
  - sub-kernel-sched
  - sub-kernel-gic
  - sub-kernel-syscall-dispatch
  - sub-kernel-syscall-abi
  - sub-kernel-vivarium
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-10-13
---
B-2e2 of the browser arc: the kernel half of the Wasm cross-CPU publish
([[dec-2026-10-13-icache-sync-space]], operator vote, superseding
[[dec-2026-10-13-wasm-space-sync]]; design [[chg-2026-10-13-b2e2-design]]).
B-2e3 puts JavaScriptCore's BBQ and OMG on it.

**What.** [[sub-kernel-sched]]: every context switch records the address
space the CPU installs (`g_cpu_space`, NULL for a kernel thread) and executes
`DSB ISH`, in `sched_install_asid_ttbr0`, which `sched()`, `thread_switch` and
`execve`'s `sched_activate_addrspace` all pass through. `sched_sync_space`
executes `DSB ISH`, reads every other CPU's record with IRQs masked, sends a
barrier IPI by ticket to each naming the space, and waits for every ticket
with IRQs on and no timeout, offering the CPU every 4096 checks.
[[sub-kernel-gic]]: the barrier IPI is acknowledged by ticket (a per-CPU
request count and done count; the handler stores the request count it reads,
then `DSB SY` so its return follows the read), `gic_synchronize_cpu` moves to
it, and the GICv3 acknowledge executes `DSB SY` after reading
`ICC_IAR1_EL1`. [[sub-kernel-syscall-dispatch]] / [[sub-kernel-syscall-abi]]:
`SYS_ICACHE_SYNC_SPACE` = 128, no arguments, no capability; `JIT_REGION_MAX`
128 MiB. [[sub-kernel-vivarium]]: the native ceiling is 128, and Linux's 128
(`restart_syscall`, a mandatory `ENOSYS` row) gets a per-number collision
assert. Mirrors: libthyla-rs `t_icache_sync_space`, `thyla_jit.h`
`thyla_jit_icache_sync_space`.

**A pre-existing race, fixed.** `gic_synchronize_cpu`'s acknowledgement was a
per-CPU count of barrier interrupts compared with the value read before the
send; an earlier sender's handler whose increment landed late satisfied a
later caller before any handler had observed its request. Its user is the ITS
drain (`gic_its_retire`), which frees an LPI lease on that witness. Found while
designing this chunk (OPEN-BUGS, P2 latent); the ticket closes it.

**What the call guarantees.** On return, every other CPU whose record named
the caller's space has run a barrier handler that observed a request this
call made after its `DSB`, and returned from it, a context-synchronization
event. The interrupt itself may have been acknowledged before the send (a
handler already running for an earlier sender can read the newer request);
the guarantee needs only the observation followed by the CSE (round 1, F1).
A CPU whose record the call missed switched in after the call's `DSB`, so
the invalidation completed before its `eret` (membarrier's store-buffering
argument; JIT-ON-WX-DESIGN B-2 item 4).

**Alternatives.** The selector (a mode of `SYS_ICACHE_SYNC`, or a flags word
in `x2`) and the number (132, leaving room for a peer branch) are in the
decision and the design note. A timeout on the wait was rejected: it would let
a descheduled vCPU turn a sync into a fatal error in JavaScriptCore. The ASID
allocator's per-CPU active slot could not serve as the record, since a
rollover zeroes it while the CPU runs on.

**Verification.** Self-audit S1 (`d2c31edbe`): the handler's return could
precede its read of the request, a CSE not waiting for an earlier load; a
`DSB SY` after the store orders them. Audit Fable 5.1 round 1: 0/0/0/4, clean
(`c5342df5f`): F1 the proof text claimed acknowledgement after the send
where the code proves observation then CSE; F2 a deterministic wait leg (the
target holds IRQs masked to a deadline the call must outwait); F3 an IRQs-on
assert and a compile-time GICv2 target bound; F4 the doubled non-preemptible
`SYS_ICACHE_SYNC` walk, closed with its reason in [[sub-kernel-syscall-dispatch]].
The first suite boot hung in the new test, whose fixture assumed a cross-CPU
`ready_on` wakes an idle peer during the suite (it does not); fixed in the test.
REDs, each red at its named witness: native ceiling 127 and number 129 (build);
a NULL record, no record compare, an early ticket, a 64 MiB limit (suite); no
wait loop (the hold leg, with the latency-only checks neutralized), a GICv2
bound of seven (build), the call made with IRQs masked (the assert). Land gates
on `c5342df5f`: CI image ls-jsc 45/45 twice (JavaScriptCore unchanged, its
kernel changed); suite 1993/1993; test-fault 8/8; ci-smp-gate N=10 50/50
(default smp1/4/8, UBSan smp4/8), 0 corruption, lowest free disk 5816 MiB;
check-production --all PASS.
