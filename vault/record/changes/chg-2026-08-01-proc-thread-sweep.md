---
id: chg-2026-08-01-proc-thread-sweep
type: chg
title: "The proc/thread sweep: the death lineage, and a doc that contradicts itself four lines apart"
date: 2026-08-01
arc: arc-vault
commits: []
touched:
  - inv-i9
  - arc-holotype-rw
  - arc-go-build
established:
  - moc-kernel-execution
  - sub-kernel-proc
  - sub-kernel-thread
  - sub-kernel-death
  - inv-i24
  - inv-i32
  - spec-death-wake
  - lock-proc-table
  - arc-phase2-lifecycle
  - arc-pty
  - seam-exiting-tails-never-sleep
  - seam-close-flush-unbounded
  - seam-death-cascade-smp-harness
  - seam-rfork-flags-unimplemented
  - seam-proc-find-no-refcount
  - seam-legate-member-sweep-race
  - seam-sak-revoke-note
  - view-closed-sub-kernel-death
closed: []
opened:
  - seam-rfork-flags-unimplemented
  - seam-proc-find-no-refcount
  - seam-legate-member-sweep-race
  - seam-sak-revoke-note
mirrors-checked: []
depth: rich
---
## What

Sweep batch 7 — the execution area (`kernel/proc.c` 3743 + `proc.h` 1922 +
`thread.c` 711 + `thread.h` 671 + `specs/death_wake.tla` and its two cfgs,
all read in full per the standing sweep bar).

Present: a new area MOC and three dossiers split along the code's own seams
— [[sub-kernel-proc]] (the kproc-rooted table, the `rfork` inherit/fresh/
strip ledger, the I-32 floor, sessions and groups, `wait_pid_for`),
[[sub-kernel-thread]] (the four creation shapes, the kstack + guard
geometry, the `on_cpu` protocol), and [[sub-kernel-death]] (the cascade, the
ZOMBIE chokepoint, the close-at-exit window, the shared stop park). Plus
[[inv-i24]], [[inv-i32]], [[spec-death-wake]], and [[lock-proc-table]] — the
one global lock the whole area turns on.

Record: two arcs ([[arc-phase2-lifecycle]], [[arc-pty]]), eight retro chgs,
five audit rounds, and their findings. Seven seams.

[[inv-i9]]'s standing backfill hook is DISCHARGED on its death-wake leg:
[[spec-death-wake]] joins its validators and [[sub-kernel-death]] its
guards.

`docs/reference/14-process-model.md` STUBBED (absorbed).

## Why

The recorded batch-7 alternative was pouch; execution won because it is the
area whose vault entry pays most. The death lineage
(#788/#806/#807/#808/#860/#809/#811/#926/#68 — nine bugs) is the most
bug-prone in the tree, and the reason is structural rather than incidental:
death is the one operation that dismantles a Proc's state while other CPUs
may still be reading it, where a wake arriving a moment too late is
indistinguishable from one that never arrives, and where the failure mode is
a HANG rather than a crash. Every one of those nine was a lost wake, a
pointer freed while a peer still ran on it, or a state observed too early.

Reading the code surfaced the thing a doc cannot: the area's centre of
gravity is teardown, not creation. Of 3743 lines, the majority is the
lifecycle-teardown machinery, and the two most recent P1s in it
([[fnd-68-r1-f1]], [[fnd-68-r2-f1]]) were the SAME class — a premise about
when the death machinery is armed that turned out false. `group_exit_msg` is
set on every `SYS_EXIT_GROUP`, a clean `exit_group(0)` included, so "the
flag is set" never meant "killed"; and the `exits()` close site is reachable
with the LS-5 terminate latch deliberately still armed. Both premises were
stated confidently in round-1 reasoning and falsified in round 2.

## Verification

The staleness verdict is a SECOND instance of the mode batch 6 named — and
a sharper one. `14-process-model.md` carries meticulously current, correct,
multi-hundred-word paragraphs on #68, #344 and #80, and a SKELETON frozen at
P2-A around them:

- line 117 asserts "`exits` requires `thread_count == 1` (single-thread
  Procs only). Multi-threaded Procs require IPI-based termination of sibling
  threads (Phase 5+)" — false since #809/#811, and it sits FOUR LINES ABOVE
  the current paragraph that describes exactly that machinery in detail;
- `sizeof(struct Proc) == 296` (now 400);
- the thread state machine is drawn in terms of `thread_block`/`thread_wake`
  — functions that do not exist (the primitives are `sleep`/`wakeup`) — and
  omits EXITING-via-die-check and the stop park entirely;
- the spec cross-reference names only `scheduler.tla`, though
  `death_wake.tla`, `debug_stop.tla` and `pty_stop.tla` all model this
  surface;
- the Status table still reads "In-kernel tests | 2 added" and "EEVDF
  scheduler | P2-B" as future work, against 35 `proc.*` + 3 `thread.*` +
  6 `sys_spawn.*` tests in the tree.

So a reader arriving at the top learns a P2-A system and is handed one
current paragraph as evidence the whole page is maintained. That is the
partial-update failure named in batch 6, now with a direct self-contradiction
inside a five-line window.

Every dossier claim traced to current source rather than to the closed
lists, which are 17-61 days old. Spot-checks that mattered: the tests
enumerated from `kernel/test/test.c`; every retro SHA verified with
`git rev-parse`; the `struct Proc`/`struct Thread` sizes and offsets read
from the live `_Static_assert`s.
