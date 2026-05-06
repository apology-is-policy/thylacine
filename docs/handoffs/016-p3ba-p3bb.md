# Handoff 016 — P3-Ba (ASID allocator) + P3-Bb (kernel direct map)

**Date**: 2026-05-06
**Tip**: `8695dd3` (P3-Bb hash-fixup; substantive at `8a888e1`).
**Phase**: Phase 3 OPEN. P3-A + P3-Ba + P3-Bb landed. R6-B (P3-Bb focused audit) running in background as of this handoff write-time.

---

## Pickup pointer

**Read in this order:**

1. `CLAUDE.md` (root) — operational framework.
2. **This file** — canonical pickup at P3-Bb close.
3. `docs/handoffs/015-p3a-f75.md` — P3-A predecessor.
4. `docs/handoffs/014-p2h-r5h.md` — Phase 2 close predecessor.
5. Earlier handoffs (013 → 001) in reverse.
6. `docs/VISION.md` + `docs/ARCHITECTURE.md` (especially **§6.2 + §6.10** — the latter NEW at P3-Bb describing the v2.x capability-addressing direction) + `docs/ROADMAP.md` (§6 for Phase 3).
7. `docs/NOVEL.md` §3.9 Contract D (NEW at P3-Bb; capability-based kernel addressing as v2.x architectural goal).
8. `docs/phase3-status.md` — sub-chunk plan with all landed rows.
9. `docs/REFERENCE.md` snapshot + `docs/reference/14-process-model.md` → `23-direct-map.md` (latest at P3-Bb).
10. `specs/scheduler.tla` etc. + `specs/SPEC-TO-CODE.md`.
11. `memory/audit_r5h_closed_list.md` + `memory/audit_r6a_closed_list.md` + (R6-B closed list when audit lands).

---

## What landed since handoff 015

### P3-Ba — ASID allocator (`24e565d`)

Forward-looking ASID allocator. 8-bit ASIDs (256 total; 0 reserved for kernel TTBR1; 1..255 for user Procs). Free-list LIFO + monotonic counter fallback. `asid_free` issues `tlbi aside1is, <asid>` (inner-shareable broadcast) BEFORE returning the slot — next allocator pop sees flushed TLB. Generation rollover deferred to Phase 5+; v1.0 extincts on hard exhaustion. Boot-time `asid_init()` in main.c bootstrap (slub_init → pgrp_init → handle_init → vmo_init → **asid_init** → proc_init → thread_init → sched_init).

3 new tests: `asid.alloc_unique`, `asid.free_reuses`, `asid.inflight_count`.
New reference doc: `docs/reference/22-asid.md`.

No Proc-side caller at P3-Ba (forward-looking). P3-Bc wires `proc_alloc` / `proc_free` callsites; P3-Bd loads ASID into TTBR0_EL1 at context switch.

### P3-Bb — kernel direct map (`8a888e1` + `8695dd3`)

**Why P3-Bb existed**: the original Phase 3 plan had P3-Bb as "per-Proc page-table allocate/free." During design, a foundational issue surfaced: `mm/slub.c:151` puns `void *obj = (void *)(uintptr_t)(base_pa + i * c->actual_size)` — relying on TTBR0's identity-map of low PAs to make PA = VA. Per-Proc TTBR0 swap would break SLUB and every kernel allocator user.

P3-Bb is the prerequisite refactor: build the kernel direct map per ARCH §6.2; migrate SLUB + kpage_alloc to use direct-map KVAs. After P3-Bb, kernel data access is independent of TTBR0 contents.

**ARCH §6.10 + NOVEL §3.9 Contract D (v2.x architectural goal)**: before implementing, recorded the design discussion. The direct map IS standard (Linux/FreeBSD/XNU pattern) but it's not the SOTA — it exposes every byte of RAM to every line of kernel code. The principled alternative (capability-based kernel addressing, seL4 / CHERI Morello) is multi-year and requires Rust or hardware capabilities. v1.0 takes the direct-map compromise; v2.x is the principled path. The contract makes v1.0's choice EXPLICITLY the pragmatic compromise rather than implicitly the only option.

**What landed**:

- `kernel/include/thylacine/page.h`: new `pa_to_kva(pa) = pa | KERNEL_DIRECT_MAP_BASE`; `kva_to_pa(kva) = kva & ~KERNEL_DIRECT_MAP_BASE`. KERNEL_DIRECT_MAP_BASE = `0xFFFF_0000_0000_0000`.
- `arch/arm64/mmu.h`: new `mmu_map_mmio(pa, size)` API; new `PTE_DEVICE_RW` (page-grain Device-nGnRnE).
- `arch/arm64/mmu.c`: new BSS tables `l1_directmap`, `l1_vmalloc`, `l2_vmalloc`, `l3_vmalloc`. `build_page_tables` extended:
  - `l0_ttbr1[0] → l1_directmap; l1_directmap[1..8] = R/W+XN 1-GiB blocks` covering PA 1..9 GiB.
  - `l0_ttbr1[256] → l1_vmalloc → l2_vmalloc → l3_vmalloc` (2 MiB page-grain).
- `mmu_map_mmio` implementation: bumps `g_vmalloc_next_idx`; populates l3_vmalloc with `PTE_DEVICE_RW`; dsb_ishst + isb publishes; no TLB flush needed (previous entries invalid).
- `mm/slub.c`: `slab_init_freelist`, `kmem_cache_free`, `kmalloc` large path, `kfree` all refactored to use direct-map KVAs via `pa_to_kva` / `kva_to_pa`.
- `mm/phys.c`: `alloc_pages` KP_ZERO loop, `kpage_alloc`, `kpage_free` refactored.
- W^X invariant I-12 holds at the alias level: kernel image at TTBR1 KASLR offset is R/X; direct map at `0xFFFF_0000_*` is R/W + XN unconditionally; never both R/W and X via the same translation.

**What's deliberately deferred** (P3-Bc/Bd will refactor):

- DTB pointer cast (`lib/dtb.c`).
- struct_pages array cast (`mm/phys.c`).
- kstack pointers + guard mechanism (`kernel/thread.c` + `arch/arm64/mmu.c::mmu_set_no_access_range`).
- UART base (`arch/arm64/uart.c`).
- GIC dist + redist bases (`arch/arm64/gic.c`).
- Removal of TTBR0 identity entirely.

These ALL work at P3-Bb via TTBR0 identity (kept intact).

**3 new tests**: `directmap.kva_round_trip`, `directmap.alloc_through_directmap`, `directmap.vmalloc_mmio_smoke`.

**New reference doc**: `docs/reference/23-direct-map.md`.

---

## Current state at handoff

- **Tip**: `8695dd3` (P3-Bb hash-fixup). Substantive at `8a888e1`.
- **Phase**: Phase 3 OPEN. P3-A + P3-Ba + P3-Bb landed. R6-B audit running in background.
- **Working tree**: clean (only `docs/estimate.md`, `loc.sh` untracked).
- **`tools/test.sh`**: PASS. **56/56 in-kernel tests** (3 new at P3-Ba + 3 new at P3-Bb); ~260 ms boot (production), ~301 ms (UBSan).
- **`tools/test.sh --sanitize=undefined`**: PASS.
- **`tools/test-fault.sh`**: PASS (4/4).
- **`tools/verify-kaslr.sh -n 5`**: PASS (5/5 distinct).
- **Specs**: 4 written + 11 cfg variants. Unchanged at P3-Ba/Bb — both impl-only.
- **In-kernel tests**: 56.
- **LOC**: ~8800 kernel/asm + ~1700 TLA+ ≈ ~10500 LOC total.
- **Open audit findings**: 0 unfixed P0/P1/P2 pre-R6-B. R6-B in flight; will triage on completion.

---

## Phase 3 sub-chunk plan (revised at P3-Bb)

The original plan had P3-B as 4 sub-chunks (Ba ASID, Bb per-Proc page-tables, Bc context-switch, Bd kproc-only). Revised after P3-Bb's discovery:

1. ✅ **P3-A: F75 close — proc-table lock** (`9d5c271` + `4bfc1bd` R6-A).
2. ✅ **P3-Ba: ASID allocator** (`24e565d`).
3. ✅ **P3-Bb: kernel direct map in TTBR1** (`8695dd3`).
4. **P3-Bc: per-Proc page-tables + struct Proc.asid + remove TTBR0-identity dependencies**. Each Proc gets a page-table tree at proc_alloc + asid via asid_alloc; freed at proc_free. Refactor remaining TTBR0-identity callers (DTB cast, kstack pointers + guards, MMIO bases) to use direct map / vmalloc. This is a substantial chunk — likely sub-split into Bca (Proc-side struct + alloc/free), Bcb (refactor TTBR0-identity callers), Bcc (test + audit).
5. **P3-Bd: cpu_switch_context TTBR0 swap + remove TTBR0 identity**. Extends asm context-switch to write `TTBR0_EL1 = (asid << 48) | pgtable_root_pa`; removes TTBR0 identity-map entirely.
6. **P3-Be: kproc kernel-only TTBR0**. kproc placeholder; final cleanup.
7. **P3-C: page-fault handler**. Decode FAR_EL1 + ESR_EL1; demand-page; COW.
8. **P3-D: VMA tree + VMO mapping**. `vmo_map` syscall surface integrating Phase 2 vmo.c.
9. **P3-E: exec syscall**. Completes P2-Ga's parse-validate with map-and-dispatch.
10. **P3-F: minimal /init**. First userspace process.
11. **P3-G: P2-Dd pulled forward** — ready/wakeup IPI_RESCHED. Closes R5-H F77 + F78.
12. **P3-H: Phase 3 closing audit**.

Phase 3 audit-trigger surfaces (per ARCHITECTURE.md §25.4): `mm/vm.c` (new), `arch/arm64/fault.c` (new), kernel/elf.c mapping helpers, exec syscall, per-Proc TTBR0 swap, `arch/arm64/asid.c` (added at P3-Ba).

---

## Verify on session pickup

```bash
git log --oneline -8
# Expect:
#   8695dd3 P3-Bb: hash fixup
#   8a888e1 P3-Bb: kernel direct map in TTBR1 (+ ARCH §6.10 / NOVEL §3.9 D)
#   24e565d P3-Ba: hash fixup
#   2095a6c P3-Ba: ASID allocator
#   4bfc1bd P3-A R6-A: hash fixup
#   402f5ca P3-A R6-A audit close
#   d6633e2 P3-A: hash fixup + handoff 015
#   9d5c271 P3-A: F75 close

git status
# Expect: clean (only docs/estimate.md, loc.sh untracked)

tools/build.sh kernel
tools/test.sh
# Expect: 56/56 PASS, ~260 ms boot.

tools/test.sh --sanitize=undefined
# Expect: 56/56 PASS, ~301 ms boot.

tools/test-fault.sh
# Expect: 4/4 PASS.

tools/verify-kaslr.sh -n 5
# Expect: 5/5 distinct.
```

---

## Trip-hazards added at P3-Ba/Bb

### P3-Ba (cumulative #104-107)

104. **ASID 0 is RESERVED for kernel TTBR1 mappings**. User-Proc allocation must use `asid_alloc` which structurally excludes 0.
105. **`asid_free` MUST run before the Proc's struct is reaped**. The TLB flush targets a specific ASID; freeing the Proc without asid_free leaks the ASID and leaves stale TLB entries.
106. **No ASID rollover at v1.0**. Hard exhaustion at 255 lifetime ASIDs without a single free extincts. Phase 5+ adds Linux-style generation rollover.
107. **`asid_free`'s TLB flush is INNER-SHAREABLE BROADCAST**. Cost is multi-cycle. Many-core scaling concern.

### P3-Bb (cumulative #108-112)

108. **Direct map covers PA 1..9 GiB**. Larger physical memory needs the L1 bump extended in mmu.c. v1.0 caps at 8 GiB.
109. **`kva_to_pa` REQUIRES a direct-map KVA as input**. Passing a kernel-image VA (high TTBR1 KASLR address) returns an arithmetically-meaningful but semantically-wrong PA (because kernel-image VAs ALSO have bits 63:48 = 0xFFFF; the AND would clear them and leave the lower bits as the KASLR-randomized image VA, NOT the PA). Future kernel code handling arbitrary kernel pointers needs an `is_directmap_kva()` predicate.
110. **`mmu_map_mmio` cursor is NOT lock-protected**. v1.0 single-thread boot calls; Phase 3+ multi-thread callers (driver init, exec) MUST acquire a vmalloc lock.
111. **TTBR0 identity is still alive at P3-Bb**. Many code paths still rely on it (DTB, kstack, MMIO via mmu_map_device). P3-Bd removes; the listed paths MUST refactor before then.
112. **Direct-map alias safety**: same physical RAM is mapped at TWO VAs (TTBR0 identity + TTBR1 direct map), both Normal-WB. ARM ARM B2.8 allows this. But: Device-nGnRnE alias of RAM (e.g., test_directmap.vmalloc_mmio_smoke) is CONSTRAINED UNPREDICTABLE per ARM ARM. The test only verifies plumbing (KVA in range), not access through the Device alias.

---

## Open follow-ups

- **R6-B audit findings**: when it returns, triage + close P0/P1/P2.
- **R5-H carried-over deferrals** (still relevant):
  - F77 + F78 (P2): bundled with P3-G (ready/wakeup IPI_RESCHED).
  - F81/F82/F84/F85 (P2): scheduler optimization deferred.
- **R6-A carried-over deferrals**: F108/F109/F110 (P3, all v1.x).
- **R5-F/G P3 deferrals**: see prior closed-lists.
- **P3-Bc: refactor TTBR0-identity callers** — DTB / kstack / MMIO. Substantial.
- **v1.x hardening**: KASLR-randomize the direct-map base (currently fixed `0xFFFF_0000_0000_0000`).
- **v2.x architectural goal**: capability-based kernel addressing (NOVEL §3.9 D / ARCH §6.10).

---

## Closing notes

P3-Bb is a substantial chunk. The pattern: design discovery (SLUB PA-as-VA pun blocks per-Proc TTBR0 swap) → user discussion (the direct map IS the principled-enough v1.0 answer; v2.x is capabilities) → architectural commitment (NOVEL Contract D + ARCH §6.10 added) → implementation (direct map + SLUB refactor + tests + reference doc) → audit (R6-B in flight).

The architectural meta-discussion was load-bearing. v1.0's direct-map design is now EXPLICITLY a compromise rather than implicitly the only option, with the v2.x principled path documented and the v1.0-survives-the-migration commitments enumerated. This makes future Rust + CHERI work tractable rather than a multi-year rewrite from scratch.

P3-Bc is the next sub-chunk. Substantial scope (refactor every TTBR0-identity caller). Recommend `/compact` after R6-B if findings allow; P3-Bc deserves fresh context.

The thylacine maps its own pages.
