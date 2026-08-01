---
id: moc-kernel-execution
type: moc
title: "Kernel execution: Procs, Threads, and the death path"
parent: moc-kernel
created: 2026-08-01
updated: 2026-08-01
---
The Plan 9 Proc/Thread pair and the lifecycle over them. A **Proc** owns an
address space, a Territory, a handle table, an identity, and a list of
**Threads**; a Thread owns a register context, a kernel stack, and a run
state. Creation is `rfork` (Plan 9's fork/thread unifier — only `RFPROC` is
implemented); termination is a cascade, not a call.

The area's centre of gravity is not creation but **death**, and that is a
deliberate reading of the code rather than a stylistic choice: the file is
3743 lines of which the lifecycle-teardown machinery — `exits`,
`thread_exit_self`, `proc_group_terminate`, the ZOMBIE chokepoint, the
EL0-return die-check, the stop park, the orphan rule — is the majority, and
it carries the single most bug-prone lineage in the tree
(#788/#806/#807/#808/#860/#809/#811/#926/#68). Every one of those bugs was a
wake that did not arrive, a pointer freed while a peer still ran on it, or a
state observed a moment too early.

Three surfaces, matching the code's own seams:

## Children

- [[sub-kernel-proc]] — the Proc: the kproc-rooted table, lineage
  (parent/children/sibling), `rfork`'s inherit/fresh/strip ledger, the
  identity + console-role fields, the I-32 resource floor, POSIX
  sessions/groups, and `wait_pid_for`.
- [[sub-kernel-thread]] — the Thread: creation and the three trampoline
  shapes, the 16 KiB kstack + its guard region, the `on_cpu` protocol that
  makes `thread_free` safe, and the tid space.
- [[sub-kernel-death]] — the death path: the ZOMBIE chokepoint, the #811
  universal death-wake, the #68/#926 close-at-exit window, orphan
  reparenting, and the stop park death shares with the debugger and job
  control.

## Cross-cutting

- Invariants: [[inv-i24]] (group termination atomic + exactly-once) ·
  [[inv-i9]] (no lost wake — the death-wake generalization is this area's
  hardest obligation) · [[inv-i32]] (the per-Proc resource floor) ·
  [[inv-i1]] (a Proc's Territory is its own; `rfork` clones it) ·
  [[inv-i33]] (`Proc.exe_path` is name-retention, non-load-bearing).
- Specs: [[spec-death-wake]] (the cascade's register-then-observe, and the
  hang its buggy cfg reproduces).
- Locks: [[lock-proc-table]] — the one global lock the whole area turns on.
- Arcs: [[arc-phase2-lifecycle]] (where Proc/Thread came from) ·
  [[arc-pouch-boot]] (multi-thread Procs) · [[arc-holotype-rw]] (#809/#811 —
  the cascade) · [[arc-go-build]] (#344, #68 — what the Go toolchain broke) ·
  [[arc-pty]] (sessions, groups, the job stop) · [[arc-go-ide]] (the
  debugger stop that shares the park) · [[arc-identity-detour]] (identity,
  legates, the console roles).
- Adjacent areas: [[moc-kernel-namespace]] (`rfork` clones the Territory;
  the death path drops its last ref) · [[moc-kernel-srv]] (a dying Proc
  tombstones its `/srv` posts).
