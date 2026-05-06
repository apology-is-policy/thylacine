# Handoff 019 — P3-Bd: per-Proc TTBR0 swap milestone

**Date**: 2026-05-06
**Tip**: `fd40047` (P3-Bdb). Substantive: `ceac629` (P3-Bda), `fd40047` (P3-Bdb).
**Phase**: Phase 3 OPEN. **Per-Proc independent address spaces are now real.**

---

## Pickup pointer

**Read in this order:**

1. `CLAUDE.md` (root) — operational framework.
2. **This file** — P3-Bda + P3-Bdb (the surface this handoff covers).
3. `docs/handoffs/018-p3bca-p3bcb.md` — P3-Bca + P3-Bcb predecessors.
4. `docs/handoffs/017-p3bb-hardening-r6b.md` — direct-map hardening + R6-B.
5. `docs/handoffs/016-p3ba-p3bb.md` — direct-map + ASID baseline.
6. Earlier handoffs (015 → 001) in reverse.
7. `docs/VISION.md` + `docs/ARCHITECTURE.md` (especially §6.2, §6.10, §16) + `docs/ROADMAP.md` §6.
8. `docs/phase3-status.md` (full sub-chunk plan + cumulative trip-hazards).
9. `docs/REFERENCE.md` snapshot + `docs/reference/14-process-model.md` → `24-per-proc-pgtable.md` (latest; extended at P3-Bdb with TTBR0 swap section).
10. `memory/audit_r{1..6b}_closed_list.md` — cumulative do-not-report set.

---

## What landed

### P3-Bda: DTB buffer relocation + retire TTBR0 identity (`ceac629`)

Closes the last TTBR0-identity dependency (DTB) and retires the TTBR0 identity map entirely. Boot-CPU TTBR0_EL1 still points at `l0_ttbr0` (now with all-zero L2s); P3-Bdb wires the per-Proc swap.

- **DTB transition**: new `dtb_relocate_to_buffer()` (lib/dtb.c) copies the DTB blob to a buddy-allocated buffer (sized via bytes_to_order on totalsize). g_dtb gains `relocated` / `struct_kva` / `strings_kva` fields; `dtb_struct_base` / `dtb_strings_base` prefer KVA post-relocation. Pre-relocation queries (dtb_init's header parse + dtb_get_memory + the PL011 dtb_get_compat_reg) use TTBR0 identity at the original PA. Post-relocation queries (gic_init's "arm,gic-v3" lookup, etc.) use direct-map KVA into the buffer.

- **struct_pages**: `mm/phys.c::phys_init` no longer casts struct_pages_pa_start to a low-VA pointer; uses `pa_to_kva()` instead. The buddy zone init's 24 MiB struct_page write goes through TTBR1's direct map.

- **SP_EL0 re-anchoring**: start.S step 8.6 (NEW) on primary re-anchors SP_EL0 to high VA of `_boot_stack_top` after mmu_enable. secondary_entry mirrors with high VA of `g_secondary_boot_stacks[idx-1]` slot post-mmu_program_this_cpu.

- **TTBR0 identity retirement**: new `mmu_retire_ttbr0_identity()` zeros every l2_ttbr0 entry across all 4 GiB + broadcasts TLB invalidate. Called from boot_main AFTER smp_init (so secondaries' bring-up can use TTBR0 identity through their re-anchor).

After P3-Bda, NO kernel low-VA access remains.

### P3-Bdb: cpu_switch_context TTBR0 swap (`fd40047`)

Per-Proc independent address spaces become functional. Each Proc's ASID + pgtable_root encodes its user-half mapping; `cpu_switch_context` swaps TTBR0_EL1 atomically with the other register saves/loads.

- **struct Context grows 112 → 120 bytes**: +8 bytes for `u64 ttbr0` (the TTBR0_EL1 value to swap in). struct Thread grows 224 → 232 bytes via the embedded ctx. `_Static_assert`s updated.

- **context.S extended**:
  - Save: `mrs x9, ttbr0_el1; str x9, [x0, #112]`.
  - Load: `ldr x9, [x1, #112]; msr ttbr0_el1, x9; isb`.
  - The ISB serializes the TTBR0 change so subsequent translations see the new value.
  - **No TLB flush at swap** — per-Proc ASIDs (1..255) tag user mappings; cross-Proc TLB entries don't collide. Kernel mappings via TTBR1 are global (nG=0).

- **ctx.ttbr0 wiring**:
  - `thread_create_internal`: `ttbr0 = (proc->pgtable_root != 0) ? ((asid << 48) | pgtable_root) : mmu_kernel_ttbr0_pa()`.
  - `thread_init` (kthread): pre-populate with `mmu_kernel_ttbr0_pa()` for defense-in-depth (FIRST switch INTO kthread bypasses the save path).
  - `thread_init_per_cpu_idle`: same pre-populate.

- **mmu.c additions**: `g_l0_ttbr0_pa` captured in build_page_tables (PA of l0_ttbr0; PIC adrp+add at PC=load_PA gives load PA). `mmu_kernel_ttbr0_pa()` accessor.

- **New test `proc.ttbr0_swap_smoke`**: rfork two children. Each reads its live TTBR0_EL1 + records its Proc's pgtable_root + ASID. Parent verifies post-reap that TTBR0_EL1 == (asid << 48) | pgtable_root for each child. Cross-checks distinct ASIDs and pgtable_roots.

After P3-Bdb, the per-Proc address-space milestone is met.

---

## Current state

- **Tip**: `fd40047` (P3-Bdb).
- **Phase**: Phase 3 OPEN. P3-A + P3-Ba + P3-Bb (+ hardening + R6-B) + P3-Bca + P3-Bcb + **P3-Bda + P3-Bdb** landed.
- **Working tree**: clean (only `docs/estimate.md`, `loc.sh` untracked).
- **`tools/test.sh`**: PASS. **59/59 in-kernel tests** (1 new at P3-Bdb); ~322 ms boot (production), ~344 ms (UBSan).
- **`tools/test.sh --sanitize=undefined`**: PASS.
- **`tools/test-fault.sh`**: PASS (4/4) — kstack_overflow still works (direct-map guard pages still functional under per-Proc TTBR0).
- **`tools/verify-kaslr.sh -n 5`**: PASS (5/5 distinct).
- **Specs**: 4 written + 11 cfg variants. Unchanged at P3-Bda / P3-Bdb — both impl-only.
- **In-kernel tests**: 59 (up from 58 at end of P3-Bcb).
- **LOC**: ~11200 kernel/asm + ~1700 TLA+ ≈ ~12900 LOC total.
- **Open audit findings**: 0 unfixed P0/P1/P2.

---

## Verify on session pickup

```bash
git log --oneline -10
# Expect:
#   fd40047 P3-Bdb: cpu_switch_context TTBR0 swap
#   ceac629 P3-Bda: DTB buffer relocation + retire TTBR0 identity
#   d9975a0 P3-Bca + P3-Bcb: handoff 018 + reference doc 24 + REFERENCE.md snapshot
#   5d9424a P3-Bcb: hash fixup
#   d256804 P3-Bcb: per-Proc page-table allocate/free + struct Proc.asid
#   d71f08a P3-Bca: hash fixup
#   26b01a7 P3-Bca: refactor TTBR0-identity callers to direct map + vmalloc
#   47c3e78 P3-Bb hardening + R6-B: handoff 017
#   13ddf48 P3-Bb R6-B: hash fixup
#   a924770 P3-Bb R6-B audit close: 0 P0 + 0 P1 + 2 P2 + 8 P3

git status
# Expect: clean (only docs/estimate.md, loc.sh untracked)

tools/build.sh kernel
tools/test.sh
# Expect: 59/59 PASS, ~322 ms boot. proc.ttbr0_swap_smoke is the new test.

tools/test.sh --sanitize=undefined
# Expect: 59/59 PASS, ~344 ms boot.

tools/test-fault.sh
# Expect: 4/4 PASS.

tools/verify-kaslr.sh -n 5
# Expect: 5/5 distinct.
```

---

## Trip-hazards added at P3-Bda + P3-Bdb (cumulative #117-124)

### P3-Bda

117. **DTB blob is copied to a buddy-allocated buffer post-phys_init** — pre-relocation queries use TTBR0 identity at the original PA; post-relocation queries use direct-map KVA into the buffer. The original PA's data becomes unreachable after `mmu_retire_ttbr0_identity`.

118. **TTBR0 identity is retired AFTER smp_init** — secondaries' `secondary_entry` runs with SP_EL0 = load PA until the post-mmu_program_this_cpu re-anchor (step 8.6 equiv); retiring before would fault their stack.

119. **start.S step 8.6 (NEW): primary re-anchors SP_EL0 to high VA** — mirrors step 8.5 (SP_EL1). secondary_entry has the equivalent post-mmu_program_this_cpu.

120. **mm/phys.c::phys_init uses `pa_to_kva(struct_pages_pa_start)`** — was load-PA cast pre-P3-Bda. The struct_pages array is now backed by direct-map mappings.

### P3-Bdb

121. **struct Context grows to 120 bytes; struct Thread grows to 232 bytes** — `u64 ttbr0` added. `_Static_assert` updated.

122. **No TLB flush at cpu_switch_context's TTBR0 swap** — per-Proc ASIDs tag user mappings; cross-Proc TLB entries don't collide. ASID reuse is safe via `asid_free`'s pre-return TLB flush (P3-Ba; trip-hazard #107).

123. **kthread + per-CPU idle have ctx.ttbr0 = mmu_kernel_ttbr0_pa()** — defense-in-depth: pre-populating protects the FIRST-switch-INTO path that bypasses save.

124. **TTBR0 swap relies on `mmu_kernel_ttbr0_pa()` returning non-zero** — g_l0_ttbr0_pa is captured in build_page_tables. Pre-build_page_tables, mmu_kernel_ttbr0_pa() returns 0; no thread_create / thread_init runs that early at v1.0. Phase 5+ early-init refactors must preserve this invariant.

---

## What's NEXT — Phase 3 sub-chunks

Per `docs/phase3-status.md`:

1. ✅ **P3-A: F75 close — proc-table lock**.
2. ✅ **P3-Ba: ASID allocator**.
3. ✅ **P3-Bb: kernel direct map in TTBR1** (+ hardening + R6-B).
4. ✅ **P3-Bca: refactor TTBR0-identity callers**.
5. ✅ **P3-Bcb: per-Proc page-table lifecycle**.
6. ✅ **P3-Bda: DTB buffer + retire TTBR0 identity**.
7. ✅ **P3-Bdb: cpu_switch_context TTBR0 swap**.
8. **P3-Be (implicit/skipped)**: kproc kernel-only TTBR0 — already satisfied by `mmu_kernel_ttbr0_pa()` defaulting in thread_init / thread_init_per_cpu_idle. A separate explicit P3-Be chunk is unnecessary.
9. **P3-C: page-fault handler**. NEXT. New `arch/arm64/fault.c` (or extend exception.c). Decode FAR_EL1 + ESR_EL1; classify (read/write/exec/translation/permission); dispatch to VMA tree. Demand-page allocation: walk VMAs, allocate physical page, install PTE in the per-Proc tree.
10. **P3-D: VMA tree + VMO mapping**. New mm/vm.c (or similar) with VMA tree per Proc + the `vmo_map` syscall surface. Triggers `proc_pgtable_destroy` extension to walk sub-tables (trip-hazard #116).
11. **P3-E: exec syscall**.
12. **P3-F: minimal /init**.
13. **P3-G: P2-Dd pulled forward** — closes R5-H F77 + F78.
14. **P3-H: Phase 3 closing audit**.

Phase 3 specs to land: `specs/exec.tla` or extension to `vmo.tla`; revisit `scheduler.tla` at P3-G.

---

## Open follow-ups (cumulative)

- **F77 + F78 (P2)**: bundled with **P3-G**.
- **F81 + F82 + F84 (P2)**: try_steal contention etc. Phase 3+ scheduler optimization or post-v1.0.
- **F85 (P2)**: try_steal priority inversion. Policy-choice; benchmark-driven post-v1.0.
- **U-30 (P2-A R4 F48)**: kstack zeroing on free.
- **R5-F deferred items**: F56/F57/F58/F59 (P3, future-chunk-named).
- **R5-G deferred items**: F65/F66/F72/F73/F74 (P3, mostly Phase 3+ exec-side).
- **R5-H deferred items**: F86-F104 (P3, mostly P3-G or post-v1.0).
- **R6-A deferred items**: F108-F110 (P3).
- **R6-B deferred items**: F113/F115/F116/F119 (P3).
- **proc_pgtable_destroy sub-table walk (#116 in trip-hazards)**: P3-D-blocking.

---

## Closing notes

P3-Bd (Bda + Bdb) is the **per-Proc independent address space milestone**. Each rfork-created Proc has its own L0 page table; cpu_switch_context swaps TTBR0_EL1 atomically with the register save/load. The TTBR0 identity map is gone (retired); kernel low-VA access faults; all kernel paths use TTBR1 (kernel image high VA, direct map, vmalloc).

The remaining Phase 3 work is the page-fault handler (P3-C) + VMA tree + VMO mapping (P3-D) + exec (P3-E) + /init (P3-F). With independent address spaces in place, these unlock userspace.

P3-C is fresh-subsystem-class scope (new `arch/arm64/fault.c`, decode FAR/ESR, dispatch). Warrants a new context if going through `/compact`.

Posture: 59/59 × (default + UBSan) green. ~322 ms boot. 0 unfixed P0/P1/P2. 4 specs all TLC-clean.

The thylacine swaps page tables on every context switch.
