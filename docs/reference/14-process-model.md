# 14 — Process model [ABSORBED INTO THE VAULT]

This document was absorbed at the proc/thread sweep
(`chg-2026-08-01-proc-thread-sweep`). Its content now lives, code-verified
and current, across three dossiers:

    vault/system/kernel/execution/sub-kernel-proc.md
    vault/system/kernel/execution/sub-kernel-thread.md
    vault/system/kernel/execution/sub-kernel-death.md

(the kproc-rooted table and the `rfork` inherit/fresh/strip ledger; the four
Thread creation shapes, the kstack + guard geometry and the `on_cpu`
protocol; and the death cascade, the ZOMBIE chokepoint, the close-at-exit
window and the shared stop park).

**What this file got WRONG by the time it was absorbed.** The interesting
part is the SHAPE, not the count. This page carried meticulously current,
correct, multi-hundred-word passages — the #68 close, the #344 multi-waiter
argument, the #80 orphan-naming, the proc-table-lock section — bolted onto a
skeleton frozen at P2-A. A reader arriving at the top learns a 2026-05
system and is handed those immaculate passages as evidence the whole page is
maintained. That is the partial-update failure the territory sweep named a
day earlier, here with a direct self-contradiction inside a five-line window:

- **Line 117**: "At v1.0 P2-Da, `exits` requires `thread_count == 1`
  (single-thread Procs only). Multi-threaded Procs require IPI-based
  termination of sibling threads (Phase 5+)." False since #809/#811 — and it
  sat FOUR LINES ABOVE the current #68 paragraph describing exactly that
  machinery in detail.
- `sizeof(struct Proc) == 296` — is 400. The `struct Proc` listing showed 11
  fields (the real struct has ~45, and `struct Thread` — listed with 8, no
  `magic` — is 1232 bytes).
- `thread_create` described as "4 pages = 16 KiB" via `THREAD_KSTACK_ORDER`;
  the real allocation is `THREAD_KSTACK_TOTAL_ORDER` = 3 = 8 pages = 32 KiB,
  half of it a guard region.
- **The two step-lists describe the exact PRE-FIX bodies of this area's two
  worst bugs.** `thread_free` is given in four steps with no
  `sched_remove_if_runnable`, no RUNNING re-check and no `on_cpu` spin — i.e.
  pre-#788, the SLEEPING-but-still-running use-after-free. `thread_switch` is
  given in six steps with no IRQ mask — i.e. pre-#101, and the doc's claim
  that the ordering "is intentional" is the same false premise the code
  comment carried and that caused the bug.
- The thread state machine was drawn in terms of `thread_block` /
  `thread_wake` — functions that do not exist; the primitives are
  `sleep` / `wakeup` — and omitted EXITING-via-die-check and the two-owner
  stop park entirely.
- The spec cross-reference named only `scheduler.tla`, though
  `death_wake.tla`, `debug_stop.tla` and `pty_stop.tla` all model this
  surface. `death_wake.tla` is the one that pins I-24.
- The Status table still read "In-kernel tests | 2 added:
  `context.create_destroy`, `context.round_trip`" and listed "EEVDF
  scheduler | P2-B", "Wait/wake | P2-B", "Work-stealing | P2-C" as future
  work — against 35 `proc.*` + 3 `thread.*` + 6 `sys_spawn.*` tests in the
  tree and all three subsystems long landed.
- The title framed the whole thing as "process model **bootstrap**", which it
  stopped being three phases ago.

The invariants live at `vault/invariants/inv-i24.md` (group termination
atomic + exactly-once + total) and `inv-i32.md` (the per-Proc resource
floor); the death-wake half of `inv-i9.md` was discharged here. The spec is
`vault/specs/spec-death-wake.md`; the lock is
`vault/locks/lock-proc-table.md`. The audit history (#811, #926, and the
three converging #68 rounds) lives as adt-/fnd- Record notes, with the
do-not-re-report preamble generated at
`vault/views/view-closed-sub-kernel-death.md`. Open debt:
`seam-exiting-tails-never-sleep`, `seam-close-flush-unbounded`,
`seam-death-cascade-smp-harness`, `seam-rfork-flags-unimplemented`,
`seam-proc-find-no-refcount`, `seam-legate-member-sweep-race`,
`seam-sak-revoke-note`.

Design scripture is unchanged: `docs/ARCHITECTURE.md` §7.2 / §7.3 / §7.4 /
§7.9 / §7.9.1 / §8.8.1, `docs/IDENTITY-DESIGN.md` §3.8 + §9.8,
`docs/PTY-DESIGN.md` §4.
