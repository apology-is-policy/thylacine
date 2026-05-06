# Phase 3 — status and pickup guide

Authoritative pickup guide for **Phase 3: Address spaces + page-fault handler + per-Proc TTBR0 + exec/userspace bringup**.

## TL;DR

Phase 3 lifts the kernel from "kernel-only multitasking" to "userspace processes running in EL0." Major deliverables: per-Proc TTBR0 (independent address spaces), page-fault handler (demand-page allocation, COW, stack growth), VMO mapping infrastructure (mmap_handle / vmo_map syscall surface), exec syscall (parse-validate-map-dispatch from P2-Ga's loader), minimal `/init` userspace process. Phase 4's Stratum 9P integration depends on Phase 3's address-space work landing first.

Per `ROADMAP.md §6`. Phase 3 entry chunk **P3-A** (R5-H F75 proc-table lock close) has landed early to establish the SMP locking discipline before cascading-rfork-based userspace patterns layer on top.

## Landed chunks

| Commit SHA | What | Tests |
|---|---|---|
| `402f5ca` | **R6-A audit close** (P3-A focused audit). 0 P0 + 0 P1 + 2 P2 + 5 P3 + 1 withdrawn. **Closed**: F105 (doc precision — wait_pid_cond invariant 1 over-stated; kproc->children IS multi-writer at v1.0, but quiescent because no test creates orphans; doc rewritten + Phase 5+ trip-hazard sharpened), F106 (cascading_rfork_stress didn't actually exercise the F75 race because it always reaped grandchildren before parent exits; added `proc.orphan_reparent_smoke` test that exercises proc_reparent_children with non-empty children — boot rforks A; A rforks B; A exits without waiting; A's reparent moves B to kproc; boot drains both via wait_pid since boot IS kproc), F107 (g_next_pid changed `int → u32` for defined modular wrap; INT_MAX guard at proc_alloc + cast to int at p->pid assignment), F111 (invariant (3) wording refined — folded into F105's doc rewrite). **Deferred with rationale**: F108 (transient pid=0 window — no v1.0 walker uses by-pid lookup; Phase 5+), F109 (wakeup-in-lock holds two locks while spinning on on_cpu — latency hazard; Phase 3+ when TTBR0 swap lengthens cpu_switch_context), F110 (zombie reparent leak under non-kproc grandparent — v1.0 has only kproc as grandparent; Phase 5+ kthread reaper). Memory: `audit_r6a_closed_list.md`. | **50/50 tests** PASS (1 new: orphan_reparent_smoke); ~295 ms boot; ~339 ms UBSan; 4/4 fault matrix; KASLR 5/5 distinct; 4 specs unchanged. |
| `9d5c271` | **P3-A: F75 close — proc-table lock**. R5-H F75 (cascading-exits SMP UAF) was deferred at Phase 2 close with explicit "mandatory at Phase 3+ when cascading rforks land" rationale. P3-A pulls forward the close: new `g_proc_table_lock` (global spinlock) guards the Proc-lineage state machine — children list mutations, parent pointer rewrites, ALIVE→ZOMBIE transitions, exit_msg/exit_status, companion Thread EXITING transition. Lock order: `proc_table_lock → r->lock` (single direction; established by `exits()` calling `wakeup(parent->child_done)` INSIDE the lock so the parent stays alive through the wakeup — without this, the parent could be reaped between lock release and wakeup, UAF). `wait_pid_cond` deliberately does NOT acquire `proc_table_lock` — would create the inverse `r->lock → proc_table_lock` order and deadlock. v1.0 single-thread-Procs invariant + wakeup→sleep release/acquire handshake provide visibility for `wait_pid_cond`'s plain reads of children list + per-child state. Phase 5+ multi-thread-Procs MUST refactor (trip-hazard documented). Self-audit found `g_next_pid++` non-atomic; fixed to `__atomic_fetch_add(...)` in same chunk (cascading rforks under SMP can have multiple Procs in proc_alloc concurrently). New tests: `proc.cascading_rfork_wait_smoke` (single iteration of 3-level lineage: boot → child → grandchild) + `proc.cascading_rfork_stress` (100 iterations; counter checks for leaks). Updated `docs/reference/14-process-model.md` with the lock discipline + Phase 5+ trip-hazard. | **49/49 tests** PASS (2 new); ~292 ms boot (production); ~392 ms UBSan; **4/4 fault matrix**; KASLR 5/5 distinct; **4 specs all TLC-clean** (unchanged — proc-table lock is impl-only). |

## Remaining work

Sub-chunk plan (refined as Phase 3 progresses):

1. ✅ **P3-A: F75 close — proc-table lock**. Landed.
2. **P3-B: per-Proc TTBR0**. Each Proc gets its own page table (TTBR0) for the user-half of address space. Kernel-half (TTBR1) stays shared. cpu_switch_context extended to swap TTBR0 on context switch (with TLB invalidation for the new ASID). New `arch/arm64/asid.c` (or similar) for ASID allocation. `struct Proc` gets a `pgtable_root` field + ASID. kproc uses a "kernel-only" TTBR0 (effectively unused for kernel code).
3. **P3-C: page-fault handler**. New `arch/arm64/fault.c` (or extend `exception.c`). Decode FAR_EL1 + ESR_EL1, classify (read-fault, write-fault, exec-fault, translation, permission), dispatch to the VMA tree. Demand-page allocation: walk VMAs, allocate physical page, map. COW: marker bit on PTE + clone-on-write protocol.
4. **P3-D: VMA tree + VMO mapping**. New `mm/vm.c` (or similar) with VMA tree per Proc + the `vmo_map` syscall surface. Integrates with Phase 2's `kernel/vmo.c` — `vmo_map(vmo_handle, vaddr, length, prot)` allocates a VMA, refs the VMO, sets up PTEs lazily on fault. Tests: small mmap-write-read-unmap cycle.
5. **P3-E: exec syscall**. Completes P2-Ga's parse-validate pipeline with map-and-dispatch. `exec(elf_blob, args)` calls `elf_load`, then for each segment creates a VMO + maps it into the calling Proc's TTBR0, sets up the user stack, ERET to EL0 entry. v1.0 supports static ELF only; PIE / dynamic linker post-v1.0.
6. **P3-F: minimal /init**. The first userspace process. Embedded ramfs at boot (Phase 4 swaps to Stratum). `/init` could be: a static binary that prints "hello" via syscall. Validates the full chain: kernel → exec → userspace → syscall → kernel.
7. **P3-G: P2-Dd pulled forward** — `ready()` / `wakeup()` send IPI_RESCHED when target CPU is idle. Closes R5-H F77 + F78 (sched-extinction-on-transient-race + idle-CPU-misses-ready). Critical for SMP scheduling fairness once userspace creates real load.
8. **P3-H: Phase 3 closing audit**. Cumulative review covering all P3 sub-chunks; Round-N spec verification; ROADMAP §6 exit-criteria checklist.

## Exit criteria status

Per `ROADMAP.md §6`, post-P3-A:

(To be filled in as Phase 3 progresses. Initial entry — only P3-A landed.)

- [ ] Userspace process runs in EL0; syscall returns to EL0.
- [ ] Static ELF from ramfs loads + runs via exec().
- [ ] Page fault handler allocates demand pages; user stack growth works.
- [ ] mmap of VMO + read/write/unmap cycle correct.
- [ ] /init starts and prints "hello" via syscall.
- [ ] R5-H F75 closed. ← **MET in P3-A**.
- [ ] R5-H F77/F78 closed via P3-G.
- [ ] No P0/P1 audit findings.

## Build + verify commands

```bash
tools/build.sh kernel
tools/test.sh
tools/test.sh --sanitize=undefined
tools/test-fault.sh
tools/verify-kaslr.sh -n 5
```

## Trip hazards

(Cumulative from Phase 1+2 plus new Phase 3 entries.)

### NEW at P3-A

100. **Lock order: `proc_table_lock → r->lock`** (P3-A). Established in `exits()` to keep the wakeup-target Proc alive through the wakeup. The reverse (`r->lock → proc_table_lock`) is **forbidden** — would deadlock against the established order. Verified by code review: `wait_pid_cond` is the only `r->lock`-holder that touches Proc-lineage state, and it deliberately reads without `proc_table_lock` (single-thread-Procs invariant at v1.0).
101. **`wait_pid_cond` lockless walk REQUIRES single-thread Procs** (P3-A). At v1.0 the parent's children list head + sibling chain is single-writer (the parent's own thread). When v1.0 lifts to multi-thread Procs (Phase 5+), `wait_pid_cond` must acquire `proc_table_lock`, AND the sleep protocol must be refactored to break the `r->lock → proc_table_lock` cycle that re-introduces. The refactor likely splits cond evaluation outside `r->lock` with an additional re-check under `r->lock` to preserve atomicity (Linux's pattern for fancy waitqueues).
102. **`g_next_pid` is atomic** (P3-A). `__atomic_fetch_add(&g_next_pid, 1, RELAXED)` — required for SMP cascading rforks where multiple Procs in `proc_alloc` from different CPUs must not collide on the same PID.
103. **Cascading-rfork tests exercise the proc-table-lock paths** (P3-A). `proc.cascading_rfork_wait_smoke` and `proc.cascading_rfork_stress` (100 iters of 3-level lineage) are the regression-protection. The F75 race is timing-dependent; tests don't deterministically trigger it. The lock + spec-level reasoning closes the bug; tests ensure no future change bypasses the locking discipline silently.

## References

- `docs/ROADMAP.md` §6 — Phase 3 binding scripture.
- `docs/ARCHITECTURE.md` §10/§11 (memory) + §16 (process address space) — Phase 3 design baseline.
- `docs/handoffs/014-p2h-r5h.md` — Phase 2 close pickup pointer.
- `docs/handoffs/015-p3a-f75.md` *(pending)* — P3-A close pickup pointer.
- `docs/reference/14-process-model.md` — Proc lifecycle reference, with the P3-A locking discipline addition.
- `memory/audit_r5h_closed_list.md` — F75 was deferred at R5-H with explicit "Phase 3+" rationale; P3-A is the closure.
