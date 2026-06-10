# HOLOTYPE RW-2 — Scheduler / SMP / threads / death / wait-wake

**Status: CLOSED** (`@<close-sha>`). 0 P0 across the surface; every P1 and the
in-scope P2/P3 soundness defects fixed in-arc with regressions; the non-soundness
+ design-bearing items surfaced/registered. The #809/#811/LS-5/#926/wait_pid-v2
death machinery was prosecuted **SOUND under composition** — the live defects
were all the same shape: *preconditions that earlier chunks deferred to "when
multi-thread-per-Proc lands" were never lifted after P6 landed it.* Full finding
catalog + dispositions: `docs/holotype/00-register.md` (HT02.*). Closed-list
(do-not-re-report): `memory/audit_holotype_rw2_closed_list.md`.

## Scope + method
Tier: **FULL** on the death/wait-wake half, **DELTA** on the sched core (the
deep-smp-review redesign, model-gated by `specs/sched_alpha.tla` +
`sched_oncpu.tla`). Sub-surfaces, one Fable-max `holotype-reviewer` each (all
self-reported `claude-fable-5`, no mid-run fallback) + an Opus self-audit:
- **2A** sched core + SMP + timer (DELTA): `sched.c` (EEVDF/on_cpu/steal/place/
  switch/wakeup), `smp.c`, `context.S`, `timer.c`.
- **2B** death paths (FULL): `proc.c` (exits / group-terminate / reap / wait_pid
  v2 / #926 close / fault-terminate), `thread.c`.
- **2C** wait/wake primitives (FULL): `sched.c` sleep/tsleep, `torpor.c`,
  `notes.c`/`devnotes.c`, `poll.c`, `pipe.c`, `irqfwd.c`.

Inputs: ARCH §7.9/§7.9.1, §8.4/§8.5/§8.7/§8.8.1/§8.8.2; the closed lists
ls5/926_u6f/u7pre/811/sys_exit_group. The dynamic counterpart was the SMP gate
(default+UBSan × smp4/smp8, N=10) run on the fix state.

**The FULL re-review's value (vs the per-chunk audits):** the death/wait-wake
surface accreted FIVE layered changes (#809 → #811 → #926 → wait_pid v2 → LS-5),
each audited *in isolation*. The four P1s are precisely the **interactions** +
**expired preconditions** an isolated audit cannot see. Four prosecutors found
four disjoint finding-sets: each Fable reviewer found a P1 the Opus self-audit
missed; the self-audit found the verification-method gaps (no death-wake spec, no
deterministic multi-CPU test, the #926×LS-5 cross-layer hazard) the reviewers
missed.

## Verdict
**Sound surface, but four real P1s — all from the multi-thread-Proc lift that P6
landed but the wait/poll machinery never caught up to.** 0 P0. The death cascade
itself (the most bug-prone lineage in the tree) is airtight under composition.

## Findings + dispositions

### Round 1 (3 reviewers + self-audit)

| ID | Lens | Sev | Where | Finding | Disposition |
|---|---|---|---|---|---|
| HT02.2C-F1 | S | **P1** | `poll.c` `poll_scan_one`/`sys_poll_for_proc` | A poll stack-waiter outlived the obj ref: `poll_scan_one` `handle_put` before `tsleep` while the waiter stayed listed on the object's `poll_list`; a sibling Thread (multi-thread Proc) closing the last handle freed the object + its embedded list → `poll_waiter_list_unregister` spin-locked freed memory. The `poll.h:61-77` precondition deferred the fix to "when multi-thread lands" — P6 landed it. | **FIXED** `@e504e8b` (retain the handle ref across the sleep, release after the unregister sweep; transitively keeps a pipe ring / devsrv conn alive via the Spoor). poll.h precondition rewritten. |
| HT02.2A-F1 | S | **P1** | `sched.c` `ready_on` | The wake path never rebased `vd_t`: a thread woken onto a CPU other than where it last ran carried a stale key from a foreign per-CPU clock → tailed behind every fresh local yielder → starvation bounded only by the inter-CPU counter gap (I-17 violation + the ARCH §8.2 "reinsert at current virtual time" fidelity hole). | **FIXED** `@e504e8b` (clamp `vd_t` to the target CPU's `vd_counter`; no-op same-CPU). Regression `scheduler.ready_on_clamps_stale_vd`. |
| HT02.2B-F1 | S | **P1** | `proc.c` `wait_pid_for` + `sched.c:1331` | Two peer Threads of one Proc blocking in `wait_pid` trip `child_done`'s single-waiter assert → **whole-kernel extinction** (unprivileged DoS). The wait machinery is single-thread-parent only; never lifted at P6. | **FIXED** `@e504e8b` (per-Proc `wait_active` serialization). Regression `proc.wait_pid_concurrent_waiter_refused`. |
| HT02.2B-F2 | S | **P1** | `proc.c` `wait_pid_cond` | `wait_pid_cond`'s lockless `p->children` walk (under r->lock, never g_proc_table_lock — the P3-A choice) races a peer Thread's `proc_unlink_child`+`proc_free` reap → UAF on the freed child. Same root as F1. | **FIXED** `@e504e8b` (same `wait_active` flag serializes the reaper too — at most one Thread of a Proc in the section). |
| HT02.2B-F3 | S | **P2** | `proc.c:533` `proc_reparent_children` | Orphans reparent to `g_kproc`, which never calls `wait_pid` → unbounded permanent-zombie + 32 KiB-kstack leak per orphan (an unprivileged spawn-orphan loop → OOM). Also a scripture divergence: ARCH §7.9 step 6 says re-parent to **PID 1 (init)**; the code uses kproc (pid 0). | **SURFACED** (task #17) — design-bearing init-reaping policy (kproc-reaper kthread vs reparent-to-joey-PID1-who-reaps-any); the user's call, not auto-fixed in the audit close. Confirmed real P2, pre-existing. |
| HT02.2A-F2 | S | P3 | `sched.c:992` `sched()` | The `prev==next` branch re-inserted a RUNNING, on_cpu thread into the run tree (a latent RunqRunnable/RunqOnCpuSafe violation that would extinct the *next* picker). Dead today (pick/steal never return current). | **FIXED** `@e504e8b` (replace the body with `extinction` — fail loud at the source per §8.4.5). |
| HT02.2A-F3 | S | P3 | `sched.c:1107` `sched()` resume | The resume-path `spin_unlock(lk)` lacked the NULL guard its `sched_finish_task_switch` twin has (test-only `thread_switch` reachable → `spin_unlock(NULL)` = store to VA 0). | **FIXED** `@e504e8b` (mirror `if (lk) spin_unlock(lk)`). |
| HT02.2A-F4 | S | P3 | `smp.c` `per_cpu_main` | No cross-check that the PSCI `context_id` (stack/sched index) equals `smp_cpu_idx_self()` (MPIDR-derived, used by every runtime per-CPU access) → a sparse/cluster-MPIDR board silently aliases a peer's `CpuSched` (the #860-class corruption). A bound check can't catch aliasing. | **FIXED** `@e504e8b` (equality assert — fail loud on the first non-dense board). Advances the tracked DTB-index item (#7). |
| HT02.2A-F5 | C | P3 | `thread.h`,`sched.h`,`context.S`,ARCH | Four stale load-bearing comments: thread.h `rendez_blocked_on` *lied* about the #811 protocol ("wakeup clears" — actually the owner clears under `wait_lock`); sched.h "bootcpu idle stays FALSE forever"; context.S "per-Proc ASIDs (1..255)"; ARCH balance() "bounded steal frequency". | **FIXED** `@e504e8b` (all four corrected to as-built). |
| HT02.2A-F6 | C | H2 | `sched.c`, ARCH §8.2/§8.6/§8.7/§28 | The shipped "EEVDF" is a per-CPU monotonic yield counter — no `ve_t`, no `W_total`, `weight` set everywhere but read by nothing; tickless idle (§8.6) unimplemented (fixed 1 kHz); the IPI taxonomy (§8.7 lists 4, only IPI_RESCHED exists). | **REGISTERED** for RW-13 (reconcile ARCH §8.2/§8.6/§8.7/§28-I17 to as-built). Overlaps SA-1. Non-soundness (the equal-weight RR bounds latency *within* one CPU's clock; F1 was where the cross-CPU bound broke). |
| HT02.2A-F7 | S | P3 | `sched.c` `ready` (Place) | The model's atomic `Place` is a two-lock two-step in code (state under r->lock, enqueue under cs->lock); between, a thread is RUNNABLE/in-no-tree/!on_cpu — a window a concurrent `thread_free` could sail through. | **ACCEPTED** — 2B confirmed the reap-lifecycle excludes the live UAF (`thread_free` runs only at reap on EXITING threads; a mid-Place thread is RUNNABLE in an ALIVE Proc). Model-fidelity note; not a live defect. The exclusion is proc.c's to keep. |
| HT02.2C-F2 | C | H3 | `notes.c`,`devnotes.c` | An *inherited* (not self-opened via SYS_NOTE_OPEN) `/dev/notes` reader is default-terminated by `interrupt` instead of receiving it (the self-managing gate keys on the open syscall, not on holding the fd). | **REGISTERED** — v1.0-unreachable (no v1.0 spawn passes a notes fd to a child); arguably as-designed (opening *is* the declaration). On record before any future notes-fd-across-spawn. |
| HT02.SA-1 | C/T | H | (verification method) | The death-wake cascade — the single most bug-prone mechanism in the tree (8 historical bugs) — has **no machine-checked model**. `sched_alpha.tla` proves migration safety but models wakes only via the benign `Place`; nothing models `group_exit_msg`/`wait_lock`/`rendez_blocked_on`/death-interrupt. The project re-enabled spec-first for `sched_alpha` AND ASID rollover on exactly the "SMP-race-bearing + benefits-from-machine-checked-exploration" bar this cascade meets. | **SURFACED** for RW-13 (task #9) — weigh a `specs/death_wake.tla` (or a death-interrupt extension to `sched_alpha`). Spec re-enablement is a user vote (the prior two were). NOT a live defect (3× audited clean + prose-validated). |
| HT02.SA-2 | C | H | (test coverage) | No deterministic multi-CPU death-cascade test. Coverage is single-sleeper (`rendez.death_interrupts_sleep`, devproc kill flag, notes death-leg); the full cascade WALK + broadcast-IPI + last-out-reap at -smp>1 (the 3-way interleave) rides only the SMP gate + a racy E2E. Every death-path closed list carries this as an owed future-TSan/stress harness. | **TRACKED** (task #10) — the owed concurrent harness (shared with the Loom/9p closed lists). The empirical twin of SA-1. |
| HT02.SA-3 | S/C | P3 | `proc.c:1505` / `:1661` | The #926 fd-close-at-exit asymmetry × LS-5: a single-thread Proc terminated via `proc_group_terminate` (kill / SYS_EXIT_GROUP / **LS-5 interrupt-terminate**) defers fd-close to reap (self-documented "KNOWN ASYMMETRY"), so the exact #926 drain-before-reap pipe-EOF hang is latently re-introduced for the non-voluntary path. LS-5 (which added this path) landed *after* #926 and the F2 disposition did not cite it. | **TRACKED** (task #11) — v1.0-unreachable on a *fragile* argument (saved only by the single-threaded shell can't-drain-and-forward). The robust fix is the v1.x EXITING-protocol close-on-kill lift; #926 F2 re-recorded with the sharper reachability + LS-5 cite. |

### Round 2 (dirty-close re-audit — the FIXES themselves)
A dirty close (4 P1 + the invasive poll-lifetime change). Two Fable reviewers
re-prosecuted the fixes on `@e504e8b`. **Both returned 0 P0 / 0 P1 / 0 P2** — the
four kernel fixes (wait_active, vd_t clamp, prev==next fail-loud, resume NULL
guard) + the MPIDR assert are each explicitly SOUND. Three P3s, all fixed:

| ID | Lens | Sev | Finding | Disposition |
|---|---|---|---|---|
| HT02.R2-poll-F1 | C | P3 | The poll.h "keeps the object alive directly for a SrvConn" claim is false: the only KObj_Srv path that registers a poll waiter is the SrvService *listener*, whose `handle_acquire/release_obj` are no-ops → the retain is INERT. Safe today only because the sole registry is the immortal boot registry; a mortal per-session registry (A-5b/#827) reintroduces the round-1 UAF on the listener-poll path. | **FIXED** (poll.h comment corrected: the two real paths are KOBJ_SPOOR pipe/devsrv-conn, transitive via the Spoor ref; the listener retain is inert). A-5b seam tracked (#18). |
| HT02.R2-sched-F1 | T | P3 | `test_sched_ready_on_clamps_stale_vd`'s no-op (t2) half is a host-timing flake: key-0 (band head) on the LIVE boot CPU with preemption armed → a slice-expiry in the place→assert window dispatches t2, which re-keys → false-red on the EQ(0). | **FIXED** (mask IRQs across the place→capture→remove triplet, `spin_lock_irqsave(NULL)`; the sibling cross-CPU test instead targets a parked peer). |
| HT02.R2-sched-F2 | T | P3 | `wait_pid_concurrent_waiter_refused` pre-seeded `g_wpcw_reaped_pid` with the expected pid → the final `>0` assert is tautological in isolation (closed transitively by refused==1 + done==2, so no false-pass — cosmetic). | **FIXED** (separate `g_wpcw_expect_pid`; `reaped_pid` is a sentinel set only by the reaping worker). |

## Fixes landed
- `e504e8b` — the RW-2 fix class: 4 P1 + 3 P3 (poll ref-retention; vd_t clamp;
  wait_active serialization; prev==next fail-loud; resume NULL guard; per_cpu_main
  MPIDR assert; 4 doc corrections) + 2 regression tests.
- `<close-sha>` — the round-2 P3 fixes (poll.h listener-retain caveat; the 2
  test-quality fixes) + this report + the register/HOLOTYPE rows.

## Verified SOUND (reviewed and survived — do not re-prosecute without new code)
The #809/#811/LS-5/#926/wait_pid-v2 death machinery under the 5-layer composition:
the per-Proc-lock cascade + I-9 death-wake (both interleavings, register-then-
observe under `wait_lock`), the Option-A stack-rendez pin, lock-order
`g_proc_table_lock → wait_lock → g_timerwait → r->lock` acyclic, exactly-once
ZOMBIE / no double-zombie, the on_cpu-spin-before-thread_free, `group_exit_msg`
set-once CAS, the #926 at-exit close vs a concurrent cascade (SLEEP_INTR restores
RUNNING → -P9_E_IO → close completes → zombie once), `proc_fault_terminate`
fall-through (multi-thread fault routes the cascade), the 10 `*_INTR` arms, the
LS-5 latch lifecycle, `thread_die_pending`. The redesigned on_cpu/migration core
faithfully implements `sched_alpha.tla` (DoSwitch/FinishSwitch clear-before-unlock,
RunqOnCpuSafe, the steal/pick ASSERTs, pinned-idle-never-migrates, idle_in_wfi,
#801/F1 steal-claim). The poll wait/wake (register-then-observe across N fds), the
two-fds-one-ring release, and the timer virtual-everywhere + death-checkpoint. The
fix internals: `wait_active` cleanup-on-all-paths + the `_pad_srv` clean repurpose
(0 stale readers) + the exclusion property; the poll ref balance + double-put
freedom (handle_put no-ops on the zeroed snapshot) + no-sleep-at-the-death-tail.

## Posture at close
default **808/808 PASS** (+2 regressions), 0 EXTINCTION, boot + login E2E OK.
SMP gate (default+UBSan × smp4/smp8, N=10) on the fix state: **0 corruption**.
Both round-2 reviewers clean (0 P0/P1/P2). One tracked virtio-input QMP-injection
host-timing flake (the kernel probe skips gracefully; PASS on re-run).
