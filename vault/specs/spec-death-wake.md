---
id: spec-death-wake
type: spec
title: "death_wake.tla"
models: [sub-kernel-death]
pins: [inv-i24, inv-i9]
cfgs:
  - "death_wake.cfg -- clean: Safety + the EventuallyReaps liveness witness"
  - "death_wake_buggy.cfg -- BUGGY_OBSERVE_BEFORE_REGISTER: NoLostDeathWake violated (the #809-audit F1 hang)"
gate: "any change to the wait_lock / rendez_blocked_on / cascade protocol, or to the ZOMBIE last-out determination"
created: 2026-08-01
updated: 2026-08-01
---
## Abstraction

Written **retroactively** (HOLOTYPE RW-2 SA-1, spec-first re-enabled
2026-06-10) against already-shipped, audit-clean code — the first spec in
the tree to be applied that way. The reason it exists at all is the reason
worth recording: the tree's single most bug-prone lineage
(#788/#806/#807/#808/#860/#809/#811/#926 — eight bugs) carried **no
machine-checked model**. `sched_alpha.tla` proves migration safety but
models a wake only via the benign `Place`; nothing modeled
`group_exit_msg`, the per-Thread `wait_lock` register-then-observe, or the
cascade walk.

One Proc, N Threads, one termination. Each Thread walks a six-state PC
(`run` → `acq` → `reg` → `sleeping` → `dead`, plus the buggy path's
`obs_out`), and the cascade is `CascadeSet` (publish the flag once) followed
by per-Thread `CascadeWalk`. `wlock` is an **explicit per-Thread mutex** —
modelling the lock as genuinely held across register-and-observe is exactly
what closes the window, so abstracting it away would abstract away the
theorem.

**Deliberately beneath the model:**

- the *content* of `group_exit_msg` (status derivation, the `"ok"` collapse);
- the close-at-exit window that now precedes the ZOMBIE flip (#68/#926) and
  its `exit_close_active` suppression;
- both stop owners — a debugger or job-control stop can park a thread on its
  way to the checkpoint, and `DeathWinsOverStop` is `debug_stop.tla`'s and
  `pty_stop.tla`'s obligation, not this model's;
- the orphan rule, the legate teardown, and the console-role clears that
  ride the same chokepoint;
- torpor: futex waiters are woken by a separate pass, abstracted here into
  the one `CascadeWalk`;
- `on_cpu` and `thread_free` — the reap-side lifetime, which
  `sched_alpha.tla` and the #788 gate own.

The model therefore proves the **wake protocol**, not the whole death path.
Read [[sub-kernel-death]] for what sits above it.

## Action-site map

| Action | Site |
|---|---|
| `CascadeSet` | `proc_group_terminate` — the set-once RELEASE CAS on `p->group_exit_msg` |
| `CascadeWalk(t)` | the `p->threads` walk: `spin_lock(&peer->wait_lock)` → read `rendez_blocked_on` → `wakeup(r)` → unlock |
| `AcquireLock(t)` + `RegisterObserve(t)` | `kernel/sched.c` `sleep`/`tsleep`: register `rendez_blocked_on` under the owner's `wait_lock`, then re-check the die predicate **under the same lock** |
| `RegisterBuggy(t)` | *no site* — the counterexample shape (observe outside the lock, register after) |
| `Resume(t)` | the `*_INTR` return from `sleep`/`tsleep` and the caller's unwind |
| `RunCheckpoint(t)` | `el0_return_die_check` → `thread_exit_self` (noreturn), reached via the broadcast IPI or the periodic tick |
| `ProcReap` | `proc_become_zombie_locked`, called by the last-out thread (`proc_count_live_peers_locked == 0`) |

| Invariant | Obligation |
|---|---|
| `NoLostDeathWake` | [[inv-i9]] generalized — after the flag is published and every Thread walked, none is left sleeping-and-unwoken |
| `NoStuckSleeper` | the sharper form that does not wait for the walk to finish |
| `ZombieImpliesAllDead` | [[inv-i24]] totality — no Thread at EL0 after ZOMBIE |
| `EventuallyReaps` | the temporal witness that the hang cannot occur (`gflag ~> zombie`) |

The buggy cfg is the exact bug it was written to pin: the cascade sets the
flag and walks a Thread that is not yet SLEEPING (skipping it) in the window
between that Thread's out-of-lock check and its registration; the Thread
then sleeps with the flag set and is never woken. `NoLostDeathWake` fails.
