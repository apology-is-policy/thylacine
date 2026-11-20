---
id: chg-2026-11-20-arm-on-ipi
type: chg
title: "A secondary's first IPI after the preemption flag arms its tick: the boot-transition window closed"
date: 2026-11-20
arc: arc-tickless-idle
commits: ["977d74f3c", "df1a4dbe2"]
touched:
  - sub-kernel-sched-smp
  - sub-kernel-boot-sequence
refers-to:
  - chg-2026-11-20-newidle-pull
established: []
closed: []
opened: []
mirrors-checked:
  - "no ABI: kernel/main.c order, kernel/smp.c arm placement, KERNEL_TESTS-only hooks; no syscall, wire or on-disk change"
depth: skeletal
no-dossier-change: "the note changes no code; sub-kernel-sched-smp and sub-kernel-boot-sequence were co-staged with the code in 977d74f3c and df1a4dbe2"
created: 2026-11-20
---
Found by the newidle-pull audit's prosecutor as an observation (OPEN-BUGS 2026-11-20T03:47Z): `kernel/main.c` turned notifications on before `smp_enable_secondary_preemption()`, and a secondary armed its periodic tick only at the top of `per_cpu_main`'s loop. A push IPI's IRQ-return preemption (`preempt_check_irq` after `gic_dispatch`) dispatches the pushed thread before the loop top is reached, so a thread pushed onto a secondary in the gap, or racing that secondary's first IPI, ran with no tick until it blocked.

**The fix (977d74f3c).** The flag goes up before notifications, and one masked helper, `smp_secondary_arm_tick_once`, arms on either leg: the loop top, or `ipi_resched_handler` on a secondary, which runs before the IRQ-return preemption can dispatch what that IPI placed. After the broadcast every secondary is armed or holds an IPI that arms it. Swapping the two calls alone does not close the window (a push still races the first IPI); a timed wait for every secondary's arm would turn a host stall into a boot failure.

**The witness.** `smp.push_onto_unarmed_secondary_arms_first` raises the flag through a test hook without the broadcast, holds cpu0 masked, pushes a probe onto a secondary in WFI and has the probe read its own CPU's timer and timer-PPI enables. With the handler arm sabotaged (4fb935190) it FAILS with its message; fixed it PASSES.

**Audit r1** (Opus 5.5 max, same family, 0/0/1/5; `memory/audit_notify_after_arm_closed_list.md`): the production claim sound. F1 (P2) the witness could leave a secondary armed for later tests: now a per-CPU disarm request runs at the next arm point with the flag down, the cleanup disarms every secondary and asserts none armed, and the harness sweep names `SECONDARY-ARMED` after every test. F2-F6 (P3): settle on WFI plus no IRQ across two ticks and assert the push was the probe CPU's only IPI, read the PPI enable too, read the CPU index under the mask, seven stale comments, a 1 s window (all in df1a4dbe2).

**Leases.** q-aa1: the witness RED on 4fb935190, GREEN on 977d74f3c. q-aa3 (05:35:57Z on): the double sabotage 012b7dd04 (no IPI-path arm, no newidle pull) FAILS both witnesses with their messages, with `tests: 2520/2522 FAIL` and `EXTINCTION: kernel test suite failed` as designed; df1a4dbe2 PASSES both, `tests: 2522/2522 PASS`, boot OK, no `SECONDARY-ARMED` line in either leg (05:45:39Z); then the SMP subset gate on df1a4dbe2 (05:45:39-06:48:52Z, mac released 06:49Z), each config its own invocation: default-smp4 10/10 and ubsan-smp4 10/10 PASS, 0 corruption; the last boot (UBSan) shows both witnesses PASS, `tests: 2522/2522 PASS`, no UBSan report. Headroom under the 8 MiB window: UBSan 0x779000, 28 KiB (one page less than at 924e7ec23); default 0x714000, 432 KiB (OPEN-BUGS 2026-11-06 addendum).

**A correction to [[chg-2026-11-20-newidle-pull]].** Its suite figure, 2388 / 0, counted only verdicts printed on a test's own `[test]` line, so every test that prints a diagnostic first was missed. The kernel's own tally on 81e11e87b (q-ni1b) is `tests: 2521/2521 PASS`; the q-ni2 last boot's tally was not captured, and its log shows the witness PASS and no failure line. The leg scripts now log the kernel's tally first.

**Not run:** the full SMP matrix (held since 2026-11-02), the interactive fleet.
