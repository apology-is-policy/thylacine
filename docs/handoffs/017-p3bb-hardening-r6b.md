# Handoff 017 — P3-Bb hardening + R6-B audit close

**Date**: 2026-05-06
**Tip**: `13ddf48` (R6-B hash-fixup; substantive R6-B at `a924770`; hardening at `6281e1b`).
**Phase**: Phase 3 OPEN. P3-A + P3-Ba + P3-Bb (incl. W^X-alias hardening + R6-B audit close) landed.

This is a brief delta-handoff covering the P3-Bb hardening commit and the R6-B audit close. **For the full P3-Ba + P3-Bb context, read `docs/handoffs/016-p3ba-p3bb.md` first.** This handoff lists only what landed AFTER 016.

---

## Pickup pointer

**Read in this order:**

1. `CLAUDE.md` (root) — operational framework.
2. **`docs/handoffs/016-p3ba-p3bb.md`** — P3-Ba + P3-Bb main pickup (substantive).
3. **This file** — what landed after 016.
4. Earlier handoffs (015 → 001) in reverse.
5. `docs/VISION.md` + `docs/ARCHITECTURE.md` (especially §6.2 + §6.10) + `docs/ROADMAP.md` (§6).
6. `docs/NOVEL.md` §3.9 Contract D.
7. `docs/phase3-status.md`.
8. `docs/REFERENCE.md` snapshot + `docs/reference/14-process-model.md` → `23-direct-map.md`.
9. `memory/audit_r6b_closed_list.md` — R6-B findings (2 P2 + 4 P3 closed; 4 P3 deferred).

---

## What landed after handoff 016

### P3-Bb-hardening — direct-map W^X alias for kernel image (`6281e1b`)

The earlier R6-B partial signal flagged: kernel `.text` physical pages are mapped R/X via the kernel image VA AND R/W via the new direct map at `0xFFFF_0000_*`. Per-translation W^X (I-12) is satisfied on each mapping individually, but the same physical page IS BOTH writable AND executable across translations. An attacker with an arbitrary-write primitive could rewrite kernel `.text` via the direct-map alias.

**Fix**: demote `l1_directmap[gib_for_kernel]` from a 1-GiB R/W block to an L2 → L3 chain.

- New BSS tables: `l2_directmap_kernel`, `l3_directmap_kernel`.
- `l2_directmap_kernel` populated with default 2-MiB R/W + XN blocks.
- `l2_directmap_kernel[kernel_2mib_idx] = make_table_pte(l3_directmap_kernel)`.
- `l3_directmap_kernel` mirrors `l3_kernel`'s per-section perms with `PTE_PXN | PTE_UXN` forced:
  - `.text` → RO + XN (PTE_KERN_RO).
  - `.rodata` → RO + XN (PTE_KERN_RO).
  - `.data` + `.bss` + tail → RW + XN (PTE_KERN_RW).
  - boot guard → invalid.
- New `PTE_KERN_RO_BLOCK` constant (2 MiB block, R + XN, Normal-WB).

KASLR for executable jumps preserved: no executable mapping at the fixed direct-map VA, so an attacker who knows `.text`'s PA cannot ROP/JOP to `.text` via the direct map without bypassing KASLR for the kernel image VA.

8 KiB BSS overhead (one L2 + one L3 table). No hot-path performance impact.

### R6-B audit close (`a924770` + `13ddf48`)

R6-B re-run (the original was killed mid-output). Posture: 0 P0 + 0 P1 + 2 P2 + 8 P3. **Hardening verdict: alias-level W^X (I-12) sound.**

**Closed (2 P2 + 4 P3)**:

| Finding | Severity | Title | Fix |
|---|---|---|---|
| F120 | P2 | No TLB invalidate before SCTLR.M=1 in `mmu_program_this_cpu` | Added `tlbi vmalle1is + dsb ish + isb` between TTBR writes and SCTLR.M=1. Cold-boot TLB contents UNKNOWN per ARM ARM D5.7; pre-P3-Bb tolerable but P3-Bb's new tables make the firmware-trust assumption load-bearing. |
| F121 | P2 | `mmu_map_mmio` wrote PTEs without TLB flush | Added `tlbi vmalle1is + dsb ish + isb` after PTE writes. Original "previous entries invalid; no flush needed" claim was wrong on impl-defined ARMv8 cores that cache invalid PTEs. |
| F114 | P3 | Firmware-area pages mapped RW in l3_kernel + l3_directmap_kernel | Explicit invalidation loop for pages `[kernel_2mib_pa, _kernel_start)` in both L3 tables. Pages 0..127 (firmware/UEFI residue) now non-present. |
| F117 | P3 | `mmu_map_mmio` overflow check could wrap | Subtraction-form bound (`n_pages > LIMIT - cursor`) cannot wrap. |
| F118 | P3 | `mmu_map_mmio` lacked TCR.IPS=40-bit guard | Added `if (pa >> 40) extinction(...)` after page alignment check. |
| F122 | P3 | `l1_directmap[9..511]` not explicitly invalidated post-loop | Documentation: BSS-init invariant comment. |

**Deferred with rationale (4 P3)**:

- **F113** — `.rela.dyn` / `.dynamic` / dynsym / dynstr / etc. mapped RW. Requires new linker.ld symbol; bundle with v1.x linker revision (alongside KASLR-randomized direct-map base).
- **F115** — `kva_to_pa` debug assert for non-direct-map input. v1.x debug-build infra OR v2.x capability-typed migration (NOVEL §3.9 D) replaces inlines.
- **F116** — explicit slab-bounds check in kfree. Pure self-documentation; current modulo check sufficient by PFN-arithmetic invariant.
- **F119** — KASLR-randomized direct-map base. Already documented as v1.x hardening in `23-direct-map.md`.

---

## Current state

- **Tip**: `13ddf48` (R6-B hash-fixup).
- **Phase**: Phase 3 OPEN. P3-A + P3-Ba + P3-Bb (incl. hardening + R6-B close) landed.
- **Working tree**: clean (only `docs/estimate.md`, `loc.sh` untracked).
- **`tools/test.sh`**: PASS. **56/56 in-kernel tests**; ~261 ms boot (production), ~301 ms (UBSan).
- **`tools/test.sh --sanitize=undefined`**: PASS.
- **`tools/test-fault.sh`**: PASS (4/4).
- **`tools/verify-kaslr.sh -n 5`**: PASS (5/5 distinct).
- **Specs**: 4 written + 11 cfg variants. Unchanged at P3-Bb / R6-B — direct map + hardening + TLB-flush fixes are impl-only.
- **In-kernel tests**: 56.
- **LOC**: ~8900 kernel/asm + ~1700 TLA+ ≈ ~10600 LOC total.
- **Open audit findings**: 0 unfixed P0/P1/P2. R5-H carry-over deferrals (F77/F78/F81/F82/F84/F85 P2 — bundled with P3-G), R6-A P3 (F108/F109/F110), R6-B P3 (F113/F115/F116/F119).

---

## Verify on session pickup

```bash
git log --oneline -8
# Expect:
#   13ddf48 P3-Bb R6-B: hash fixup
#   a924770 P3-Bb R6-B audit close: 0 P0 + 0 P1 + 2 P2 + 8 P3
#   6281e1b P3-Bb-hardening: direct-map W^X alias for kernel image
#   e436f8b P3-Ba + P3-Bb: handoff 016
#   8695dd3 P3-Bb: hash fixup
#   8a888e1 P3-Bb: kernel direct map in TTBR1 (+ ARCH §6.10 / NOVEL §3.9 D)
#   24e565d P3-Ba: hash fixup
#   2095a6c P3-Ba: ASID allocator

git status
# Expect: clean (only docs/estimate.md, loc.sh untracked)

tools/build.sh kernel
tools/test.sh
# Expect: 56/56 PASS, ~261 ms boot.

tools/test.sh --sanitize=undefined
# Expect: 56/56 PASS, ~301 ms boot.

tools/test-fault.sh
# Expect: 4/4 PASS.

tools/verify-kaslr.sh -n 5
# Expect: 5/5 distinct.
```

---

## Trip-hazards added at hardening + R6-B

### P3-Bb-hardening (cumulative #113-115)

113. **Direct-map of kernel image is page-grain via `l3_directmap_kernel`**. The first GiB of physical RAM (PA 1..2 GiB on QEMU virt) is no longer a 1-GiB block in `l1_directmap`; it goes through L2 + L3. Future modifications to the kernel image's 2 MiB block should update `l3_directmap_kernel` alongside `l3_kernel`.
114. **`PTE_KERN_RO_BLOCK` exists** (2 MiB block, R + XN, Normal-WB). Used currently as the constant pattern for the kernel-image hardening; available for other RO + XN block uses.
115. **Boot guard page is invalidated in BOTH `l3_kernel` AND `l3_directmap_kernel`**. Direct-map probes can't sidestep the guard mechanism.

### R6-B audit close (cumulative #116-118)

116. **TLB flushed at `mmu_program_this_cpu` BEFORE SCTLR.M=1**. Removes firmware-trust assumption. Applied to all CPUs (boot + secondaries via PSCI bring-up).
117. **`mmu_map_mmio` flushes TLB after PTE writes**. Cost is one flush per call (rare; boot-time only at v1.0). Comment in mmu.c corrected.
118. **Firmware-area pages [kernel_2mib_pa, _kernel_start) are non-present in `l3_kernel` AND `l3_directmap_kernel`**. Phys_init reserves the PA range (no buddy/SLUB allocations land there); the page-table change makes any access fault loudly rather than silently read/write firmware-residue.

---

## What's NEXT — Phase 3 sub-chunks

Per `docs/phase3-status.md` (revised post-P3-Bb):

1. ✅ **P3-A: F75 close — proc-table lock**.
2. ✅ **P3-Ba: ASID allocator**.
3. ✅ **P3-Bb: kernel direct map in TTBR1** (+ hardening + R6-B close).
4. **P3-Bc: per-Proc page-tables + struct Proc.asid + remove TTBR0-identity dependencies**. Refactor remaining TTBR0-identity callers (DTB cast, kstack pointers + guards, MMIO bases — UART + GIC) to use direct map / vmalloc. Substantial; likely sub-split into Bca/Bcb/Bcc.
5. **P3-Bd: cpu_switch_context TTBR0 swap + remove TTBR0 identity**. The full payoff: TTBR0 free for per-Proc use.
6. **P3-Be: kproc kernel-only TTBR0**.
7. **P3-C: page-fault handler**.
8. **P3-D: VMA tree + VMO mapping**.
9. **P3-E: exec syscall**.
10. **P3-F: minimal /init**.
11. **P3-G: P2-Dd pulled forward** — closes R5-H F77 + F78.
12. **P3-H: Phase 3 closing audit**.

---

## Closing notes

This delta-handoff is brief because the bulk of P3-Bb context lives in handoff 016. The hardening (`6281e1b`) and R6-B close (`a924770` + `13ddf48`) are tight follow-ups that:

- Closed the alias-level W^X concern surfaced by R6-B's partial signal (kernel `.text` physical pages no longer R/W via direct map).
- Closed two P2 TLB-flush correctness concerns (one pre-existing, one P3-Bb-introduced).
- Tightened mmu_map_mmio's input validation + bound check.
- Marked firmware-area pages non-present.

Recommended `/compact` here. Working tree clean; all tests green; audit closed; next chunk (P3-Bc) is fresh-subsystem-class scope (refactor every remaining TTBR0-identity caller across DTB, kstack, MMIO).

The thylacine hardens its own pages.
