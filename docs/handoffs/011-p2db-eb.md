# Handoff 011 — P2-Db through P2-Eb landed (process model hardened, namespace live)

**Date**: 2026-05-06
**Tip**: `0a6e930` (P2-Eb hash fixup)
**Phase**: Phase 2 in progress. P2-A through P2-Eb landed. The kernel now has full multi-process lifecycle (rfork + exits + wait) for single-thread Procs with per-thread kstack guard pages, the wait_pid → thread_free SMP race closed via on_cpu spin, the P2-Cg-derived `try_steal` rotate-start refinement, and **Plan 9 namespace primitives live (Pgrp + bind + unmount + rfork(RFPROC) namespace clone) with `namespace.tla` proven for cycle-freedom**.

---

## Pickup pointer

**Read in this order:**

1. `CLAUDE.md` (root) — operational framework. Mandatory.
2. **This file** — canonical pickup at P2-Eb close.
3. `docs/handoffs/010-p2cg-da.md` — predecessor (P2-Cg + P2-Da).
4. `docs/handoffs/009-p2ce-cf.md` — P2-Ce / Cf.
5. `docs/handoffs/008-p2cc-cd-cdc.md` — P2-Cc..Cdc.
6. `docs/VISION.md` + `docs/ARCHITECTURE.md` + `docs/ROADMAP.md` — binding scripture.
7. `docs/phase2-status.md` — sub-chunk plan with all landed rows + cumulative trip-hazards.
8. `docs/REFERENCE.md` snapshot + `docs/reference/14-process-model.md` (rfork + exits + wait_pid) + `docs/reference/15-scheduler.md` + `docs/reference/16-rendez.md` + `docs/reference/17-smp-bringup.md` + **`docs/reference/18-namespace.md`** (new at P2-Eb).
9. `specs/scheduler.tla` (TLC-clean at 10188 states; 3 buggy configs) + `specs/namespace.tla` (new — TLC-clean at 625 states; 1 buggy config) + `specs/SPEC-TO-CODE.md`.
10. `memory/project_active.md` — quick state summary.

---

## What landed since handoff 010

### Session arc

This handoff bundle covers **two sub-chunks per ROADMAP** (P2-Db, P2-Dc, P2-Ea, P2-Eb landed cleanly) plus **two refinement commits** (try_steal rotate-start; wait_pid on_cpu spin) and **one investigation that revealed two SMP races** (P2-Dd attempt, reverted with one race fixed defensively, one deferred). Net: the multi-process model is hardened to production posture; the namespace section is open-ended at the impl level for the eventual ramfs init flow.

### P2-Db — 1000-iter rfork+exits+wait stress

`5ddcb04` substantive + `e93a65d` hash fixup.

- `proc.rfork_stress_100` upgraded to `proc.rfork_stress_1000` (10× iterations).
- Batched 8-rfork → 8-reap pattern fans out the run-tree depth before yielding (exercises wait_pid Rendez under sustained load).
- Per-CPU run counters track which CPU executed each child; sum-must-equal-ITERS verifies no double-reap or missed run.
- Boot growth +36 ms (production 196→232 ms) for the 900 extra stress iterations — well under the 500 ms VISION §4 budget.
- Cross-CPU placement assertion **lifted** at the test level: at this phase secondaries don't run their own timer IRQs (per-CPU timer init not wired) and rfork doesn't send IPI_RESCHED to peers, so children placed by ready() in the local run tree are typically picked by the same CPU before secondaries can steal. A dedicated cross-CPU test lands once either secondary timers OR rfork-driven IPI_RESCHED is wired (P2-Dd; deferred — see "Open follow-ups" below).

### P2-Dc — per-thread kstack guard pages

`2a15cc1` substantive + `3f526a3` hash fixup.

- Closes **P2-A trip-hazard #1**: kstack overflow detection.
- thread_create now allocates 8 pages (32 KiB at order=3) instead of 4 (16 KiB at order=2). Upper 4 pages = 16 KiB usable kstack; lower 4 pages = 16 KiB no-access guard.
- **New MMU API in `arch/arm64/mmu.{h,c}`** (~150 LOC):
  - `mmu_set_no_access(pa)` / `mmu_restore_normal(pa)` — single-page.
  - `mmu_set_no_access_range(pa, n)` / `mmu_restore_normal_range(pa, n)` — batched, single TLB flush per call. Substantially faster at thread_create / thread_free volume (~10× speedup at 1000 iterations).
  - On-demand L2-block→L3-table demote: first guard insertion in a 2 MiB block allocates a fresh L3 from buddy + populates with 512 page descriptors reproducing the original block's mapping + break-before-make replaces the L2 entry. Subsequent guards in the same block reuse the L3.
- `addr_is_stack_guard()` in `arch/arm64/exception.c` extended to also check the current thread's kstack guard region (in addition to the boot-stack guard from start.S). Stack overflow → translation fault → `extinction("kernel stack overflow")` with FAR pointing into the guard.
- New `THYLACINE_FAULT_TEST=kstack_overflow` provoker in `kernel/fault_test.c`: spawns a thread, recurses with 1 KiB volatile frames; ~16 frames overflow the 16 KiB usable region into the guard.
- `tools/test-fault.sh` matrix grows to **4/4** (canary, W^X, BTI, **kstack_overflow** — new).
- New `THREAD_KSTACK_TOTAL_*` and `THREAD_KSTACK_GUARD_*` constants in `<thylacine/thread.h>`.

### `81e5f74` — try_steal rotate-start

Refinement landed alongside (closes P2-Ce trip-hazard #68).

- Single global atomic counter `g_try_steal_rotate` — try_steal's start CPU rotates per call.
- `for (k = 0; k < DTB_MAX_CPUS; k++) { i = (base + k) % DTB_MAX_CPUS; ... }` instead of the old fixed-order scan.
- Spreads spin_trylock contention across peers; breaks the pathological "everyone hits CPU 0 first" pattern.
- Forward-looking at v1.0 (only boot calls try_steal); load-bearing once per-CPU timer integration lands.

### `9b8761f` — wait_pid spins on ct->on_cpu before thread_free

Defensive SMP hardening uncovered during the P2-Dd investigation.

- exits() on a child sets state=EXITING and yields via sched(); the destination CPU's resume code (sched_finish_task_switch in trampoline OR the post-cpu_switch_context block in sched()) clears `ct->on_cpu` via `cs->prev_to_clear_on_cpu`.
- Without spinning, thread_free could race with the destination CPU still mid-switch — TPIDR_EL1 on that CPU briefly points at ct around set_current_thread(next), and freeing ct's slot mid-window means the next sched() on that CPU reads a clobbered magic ("sched() with corrupted current").
- Mirrors the on_cpu spin in wakeup() that closed the wait/wake race in P2-Cf (trip-hazard #16). Same protocol, applied to reap rather than wake.
- `__atomic_load_n` with `__ATOMIC_ACQUIRE` to pair with `__ATOMIC_RELEASE` stores in sched's resume code.
- Latent at v1.0 (secondaries don't run threads); becomes load-bearing once per-CPU timer integration lands.

### P2-Dd attempt (reverted) — what we learned

I made two attempts to land per-CPU timer init in `per_cpu_main`. Both reverted; findings carried forward:

1. **First failure**: `proc.rfork_stress_1000` extincts with `sched() with corrupted current` under per-CPU timers — race between wait_pid → thread_free and the destination CPU's resume code. **Fixed** defensively by the on_cpu spin (`9b8761f`).

2. **Second failure** (after the on_cpu fix): `scheduler.preemption_smoke` extincts with `EC=0 unhandled sync exception 0x02000000` (Unknown reason) under per-CPU timers. EC=0 typically means the CPU branched to an undefined instruction. Likely cause: SP_EL1 / SPSel interaction during cross-CPU preempt — `cpu_switch_context`'s `mov sp, x9` while SPSel=1 (in exception context) modifies SP_EL1, and some code path doesn't restore it correctly after a cross-CPU thread migration. **Still open** — needs focused debug session (see "Open follow-ups").

3. **Lessons**: the per-CPU timer infrastructure (`timer_init_per_cpu` API + g_ticks-only-on-cpu0 in handler) is correct in isolation. Wiring it into `per_cpu_main` exposes latent SMP races that were always present but unreached because secondaries never ran threads. The cross-CPU work distribution path (whether via per-CPU timer OR rfork-driven IPI_RESCHED) needs careful debugging before it can land.

### P2-Ea — namespace.tla spec

`49d5e7f` substantive + `2b46e8d` hash fixup.

- New spec `specs/namespace.tla` (~165 LOC TLA+) + `namespace.cfg` + `namespace_buggy.cfg`.
- Plan 9 `bind` modeled as a directed graph: edge `dst -> src` exists iff `src \in bindings[p][dst]` in proc p's namespace. Walking dst yields src per ARCH §9.1.
- Five spec actions: `Init`, `Bind(p, src, dst)`, `BuggyBind(p, src, dst)` (skips cycle check), `Unbind(p, src, dst)`, `ForkClone(parent, child)`.
- Primary state invariant `NoCycle`: for every (proc, path), the path is not reachable from its own bindings via the transitive closure (ARCH §28 I-3).
- `WouldCreateCycle(p, src, dst) == src = dst \/ dst \in Reachable(p, {src})` — adding edge `dst -> src` is rejected if `dst` is already reachable from `src` (would close the loop `src -> ... -> dst -> src`).
- TLC-clean at `Procs = {p1, p2}, Paths = {a, b, c}` — 625 distinct states / depth 7. `namespace_buggy.cfg` (BUGGY_CYCLE=TRUE) produces a 2-bind cycle counterexample at depth 4 / 95 states.
- Isolation (ARCH §28 I-1) structural in the data model: bindings[p] and bindings[q] for p ≠ q are independent function values; no action updates two procs simultaneously. RFNAMEG (shared namespace) deliberately not modeled at this phase.
- `specs/SPEC-TO-CODE.md` updated mapping each action + invariant to its impl site.

### P2-Eb — kernel namespace impl

`f98b602` substantive + `0a6e930` hash fixup.

- New header `kernel/include/thylacine/pgrp.h` (~130 LOC). New impl `kernel/namespace.c` (~150 LOC).
- `struct Pgrp { u64 magic; int ref; int nbinds; struct PgrpBind binds[8] }` — 80 bytes total. Magic at offset 0 (SLUB freelist write clobbers on free; double-free defense). `_Static_assert` pins the size.
- API: `pgrp_init` (boot-time SLUB cache + kpgrp), `pgrp_alloc`, `pgrp_clone(parent)` (deep copy; models spec's `ForkClone`), `pgrp_ref` / `pgrp_unref` (refcount; rfork(RFNAMEG) sharing path is forward-looking, ref always = 1 at v1.0), `bind` / `unmount`.
- `bind` returns 0 / -1 (cycle) / -2 (duplicate) / -3 (full) / -4 (self-bind). Cycle detection is a fixed-point reachability iteration over existing edges; matches `specs/namespace.tla::WouldCreateCycle` exactly. O(N²) worst case at PGRP_MAX_BINDS=8 → 64 inner iterations.
- `struct Proc` grew **80 → 88 bytes** (added `struct Pgrp *pgrp`). `_Static_assert` updated.
- Bootstrap order in `main.c::boot_main`: slub_init → **pgrp_init** (NEW) → proc_init → thread_init.
- `rfork(RFPROC)` extension in `kernel/proc.c::rfork`: calls `pgrp_clone(parent->pgrp)` for the child's namespace; OOM rolls back the proc allocation.
- `proc_free` extension: calls `pgrp_unref(p->pgrp)` before slab release. At v1.0 each Proc has ref=1 on its private pgrp, so pgrp_unref frees the slot.
- 3 new tests in `kernel/test/test_namespace.c`:
  - `namespace.bind_smoke` — alloc + bind two non-cyclic edges + idempotent rebind detection (-2) + unmount round-trip.
  - `namespace.cycle_rejected` — build chain `a -> b -> c` via two binds; attempt cycle-closing bind; verify -1; bind table unchanged after rejection. Self-bind 5 → 5 rejected with -4.
  - `namespace.fork_isolated` — pgrp_clone deep-copy verification: parent and child evolve independently after the clone.
- New per-subsystem reference doc `docs/reference/18-namespace.md` covers full public API + impl + spec mapping + caveats (PGRP_MAX_BINDS rationale, RFNAMEG deferral, mount-union flags, 9P-server mount, RB-tree refactor).

**Notable design call**: PGRP_MAX_BINDS = 8 (not 32). SLUB's KP_ZERO byte-loop (unoptimized `for (i=0; i<actual_size; i++) q[i]=0;`) on a 272-byte struct (32-entry binds[]) inflated `proc.rfork_stress_1000` boot time past the 500 ms VISION §4 budget under QEMU emulation (~1000 ms boot). 8-entry binds[] keeps the struct at 80 bytes and keeps boot at ~270 ms. Phase 5+ growable RB-tree refactor when bind count justifies. Documented in 18-namespace.md.

---

## Current state at handoff

- **Tip**: `0a6e930`.
- **Phase**: Phase 2 in progress. P2-A → P2-Eb landed. **P2-F (handles + VMO + new spec `handles.tla`)** is the recommended next sub-chunk per ROADMAP.
- **Working tree**: clean (only `docs/estimate.md` + `loc.sh` untracked, pre-existing).
- **`tools/test.sh`**: PASS. **28/28 in-kernel tests** (3 new at P2-Eb: `namespace.bind_smoke`, `namespace.cycle_rejected`, `namespace.fork_isolated`); ~270 ms boot (production), ~320 ms (UBSan).
- **`tools/test.sh --sanitize=undefined`**: PASS.
- **`tools/test-fault.sh`**: PASS (4/4: canary_smash, wxe_violation, bti_fault, **kstack_overflow** new at P2-Dc).
- **`tools/verify-kaslr.sh -n 5`**: PASS (5/5 distinct).
- **Specs**:
  - `specs/scheduler.tla` TLC-clean at 10188 distinct states; 3 buggy configs (NoMissedWakeup / NoDoubleEnqueue / IPIOrdering).
  - **NEW `specs/namespace.tla`** TLC-clean at 625 distinct states; 1 buggy config (NoCycle counterexample at depth 4).
- **In-kernel tests**: 28. New since handoff 010: namespace.bind_smoke, namespace.cycle_rejected, namespace.fork_isolated.
- **LOC**: ~7400 kernel/asm + ~785 TLA+ in `specs/` (~620 scheduler.tla + ~165 namespace.tla) ≈ ~8185 LOC total.
- **Kernel ELF**: ~316 KB debug.
- **Open audit findings**: 0. R5 audit deferred to Phase 2 close.

---

## What's NEXT

### P2-F: handle table + VMO + `handles.tla`

Per ARCH §18-19 + §28 invariants I-2, I-4, I-5, I-6, I-7. Spec-first per CLAUDE.md.

**`specs/handles.tla` invariants**:
- I-2: capability set monotonically reduces (rfork only reduces, never elevates).
- I-4: handles transfer between processes only via 9P sessions (no direct-transfer syscall).
- I-5: KObj_MMIO, KObj_IRQ, KObj_DMA cannot be transferred (hardware handles non-transferable).
- I-6: handle rights monotonically reduce on transfer.
- I-7: VMO pages live until last handle closed AND last mapping unmapped (refcount + mapping lifecycle).

Phase 4's 9P client lands the actual transfer mechanism; at v1.0 P2-F the spec models it abstractly (a placeholder action that's only enabled in the structurally-correct contexts).

**`kernel/handle.c` impl** (~200 LOC):
- struct Handle (typed kernel object). Per-Proc handle table.
- handle_alloc(type, rights, obj), handle_close(h), handle_dup(h, new_rights) (rights monotonicity).
- handle_kind enum: `KObj_Process`, `KObj_Thread`, `KObj_VMO`, `KObj_MMIO`, `KObj_IRQ`, `KObj_DMA`, `KObj_Chan`, `KObj_Interrupt`.

**`kernel/vmo.c` impl** (~150 LOC):
- struct VMO (memory object). Refcount + mapping count.
- vmo_create(size), vmo_ref, vmo_unref, vmo_map (increments mapping count), vmo_unmap (decrements). Free pages when last handle closed AND last mapping unmapped.

**Tests**:
- `handles.alloc_smoke`: handle_alloc + handle_close round-trip; counter sanity.
- `handles.rights_monotonic`: handle_dup with reduced rights succeeds; with elevated rights fails.
- `handles.hw_non_transferable`: attempt to transfer a KObj_MMIO handle — fails statically.
- `vmo.lifecycle`: create + map + unmap + close round-trip; pages freed exactly once.
- `vmo.refcount`: multiple maps, multiple closes, in arbitrary order; pages freed at correct boundary.

Best-case 100-150k tokens for spec; 100k+ for impl.

### After P2-F

- **P2-G**: ELF loader + minimal init. `arch/arm64/elf.c` rejects RWX segments per W^X invariant. `init/init-minimal.c` is the first userspace process; UART debug shell.
- **P2-H**: Phase 2 closing audit. R5 covers cumulative P2-A..P2-G surface. SPEC-TO-CODE.md fully populated. ROADMAP §5.2 exit criteria all met.

R5 audit at Phase 2 close covers all P2 sub-chunks cumulative.

---

## Open follow-ups

- **U-30 (kstack zeroing on free)**: deferred at P2-A R4 F48. KP_ZERO is the documented escape hatch.
- **R5 audit**: covers P2-B + P2-C + P2-D + P2-E cumulative. Run at Phase 2 close.
- **LatencyBound liveness spec**: deferred to Phase 2 close. Requires weak fairness + explicit Slice variable.
- **Full EEVDF math**: deferred — meaningful only with weight ≠ 1 (Phase 5+).
- **P2-Dd: per-CPU timer + cross-CPU work distribution**:
  - **Open SMP race #1** (DEFENSIVELY FIXED by `9b8761f`): wait_pid → thread_free racing with destination CPU's resume code. Hardened via on_cpu spin in wait_pid.
  - **Open SMP race #2** (UNFIXED): `scheduler.preemption_smoke` extincts with `EC=0 unhandled sync exception 0x02000000` under per-CPU timers. Likely SP_EL1 / SPSel interaction during cross-CPU preempt (cpu_switch_context's `mov sp, x9` while SPSel=1 modifies SP_EL1; some code path doesn't restore correctly after cross-CPU thread migration). Needs focused debug session — try adding diagnostic `uart_puts` in vectors.S exception entry/exit + sched()'s save/restore path to pinpoint where SP_EL1 goes stale.
  - Path forward: either fix race #2 then enable per-CPU timer init in `per_cpu_main`, OR adopt the alternative "IPI_RESCHED on rfork" approach (sends one SGI when ready() puts a thread in the local tree, prompting peers to wake-and-steal) which has a smaller scope and may avoid the race.
- **Multi-thread Procs (RFMEM-style)**: Phase 5+ with the syscall surface. Requires IPI-based termination of sibling threads on exits().
- **Userspace rfork-from-syscall split**: P2-G with ELF loader. Parent gets PID, child gets 0 via register-set tweak in syscall-return path.
- **IPI_TLB_FLUSH/HALT/GENERIC**: deferred until use-cases arrive (TLB shootdown for namespace rebind in Phase 5+).
- **Per-IPI-type ordering refinement**: when multiple IPI types coexist (P5+), ordering refines to per-(src, type, dst) FIFO. Today's per-(src, dst) version is sound while all IPIs are equal-priority + same-type.
- **Pi 5 / multi-cluster Aff{1,2,3}**: Phase 7 hardening pass — current ICC_SGI1R_EL1 encoding pinned for QEMU virt's flat-Aff0 cluster.
- **Orphan reaping at Phase 5+**: kproc adopting orphans is fine at v1.0 (no test scenario stresses it); Phase 5+ retargets to PID 1 (init).
- **Namespace P2-Eb deferrals (per `docs/reference/18-namespace.md`)**:
  - **RFNAMEG (shared namespace)**: Phase 5+ syscall surface. struct Pgrp.ref field exists for forward sharing; always 1 at v1.0.
  - **Mount-union flags (MREPL / MBEFORE / MAFTER / MCREATE)**: Phase 5+. v1.0's bindings[p][dst] is a SET (no ordering).
  - **`mount` (9P-server-attaching variant)**: Phase 4 (9P client landing).
  - **Growable RB-tree** (replacing fixed `binds[8]`): Phase 5+ when bind count justifies.
  - **`path_id_t` as `struct Chan *`**: Phase 4+ with 9P + Chan integration. v1.0 uses `u32` numeric IDs.

---

## Verify on session pickup

```bash
git log --oneline -7
# Expect: 0a6e930 P2-Eb: hash fixup / f98b602 P2-Eb: kernel namespace impl /
#         2b46e8d P2-Ea: hash fixup / 49d5e7f P2-Ea: namespace.tla spec /
#         9b8761f proc: wait_pid spins on ct->on_cpu / 81e5f74 sched: try_steal rotate-start /
#         3f526a3 P2-Dc: hash fixup

git status
# Expect: clean (only docs/estimate.md + loc.sh untracked, pre-existing)

tools/build.sh kernel
tools/test.sh
# Expect: 28/28 PASS, ~270 ms boot, full SMP banner with smp:/exception:/ipi: lines
#         + 6 new tests since handoff 010:
#                        proc.rfork_basic_smoke / exits_status / stress_1000
#                        namespace.bind_smoke / cycle_rejected / fork_isolated

tools/test.sh --sanitize=undefined
# Expect: 28/28 PASS, ~320 ms boot

tools/test-fault.sh
# Expect: 4/4 PASS (canary_smash + wxe_violation + bti_fault + kstack_overflow)

tools/verify-kaslr.sh -n 5
# Expect: 5/5 distinct

export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
cd specs

# scheduler.tla
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config scheduler.cfg scheduler.tla 2>&1 | tail -3
# Expect: 10188 distinct states found (depth 17)
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config scheduler_buggy.cfg scheduler.tla 2>&1 | tail -3
# Expect: NoMissedWakeup counterexample at depth 6 (245 distinct states)
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config scheduler_buggy_steal.cfg scheduler.tla 2>&1 | tail -3
# Expect: NoDoubleEnqueue counterexample at depth 4 (38 distinct states)
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config scheduler_buggy_ipi.cfg scheduler.tla 2>&1 | tail -3
# Expect: IPIOrdering counterexample at depth 6 (470 distinct states)

# namespace.tla
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config namespace.cfg namespace.tla 2>&1 | tail -3
# Expect: 625 distinct states found (depth 7)
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config namespace_buggy.cfg namespace.tla 2>&1 | tail -3
# Expect: NoCycle counterexample at depth 4 (95 distinct states)
```

If any fails on a clean checkout, something regressed since the handoff — investigate before proceeding.

---

## Trip-hazards added since handoff 010

### P2-Db (1000-iter stress)

- **Cross-CPU placement assertion is opportunistic at v1.0**: `proc.rfork_stress_1000` doesn't assert distinct-CPU placement because secondaries don't run their own timers and rfork doesn't IPI peers. Children placed in local tree are typically picked by the same CPU. Tightening the assertion requires P2-Dd's per-CPU timer (or rfork-driven IPI_RESCHED).

### P2-Dc (kstack guard pages)

- **`THREAD_KSTACK_TOTAL_ORDER = 3`** (8 pages, 32 KiB) instead of order=2 (16 KiB). Doubles per-thread kstack allocation cost. v1.0 bind tests use ~3 threads at a time so 6 pages overhead total — negligible.
- **`mmu_set_no_access_range` and `mmu_restore_normal_range`** require the range to lie within a single 2 MiB L2 block. thread_create's 32 KiB allocation is 32 KiB ≪ 2 MiB so always fits; future callers with larger ranges need to call multiple times.
- **L2-block→L3-table demote allocates from buddy** (one page per demote). First guard insertion in each 2 MiB block triggers demote; subsequent guards reuse. With `proc.rfork_stress_1000` allocating new kstacks each iteration, buddy can return pages from different 2 MiB blocks; each block's first kstack triggers a demote. Per-iteration overhead amortized but occasional L3 allocations.
- **`addr_is_stack_guard` reads `current_thread()->magic`** and dereferences `kstack_base`. If current_thread is corrupted, the check itself faults — matching the existing pattern in exception.c (the path is already defensive against most corruptions).
- **SMP-safety: thread_create / thread_free single-CPU at v1.0**. Concurrent demote at Phase 5+ requires a global mmu_lock.

### `81e5f74` (try_steal rotate-start)

- **Single global atomic counter for rotation**: `g_try_steal_rotate`. All CPUs share it. Sufficient for forward-looking spread; not strictly fair (CPUs racing on the counter may both observe similar values). Phase 5+ refinement could move to per-CPU counters with shuffle per call.

### `9b8761f` (wait_pid on_cpu spin)

- **`wait_pid` now spins** on `ct->on_cpu` before thread_free. Spin is no-op when on_cpu is already cleared (typical case at v1.0 single-CPU); becomes load-bearing under per-CPU timers. Adds ~25 ms boot overhead at v1.0 from atomic-load + yield × 1000 stress iterations.
- **Mirrors wakeup()'s on_cpu spin** from P2-Cf. Same protocol applied to reap rather than wake.

### P2-Ea (namespace.tla spec)

- **Spec uses fixed-size set semantics for bindings[p][dst]** (no MBEFORE/MAFTER ordering). Sound for cycle-freedom + isolation invariants; mount-union ordering refinement at Phase 5+.
- **Isolation is structural** in the spec data model (per-Proc function values; no shared mutable state). RFNAMEG sharing not modeled at this phase. When RFNAMEG lands, the spec extends with a Pgrp indirection layer where Isolation becomes a state predicate.
- **Walk determinism** (ARCH §9.1) is structurally satisfied by the functional state model — no separate state invariant.

### P2-Eb (namespace impl)

- **PGRP_MAX_BINDS = 8** (not 32). SLUB's KP_ZERO byte-loop on a larger struct (272 bytes) inflates `proc.rfork_stress_1000` boot past the 500 ms budget under QEMU. 80-byte struct keeps boot at ~270 ms. Phase 5+ growable RB-tree refactor.
- **path_id_t = u32** at v1.0. Phase 4 (9P client) replaces with `struct Chan *`.
- **Single-CPU lifecycle at v1.0**. bind / unmount / pgrp_clone are not internally synchronized; concurrent callers on different CPUs would race. Phase 5+ adds a per-Pgrp lock when RFNAMEG sharing comes online.
- **Mount union semantics not modeled**. v1.0's bind is unflagged (an edge is either present or absent). MREPL / MBEFORE / MAFTER / MCREATE are Phase 5+. Cycle-freedom doesn't depend on union semantics.
- **`mount` (9P-server-attaching variant) deferred** to Phase 4. Structurally identical to bind for cycle-freedom + isolation invariants.
- **`pgrp_init` must run before `proc_init`**. proc_init asserts `kpgrp() != NULL`. main.c bootstrap order: slub_init → pgrp_init → proc_init → thread_init.

---

## Naming holds (cumulative)

- **Plan 9 idiom names KEPT**: `sleep`, `wakeup`, `Rendez`. (P2-Bb)
- **Plan 9 process names KEPT**: `rfork`, `exits`, `wait_pid`. RF* flag constants pinned by Plan 9 / 9front idiom. (P2-Da)
- **Plan 9 namespace names KEPT** (P2-Eb): `Pgrp` (process group; confusing — it's the namespace group, not POSIX session group, but Plan 9 uses Pgrp), `bind` (despite POSIX socket bind — kernel doesn't have that symbol), `unmount`. `pgrp_alloc` / `pgrp_clone` / `pgrp_ref` / `pgrp_unref` follow the kernel's `<thing>_<verb>` pattern.
- **Linux/Plan 9 standard preempt names KEPT**: `sched_tick`, `need_resched`, `preempt_check_irq`. (P2-Bc)
- **Standard SMP names KEPT**: `smp_init`, `smp_cpu_count`, `secondary_entry`, `g_cpu_online`, `g_cpu_alive`, `smp_cpu_idx_self`. (P2-Ca/Cd)
- **PSCI names pinned by Arm DEN 0022D** (P2-Ca): PSCI_CPU_ON_64, PSCI_SUCCESS, etc.
- **GIC names pinned by ARM IHI 0069** (P2-Cdc): GICR_*, GICD_*, ICC_SGI1R_EL1, IPI_RESCHED.
- **Standard ARM names KEPT** (P2-Cc): `g_exception_stacks`, `EXCEPTION_STACK_SIZE`.
- **Linux finish_task_switch idiom KEPT**: `sched_finish_task_switch`, `pending_release_lock`, `prev_to_clear_on_cpu`, `on_cpu`. (P2-Ce/Cf)
- **Spec-action names follow Linux/POSIX vocabulary**: `Steal`, `IPI_Send`, `IPI_Deliver`, `BuggyIPI_Deliver`, `NoDoubleEnqueue`, `IPIOrdering`. (P2-Cg)

Held for explicit signoff:
- `_hang` → `_torpor` (marsupial deep-sleep state).
- Audit-prosecutor agent → stays "prosecutor" for Stratum continuity.

This handoff continues to hold all existing names.

---

## Closing notes

This bundle hardened the multi-process model and opened the namespace section:

- **Stress at scale**: `proc.rfork_stress_1000` validates 1000-iteration rfork+exits+wait without leak.
- **Stack overflow detection**: per-thread guard pages mean kernel stack overflow lands cleanly in extinction — not silent corruption.
- **SMP race close**: wait_pid → thread_free no longer races with the destination CPU's resume code (defensive on_cpu spin).
- **Spec foundation**: `specs/namespace.tla` proves cycle-freedom for the bind primitive; second TLC-checked spec in the project.
- **Impl foundation**: `kernel/namespace.c` lands the kernel-internal bind / unmount / pgrp_clone API. The eventual ramfs init flow (boot-time mount of `/`, `/proc`, `/dev`, `/net`) has its substrate.

P2-Dd (cross-CPU work distribution under per-CPU timers) is the only meaningfully-deferred item from this bundle. The on_cpu spin defensively closes one of its two SMP races; the second (preempt_smoke EC=0) needs focused investigation of the SP_EL1 / SPSel interaction during cross-CPU preempt. Either fix it, or sidestep via rfork-driven IPI_RESCHED. Path is documented in "Open follow-ups."

Posture is excellent: 28/28 tests + UBSan + 4/4 fault matrix + KASLR distinct all PASS; both specs TLC-clean; 0 audit findings open. Boot ~270 ms (production), well under the 500 ms VISION §4 budget.

P2-F (handles + VMO + handles.tla) is the next chunk per ROADMAP. Spec-first as usual: ARCH §28 invariants I-2 (capability monotonicity), I-4 (transfer-via-9P only), I-5 (hardware-handle non-transferability), I-6 (rights monotonic on transfer), I-7 (VMO refcount + mapping lifecycle). Then `kernel/handle.c` + `kernel/vmo.c` impl. Then P2-G (ELF + init), P2-H (closing audit).

The thylacine binds.
