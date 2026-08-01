# 17 — SMP secondary bring-up [ABSORBED INTO THE VAULT]

Absorbed at the scheduler sweep (`chg-2026-08-01-sched-sweep`). Its
content now lives, code-verified and current, at:

    vault/system/kernel/scheduling/sub-kernel-sched-smp.md

(PSCI bring-up, the trampoline, the online/alive flag pair,
`per_cpu_main`'s full per-CPU init, the two production gates that keep
secondaries quiescent during the test phase, and the MPIDR identity
assertion), alongside the `on_cpu` protocol and the idle park it brings
each secondary into.

**What this file got WRONG by the time it was absorbed.** Same mode as
its two siblings, and here it is nearly total: the file is frozen at
**P2-Ca**, the minimum-viable-PSCI chunk, and describes secondaries that
never run kernel code.

- "run a minimal asm trampoline that flips a per-CPU online flag, and
  **park at WFI** ... For P2-Ca there's no event source — secondaries
  sleep forever (intended)." They have run `per_cpu_main` — MMU, PAC,
  VBAR, FP, per-CPU idle, `sched_init`, GIC bring-up, IPI attach — since
  P2-Cb, and they schedule real work.
- **"Why no MMU enable?"** is a whole section arguing a deferral that was
  taken the next chunk. It reads as current design rationale.
- The `smp_init()` flow says it waits on **`g_cpu_online[i]`**. It waits
  on `g_cpu_alive[i]` — the stricter flag `per_cpu_main` sets after the
  per-CPU init completes, deliberately, because that is the one that
  proves PAC/MMU/VBAR all worked. `g_cpu_online` only proves the
  trampoline ran, and the code uses the difference to say *which stage*
  failed in its diagnostic.
- The public-API block lists four symbols. `smp.h` also exports
  `smp_cpu_idx_self`, `smp_cpu_ipi_init`, `smp_boot_cpu_ipi_init`,
  `smp_resched_others`, `smp_enable_secondary_preemption`,
  `smp_bootcpu_idle_stack_top`, `g_cpu_alive`, `g_pac_keys`,
  `g_secondary_boot_stacks`, `g_bootcpu_idle_stack`,
  `g_ipi_resched_count`.
- Nothing about the two **production gates** (`#810`'s deferred timer
  arming, `sched_set_notify_enabled`) that keep secondaries quiescent
  during the deterministic in-kernel test phase — which is load-bearing:
  without the timer gate a secondary self-waking on its own tick stole a
  test thread and surfaced as `thread_free of RUNNING thread`.
- Nothing about the **MPIDR identity assertion** (`cpu_idx ==
  smp_cpu_idx_self()`), the only thing standing between a cluster-MPIDR
  board and silent per-CPU slot aliasing — now
  `vault/seams/seam-sparse-mpidr.md`.
- Nothing about `g_bootcpu_idle_stack`, the guard-paged stack cpu0's idle
  runs on since the SMP redesign, or about why `g_exception_stacks` is
  allocated and asserted but unused at runtime under uniform EL1h.

What it got RIGHT and is worth preserving as history: the PSCI protocol
detail (conduit selection, the `PSCI_CPU_ON_64` function ID, the status
codes), the trampoline's PC-relative addressing argument, and the
`dsb sy` vs `dsb ishst` reasoning — all still accurate, and all the
primary source for the dossier's bring-up section.

Design scripture is unchanged: `docs/ARCHITECTURE.md` §20, §22.2; Arm DEN
0022D (PSCI).
