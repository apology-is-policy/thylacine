# Handoff 018 — P3-Bca + P3-Bcb

**Date**: 2026-05-06
**Tip**: `5d9424a` (P3-Bcb hash fixup). Substantive: `26b01a7` (P3-Bca), `d256804` (P3-Bcb).
**Phase**: Phase 3 OPEN. P3-A + P3-Ba + P3-Bb (+ hardening + R6-B) + **P3-Bca + P3-Bcb** landed.

---

## Pickup pointer

**Read in this order:**

1. `CLAUDE.md` (root) — operational framework.
2. **This file** — P3-Bca + P3-Bcb (the surface this handoff covers).
3. `docs/handoffs/017-p3bb-hardening-r6b.md` — direct-map hardening + R6-B.
4. `docs/handoffs/016-p3ba-p3bb.md` — direct-map + ASID baseline.
5. `docs/handoffs/015-p3a-f75.md` — proc-table lock baseline.
6. Earlier handoffs (014 → 001) in reverse.
7. `docs/VISION.md` + `docs/ARCHITECTURE.md` (especially §6.2 + §6.10 + §16) + `docs/ROADMAP.md` (§6).
8. `docs/phase3-status.md` (full sub-chunk plan + cumulative trip-hazards).
9. `docs/REFERENCE.md` snapshot + `docs/reference/14-process-model.md` → `24-per-proc-pgtable.md` (latest).
10. `memory/audit_r{1,2,3,4,5f,5g,5h,6a,6b}_closed_list.md` (cumulative do-not-report set).

---

## What landed

### P3-Bca: refactor TTBR0-identity callers to direct map / vmalloc (`26b01a7`)

UART, GIC, and per-thread kstack retired from TTBR0 identity:

- **UART**: new `uart_remap_to_vmalloc(pa, size)` calls `mmu_map_mmio` to install a Device-nGnRnE mapping at the kernel vmalloc range. boot_main calls this immediately after `dtb_get_compat_reg("arm,pl011")` succeeds. The early fallback (`pl011_base = 0x09000000UL` from .data init) covers the brief pre-remap window via TTBR0 identity; once the remap returns, `pl011_base` is the vmalloc KVA.

- **GIC**: replace the two `mmu_map_device` calls with `mmu_map_mmio`. `g_dist_base` and `g_redist_base` are now KVAs in vmalloc range (0xFFFF_8000_*). Cap the redist mapping at `dtb_cpu_count * GICR_FRAME_STRIDE` — QEMU virt's DTB reports the full reservation (≈ 64 MiB at 256 max-CPUs) but only the configured CPUs' frames are populated; mapping the full region would exhaust l3_vmalloc. New `gic_dist_pa()` / `gic_redist_pa()` accessors so the banner shows PA + KVA.

- **Kstack**: `kstack_base` is now a direct-map KVA (`pa_to_kva(stack_pa)`); `t->ctx.sp` is the corresponding KVA + size. `mmu_set_no_access_range` / `mmu_restore_normal_range` (and the singular variants) operate on the kernel direct map: walk `l1_directmap[gib]`, demote 1 GiB block → L2 if needed; walk to L2[idx], demote 2 MiB block → L3 if needed; invalidate / restore the L3 entry. Both demotes are idempotent and lazy. New helpers `directmap_walk_to_l2` / `directmap_walk_to_l3` + `make_table_pte_pa`.

- **`addr_is_kernel_image`** (exception.c) recognizes the direct-map alias of the kernel-image PA range, so a hypothetical W^X violation through `pa_to_kva(.text)` classifies correctly.

- **Removed dead code**: `mmu_map_device` deleted entirely (no callers post-refactor). `l2_ttbr0_entry_for` and `demote_l2_block_to_l3` deleted (only used by the old TTBR0-identity guard path).

**DEFERRED to P3-Bd**: DTB stays on TTBR0 identity. The direct-map path (`pa_to_kva(g_dtb.base)`) hangs at the first dereference at boot_main entry — for reasons not diagnosed in this session. Empirical bisection: with original dtb.c (PA-as-VA), boot proceeds normally; with `pa_to_kva(base)`, the first read of `hdr->magic` hangs silently. Tests/diagnostics confirm: direct-map writes/reads work post-phys_init (test_directmap_alloc_through_directmap PASSES) but pre-phys_init reads at fixed PAs (kernel image PA, GiB 2 PA, DTB PA) hang silently. P3-Bd will fix this — likely by copying the DTB blob to a kpage_alloc'd buffer post-phys_init OR via a dedicated TTBR1 reservation with explicit cache management. Documented as trip-hazard #108.

### P3-Bcb: per-Proc page-table allocate/free + struct Proc.asid (`d256804`)

- **struct Proc grows 96 → 112 bytes**: `paddr_t pgtable_root` + `u16 asid` + 6-byte alignment padding. `_Static_assert(sizeof(struct Proc) == 112)` pinned.

- **New API in `<arch/arm64/mmu.h>`**:
  - `paddr_t proc_pgtable_create(void)` — allocates one 4 KiB page (KP_ZERO; all 512 L0 entries invalid). Returns PA. Returns 0 on OOM.
  - `void proc_pgtable_destroy(paddr_t root)` — frees the L0 page. Idempotent on root == 0.

- **proc_alloc**: order is `handle_table_alloc → proc_pgtable_create → asid_alloc`. On pgtable_create OOM: state = ZOMBIE + proc_free (idempotent cleanup). On asid_alloc exhaustion: extincts (255 ASIDs at v1.0 unreachable under test scales).

- **proc_free**: releases pgtable_root via `proc_pgtable_destroy` (gated on `!= 0`); releases asid via `asid_free` (gated on `!= 0`; ASID 0 is kernel-reserved).

- **kproc**: pgtable_root = 0 + asid = 0 forever (KP_ZERO; never installed by `proc_init`). kproc never enters EL0; doesn't need a user-half page table or non-kernel ASID. proc_free's `p == g_kproc` gate prevents reaching cleanup paths anyway.

- **Scope is intentionally narrow at P3-Bcb**: lifecycle plumbing only. The allocated page tables sit unused until P3-Bd wires the TTBR0 swap. P3-D will refactor `proc_pgtable_destroy` to walk + free L1/L2/L3 sub-tables.

- **2 new tests** in `kernel/test/test_proc_pgtable.c`:
  - `proc.pgtable_alloc_smoke`: alloc returns non-zero + page-aligned pgtable_root + valid asid; free returns asid via inflight tracking.
  - `proc.pgtable_lifecycle_stress`: 64 alloc/free cycles, no leak in proc counters or asid_inflight.

- **New reference doc**: `docs/reference/24-per-proc-pgtable.md`.

---

## Current state

- **Tip**: `5d9424a` (P3-Bcb hash fixup).
- **Phase**: Phase 3 OPEN. P3-A + P3-Ba + P3-Bb (incl. hardening + R6-B) + **P3-Bca + P3-Bcb** landed.
- **Working tree**: clean (only `docs/estimate.md`, `loc.sh` untracked).
- **`tools/test.sh`**: PASS. **58/58 in-kernel tests** (2 new at P3-Bcb); ~287 ms boot (production), ~301 ms (UBSan).
- **`tools/test.sh --sanitize=undefined`**: PASS.
- **`tools/test-fault.sh`**: PASS (4/4) — kstack_overflow exercises the direct-map kstack guard (proves P3-Bca's mmu_set_no_access_range refactor is sound).
- **`tools/verify-kaslr.sh -n 5`**: PASS (5/5 distinct).
- **Specs**: 4 written + 11 cfg variants. Unchanged at P3-Bca / P3-Bcb — both impl-only.
- **In-kernel tests**: 58 (up from 56 at end of P3-Bb).
- **LOC**: ~10800 kernel/asm + ~1700 TLA+ ≈ ~12500 LOC total.
- **Open audit findings**: 0 unfixed P0/P1/P2. Cumulative deferrals: R5-H F77/F78/F81/F82/F84/F85 (P2; bundled with P3-G); R6-A F108/F109/F110 (P3); R6-B F113/F115/F116/F119 (P3); R5-F/G P3 deferrals.

---

## Verify on session pickup

```bash
git log --oneline -10
# Expect:
#   5d9424a P3-Bcb: hash fixup
#   d256804 P3-Bcb: per-Proc page-table allocate/free + struct Proc.asid
#   26b01a7 P3-Bca: refactor TTBR0-identity callers to direct map + vmalloc
#   47c3e78 P3-Bb hardening + R6-B: handoff 017
#   13ddf48 P3-Bb R6-B: hash fixup
#   a924770 P3-Bb R6-B audit close: 0 P0 + 0 P1 + 2 P2 + 8 P3
#   6281e1b P3-Bb-hardening: direct-map W^X alias for kernel image
#   e436f8b P3-Ba + P3-Bb: handoff 016
#   8695dd3 P3-Bb: hash fixup
#   8a888e1 P3-Bb: kernel direct map in TTBR1 (+ ARCH §6.10 / NOVEL §3.9 D)

git status
# Expect: clean (only docs/estimate.md, loc.sh untracked)

tools/build.sh kernel
tools/test.sh
# Expect: 58/58 PASS, ~287 ms boot.

tools/test.sh --sanitize=undefined
# Expect: 58/58 PASS, ~301 ms boot.

tools/test-fault.sh
# Expect: 4/4 PASS.

tools/verify-kaslr.sh -n 5
# Expect: 5/5 distinct.
```

---

## Trip-hazards added at P3-Bca + P3-Bcb (cumulative #108-116)

### P3-Bca

108. **DTB still on TTBR0 identity at P3-Bca** — `lib/dtb.c::dtb_init` casts `(uintptr_t)base` instead of `pa_to_kva(base)`. Direct-map dereference at boot_main entry hangs (verified empirically; cause undiagnosed). P3-Bd MUST refactor before retiring TTBR0 identity entirely. Cleanest fix: post-phys_init, copy DTB blob to a kpage_alloc'd buffer, then all DTB walks go through that buffer's KVA. Alternative: dedicated TTBR1 reservation with explicit cache management.

109. **`mmu_map_mmio` for GIC redist caps the size to `dtb_cpu_count * GICR_FRAME_STRIDE`** (P3-Bca). QEMU virt's DTB reports the FULL redist region (≈ 64 MiB at 256 max-CPUs × 0x40000), but only the configured CPUs' frames are populated. Mapping the full reported region would burn through l3_vmalloc unnecessarily.

110. **kstack VAs are now direct-map KVAs (`pa_to_kva(stack_pa)`)** (P3-Bca). Pre-P3-Bca they were PA-as-VA via TTBR0 identity. `mmu_set_no_access_range` operates on the direct map (demotes `l1_directmap[gib]` to L2 → L3 lazily). Stack overflow detection in `addr_is_stack_guard` works because FAR_EL1 == direct-map KVA of guard page == `t->kstack_base + offset`.

111. **`mmu_map_device` deleted** (P3-Bca). No callers post-refactor. The TTBR0-identity Device override for PL011 in `build_page_tables` is still intact — it provides the early-boot fallback (pre-`uart_remap_to_vmalloc`). P3-Bd retires TTBR0 identity entirely; this fallback path goes with it.

112. **`l1_directmap[gib]` may be demoted lazily by `mmu_set_no_access_range`** (P3-Bca). The first kstack_create per GiB demotes that GiB's 1 GiB block to L2; the first kstack_create per 2 MiB demotes that block to L3. v1.0 single-thread + boot-CPU-only thread_create makes this race-free. Phase 5+ multi-thread requires a global mmu_lock around demotes.

### P3-Bcb

113. **struct Proc grows to 112 bytes** (P3-Bcb). `_Static_assert(sizeof(struct Proc) == 112)` pinned. Adding a field grows the SLUB cache; update the assert deliberately.

114. **kproc has `pgtable_root = 0` and `asid = 0`** (P3-Bcb). kproc is allocated pre-phys_init (buddy not yet up) so it cannot run `proc_pgtable_create`; kproc also never enters EL0 so it never needs a user-half page table or a non-kernel ASID. ASID 0 is the kernel-reserved ASID per P3-Ba. **`proc_free` MUST gate `asid_free(p->asid)` on `p->asid != 0`** to avoid passing the kernel ASID to the user-ASID free path (asid_free's `if (asid < ASID_USER_FIRST) extinction` would fire). Same for `proc_pgtable_destroy(p->pgtable_root)` gated on `!= 0`.

115. **proc_alloc rollback gates pgtable + asid via proc_free's idempotent cleanup** (P3-Bcb). `proc_free` is the single drain — it idempotently releases pgrp / handles / pgtable_root / asid based on which were installed (KP_ZERO leaves them zero on a Proc that didn't reach those allocs). Order in proc_alloc: handle_table_alloc → pgtable_create → asid_alloc. handle_table_alloc OOM → proc_free. pgtable_create OOM → proc_free (releases handles). asid_alloc extincts (no rollback path).

116. **`proc_pgtable_destroy` at P3-Bcb only frees L0** (no sub-tables exist until P3-D adds VMA + page-fault). P3-D's destroy must walk L0 → L1 → L2 → L3, freeing every installed sub-table before the L0. Documented as P3-D-blocking.

---

## What's NEXT — Phase 3 sub-chunks

Per `docs/phase3-status.md`:

1. ✅ **P3-A: F75 close — proc-table lock**.
2. ✅ **P3-Ba: ASID allocator**.
3. ✅ **P3-Bb: kernel direct map in TTBR1** (+ hardening + R6-B close).
4. ✅ **P3-Bca: refactor TTBR0-identity callers** (UART + GIC + kstack).
5. ✅ **P3-Bcb: per-Proc page-table allocate/free + struct Proc.asid**.
6. **P3-Bd: cpu_switch_context TTBR0 swap + retire TTBR0 identity**. NEXT. The actual independent-address-space milestone. Three components:
   - asm change: extend `cpu_switch_context` to write `TTBR0_EL1 = (asid << 48) | pgtable_root` and issue context-synchronizing barriers.
   - DTB transition (#108): copy DTB to a kpage_alloc'd buffer; all dtb.c reads go through the buffer's direct-map KVA.
   - Kill TTBR0 identity: remove `l0_ttbr0` / `l1_ttbr0` / `l2_ttbr0` from build_page_tables; remove the boot-time PL011 override; remove the early UART fallback.
7. **P3-Be: kproc kernel-only TTBR0**. kproc retains a degenerate TTBR0 (or TTBR0=0 with kernel-only access) for safety during cpu_switch_context.
8. **P3-C: page-fault handler**.
9. **P3-D: VMA tree + VMO mapping**. Triggers `proc_pgtable_destroy` extension to walk sub-tables.
10. **P3-E: exec syscall**.
11. **P3-F: minimal /init**.
12. **P3-G: P2-Dd pulled forward** — closes R5-H F77 + F78.
13. **P3-H: Phase 3 closing audit**.

Phase 3 specs to land: `specs/exec.tla` (or extend `vmo.tla`); revisit `scheduler.tla` at P3-G.

---

## Open follow-ups (cumulative)

- **F77 + F78 (P2)**: bundled with **P3-G**.
- **F81 + F82 + F84 (P2)**: try_steal contention + sched_remove_if_runnable latency + in_run_tree single-CPU check. Phase 3+ scheduler optimization or post-v1.0.
- **F85 (P2)**: try_steal priority inversion. Policy-choice; benchmark-driven post-v1.0.
- **U-30 (P2-A R4 F48)**: kstack zeroing on free.
- **R5-F deferred items**: F56/F57/F58/F59 (P3, future-chunk-named).
- **R5-G deferred items**: F65/F66/F72/F73/F74 (P3, mostly Phase 3+ exec-side).
- **R5-H deferred items**: F86-F104 (P3, mostly P3-G or post-v1.0).
- **R6-A deferred items**: F108-F110 (P3, Phase 3+/5+).
- **R6-B deferred items**: F113/F115/F116/F119 (P3, v1.x or v2.x).
- **DTB direct-map deferral (#108 in trip-hazards)**: P3-Bd-blocking.
- **proc_pgtable_destroy sub-table walk (#116 in trip-hazards)**: P3-D-blocking.

---

## Closing notes

P3-Bca and P3-Bcb together advance the per-Proc TTBR0 plan from "ASID allocator + direct map exists" to "every refactorable TTBR0-identity caller now uses direct map + vmalloc, AND every Proc owns its own page table + ASID." The remaining piece for the actual independent-address-space milestone is P3-Bd — wire the TTBR0 swap on context switch, fix the DTB direct-map issue, and retire TTBR0 identity entirely. P3-Bd is fresh-subsystem-class scope; warrants fresh context.

The DTB direct-map issue (trip-hazard #108) is the single open item from this session. It's not blocking P3-Bcb but IS blocking P3-Bd. The fix is straightforward (copy DTB to kpage_alloc'd buffer post-phys_init); just needs to be done carefully because dtb_init runs BEFORE phys_init in the bootstrap order.

Posture: 58/58 × (default + UBSan) green. ~287 ms boot. 0 unfixed P0/P1/P2. 4 specs all TLC-clean.

The thylacine maps each Proc its own page.
