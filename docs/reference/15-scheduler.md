# 15 — Scheduler dispatch [ABSORBED INTO THE VAULT]

Absorbed at the scheduler sweep (`chg-2026-08-01-sched-sweep`). Its
content now lives, code-verified and current, across three dossiers:

    vault/system/kernel/scheduling/sub-kernel-sched.md
    vault/system/kernel/scheduling/sub-kernel-sched-smp.md
    vault/system/kernel/scheduling/sub-kernel-rendez.md

(the bands, the `vd_t` sort, `sched()`'s yield-vs-block contract,
placement and the preemption chain; the `on_cpu` protocol, stealing, the
pinned in-tree idle, the tickless park and secondary bring-up; and the
Rendez wait/wake primitive.)

**What this file got WRONG by the time it was absorbed.** The shape is
the point again. 1075 lines, of which **303–1075 are current, detailed
and well-maintained** — sections appended chronologically as each chunk
landed (P2-Cd, P2-Ce, P3-G, TI-4, `SYS_YIELD`, HMP, #360, prowl). Lines
**1–302 are frozen at P2-Ba**, 2026-05-05.

Nothing was ever *revised*; sections were only ever *added*. So the head
— Purpose, Public API, Implementation, Data structures, Tests, Spec
cross-reference, Error paths, Performance, Status, Known caveats —
describes a single-CPU, non-preemptive scheduler that stopped existing in
May, and hands the reader 772 lines of immaculate current prose below it
as evidence the page is maintained. This is the third consecutive sweep
to find the mode (territory, then process-model, now this).

The sharpest instance is inside **one table**. The Status table lists, as
rows a few apart, `Scheduler-tick preemption (timer IRQ -> sched) | P2-Bc`
— i.e. future work — and a bolded, current row for the #866 SMP-redesign
audit close. Batch 7 found a contradiction four lines apart; this is four
*rows* apart in the same table.

- **`thread_block` / `thread_wake`** (lines 3, 48, 251, 283) — phantom
  function names that do not exist. The primitives are `sleep` /
  `wakeup`. This is the **same** phantom pair `14-process-model.md`
  carried, which makes it a *propagated* error rather than two
  independent ones.
- **The `sched()` step-list** (line 82) gives five steps with **no IRQ
  mask, no lock, no `on_cpu`, no handoff** — the pre-#104 body. #104 was
  a permanent SMP deadlock caused by reading the per-CPU pointer before
  masking. That is the third step-list this sweep series has found
  documenting the pre-fix body of a serious bug (#788 and #101 were the
  first two).
- `sched_init(void)` — takes `unsigned cpu_idx` since P2-Cd. The Public
  API block lists 6 functions; the header exports ~30.
- The state section names `g_run_tree[]`, `g_vd_counter`,
  `g_sched_initialized` — all three gone, replaced by
  `struct CpuSched g_cpu_sched[DTB_MAX_CPUS]`.
- `kernel/sched.c (~150 LOC)` — it is 2632.
- `struct Thread`, "200 bytes total" — it is 1232.
- **A "Known caveat" that the doc itself refutes 60 lines later**:
  "Run tree is global (single-CPU) — P2-Ba uses a global `g_run_tree`
  array; P2-C makes per-CPU", directly above the section titled "Per-CPU
  dispatch (P2-Cd)".
- The caveat "IRQ-mask discipline at P2-Bc (live)" describes
  `spin_lock_irqsave(NULL)` with "at v1.0 UP there's no contention" — the
  run-queue lock has been a real contended lock since P2-Ce.
- `In-kernel tests | 2 added: scheduler.dispatch_smoke,
  scheduler.runnable_count` — the tree has 31 `scheduler.*` + 11
  `rendez.*` + 6 `tsleep.*` + 6 `smp.*`.
- The Status table lists as future work: scheduler-tick preemption,
  IRQ-mask discipline, full EEVDF math, `LatencyBound`. The first two
  landed the same day the file was written; the last two are genuinely
  still owed and are now `vault/seams/seam-eevdf-math.md`.

Registered in the vault: invariants `inv-i8`, `inv-i17` (recorded
honestly as a **design target**, not an as-built bound), `inv-i18`,
`inv-i21`; specs `spec-scheduler`, `spec-sched-oncpu`, `spec-sched-alpha`,
`spec-sched-ctxsw`, `spec-sched-tickless`, `spec-sched-rebalance`; locks
`lock-wait` / `lock-timerwait` / `lock-rendez` / `lock-runq` (the wait
chain, outermost first). Open debt: `seam-eevdf-math`,
`seam-runq-rbtree`, `seam-affinity-mask`, `seam-hmp-push`,
`seam-tickless-bare-metal`, `seam-sparse-mpidr`.

Design scripture is unchanged: `docs/ARCHITECTURE.md` §8 (§8.2–§8.6,
§8.10), `docs/TICKLESS-IDLE.md`, `docs/PROWL-DESIGN.md` §3.
