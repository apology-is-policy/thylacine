---
id: moc-kernel-execution
type: moc
title: "Kernel execution: Procs, Threads, and the death path"
parent: moc-kernel
created: 2026-08-01
updated: 2026-08-03
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

Six surfaces, matching the code's own seams — three for the lifecycle, three
for the loading:

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
- [[sub-kernel-exec]] — filling the empty Proc: the two load paths, the
  three-part gate deciding which segments are shared and which are private,
  and the System V startup frame.
- [[sub-kernel-elf]] — the validator ahead of all of it. Twenty-two ways to
  refuse a binary, no allocation, no lock, no address space touched.
- [[sub-kernel-image]] — the qid-keyed cache that lets two Procs running one
  binary share one set of text pages, and the eviction argument that makes
  it safe under SMP.

Creation reads as one step and is two. `rfork` produces a Proc with an
address space and nothing in it; exec is what puts a program there. The
split is why there is no `exec(2)`: both exec bodies reject a Proc that
already has mappings, so loading is something that happens to a *fresh* Proc,
exactly once. Several of exec's simplifying assumptions — single-threaded by
construction, exempt from the `vma_lock` discipline, no teardown path — are
that one decision cashed out.

## Cross-cutting

- Invariants: [[inv-i24]] (group termination atomic + exactly-once) ·
  [[inv-i9]] (no lost wake — the death-wake generalization is this area's
  hardest obligation) · [[inv-i32]] (the per-Proc resource floor) ·
  [[inv-i1]] (a Proc's Territory is its own; `rfork` clones it) ·
  [[inv-i33]] (`Proc.exe_path` is name-retention, non-load-bearing) ·
  [[inv-i36]] (file-backed exec, whose seven conditions span this area, the
  fault handler, and Stratum) · [[inv-i12]] (W^X — the ELF layer refuses a
  writable-executable segment, and exec's gate keeps writable data off the
  shared path).
- Specs: [[spec-death-wake]] (the cascade's register-then-observe, and the
  hang its buggy cfg reproduces).
- Locks: [[lock-proc-table]] — the one global lock the whole area turns on.
- Arcs: [[arc-phase2-lifecycle]] (where Proc/Thread came from) ·
  [[arc-pouch-boot]] (multi-thread Procs) · [[arc-holotype-rw]] (#809/#811 —
  the cascade) · [[arc-go-build]] (#344, #68 — what the Go toolchain broke) ·
  [[arc-pty]] (sessions, groups, the job stop) · [[arc-go-ide]] (the
  debugger stop that shares the park) · [[arc-identity-detour]] (identity,
  legates, the console roles) · [[arc-revenant]] (file-backed exec — and what
  making one memory arm able to sleep did to every lock that touches user
  memory).
- Adjacent areas: [[moc-kernel-namespace]] (`rfork` clones the Territory;
  the death path drops its last ref) · [[moc-kernel-srv]] (a dying Proc
  tombstones its `/srv` posts).
