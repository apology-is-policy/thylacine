# Handoff 002 — Phase 1 mid-late (post test-harness)

**Date**: 2026-05-04
**Tip commit**: `695c0cc` (test-harness hash-fixup)
**Author**: Claude Opus 4.7 (1M context)
**Audience**: the next Claude Code session (or human collaborator) picking this up.
**Predecessor**: `docs/handoffs/001-phase1-mid.md` (P1-C close, 2026-05-04 earlier).

This is the second formal handoff. It covers the substantial Phase 1 progress since handoff 001 — six landed chunks (P1-C-extras Parts A and B, P1-D, P1-E, P1-F, plus the cross-cutting test harness) — and sets the stage for P1-G (timer + GIC + IRQ-driven UART). A fresh session should read this top-to-bottom; everything in handoff 001 still applies for the design intent and project-wide policies, but the implementation state has moved a long way.

---

## TL;DR

- **Phase 1 momentum**: P1-A, B, C, C-extras (A+B), D, E, F all landed. Plus a cross-cutting in-kernel test harness with 4 leaf-API tests (4/4 PASS every boot).
- **Invariants live**: I-12 (W^X — PTE constructors with `_Static_assert`), I-15 (DTB-driven HW discovery — UART base, memory range, kaslr/rng-seed), I-16 (KASLR — 13-bit randomized 2 MiB-aligned offset in [0, 16 GiB) within `0xFFFFA00*` slot).
- **Kernel posture**: boots in ~50 ms to a banner showing 2030 MiB free of 2048 MiB (P1-D) → 2022 MiB free of 2048 MiB (post-P1-E with 24 MiB struct page array). Vector table armed (sync faults route to extinction with diagnostics). Tests run on every boot.
- **Next chunk: P1-G** (ARM generic timer + GIC v2/v3 autodetect + IRQ-driven UART). The IRQ vector slot in `vectors.S` currently routes to `exception_unexpected`; P1-G replaces with a GIC dispatch.

---

## Current state of the world

### Build

- **Toolchain**: clang 22.1.4 + ld.lld 22.1.4 (Homebrew), cmake 4, qemu 10. Cross-compile target `aarch64-none-elf`. Apple Silicon Mac under Hypervisor.framework.
- **Build flags** (in `cmake/Toolchain-aarch64-thylacine.cmake`):
  - Compile: `-fpie -fdirect-access-external-data -mcmodel=tiny -ffreestanding -fno-builtin -fno-common -fno-stack-protector -mgeneral-regs-only -mno-outline-atomics -fno-omit-frame-pointer -O2 -g -std=c99`.
  - Link: `-Wl,-pie -Wl,-z,text -Wl,-z,norelro -Wl,-z,nopack-relative-relocs -Wl,--no-dynamic-linker -nostdlib -nostartfiles -static`.
- **Build invocation**: `tools/build.sh kernel` (or `make kernel`). Produces `build/kernel/thylacine.elf` (~175 KB debug) and `build/kernel/thylacine.bin` (~25 KB flat binary).
- **Phase tag**: `THYLACINE_PHASE=P1-F` (CMake cache; banner reads it). Bump to `P1-G` when P1-G lands.

### What runs

```
$ make test
==> PASS: boot banner observed.
Thylacine v0.1.0-dev booting...
  arch: arm64
  el-entry: EL1 (direct)
  cpus: 1 (P1-C-extras; SMP at P1-F)
  mem:  2048 MiB at 0x0000000040000000
  dtb:  0x0000000048000000 (parsed)
  uart: 0x0000000009000000 (DTB-driven)
  hardening: MMU+W^X+extinction+KASLR+vectors (P1-F; PAC/MTE/CFI at P1-H)
  kernel base: 0xffffa00220e80000 (KASLR offset 0x0000000220e00000, seed: DTB /chosen/kaslr-seed)
  ram: 2048 MiB total, 2022 MiB free, 26224 KiB reserved (kernel + struct_page + DTB)
  tests:
    [test] kaslr.mix64_avalanche ... PASS
    [test] dtb.chosen_kaslr_seed_present ... PASS
    [test] phys.alloc_smoke ... PASS
    [test] slub.kmem_smoke ... PASS
  tests: 4/4 PASS
  phase: P1-F
Thylacine boot OK
```

The kernel halts in a WFI loop after the banner. KASLR offset varies per boot (5+ verified distinct).

### Layout on disk

```
thylacine/
├── arch/arm64/
│   ├── kernel.ld           # linker script (links at KERNEL_LINK_VA = 0xFFFFA00000080000)
│   ├── start.S             # Linux ARM64 image header + EL2→EL1 drop + KASLR boot
│   ├── mmu.h, mmu.c        # MMU + W^X (P1-C); TTBR1 high-half mapping (P1-C-extras Part B)
│   ├── kaslr.h, kaslr.c    # KASLR (P1-C-extras Part B); kernel_pa accessors (P1-D)
│   ├── exception.h, exception.c, vectors.S   # Exception handling (P1-F)
│   ├── uart.h, uart.c      # PL011 polled (P1-A; DTB-driven from P1-B)
│
├── kernel/
│   ├── main.c              # boot_main(): 12-step entry sequence
│   ├── extinction.c        # ELE primitive + ABI prefix
│   ├── include/thylacine/
│   │   ├── types.h
│   │   ├── dtb.h           # FDT parser API
│   │   ├── extinction.h
│   │   ├── page.h          # struct page (48 bytes), PG_*, KP_* (P1-D + P1-E)
│   │   └── spinlock.h      # stub at P1-D; LL/SC at P1-F+
│   ├── test/               # In-kernel test harness (post-P1-F)
│   │   ├── test.h, test.c
│   │   ├── test_kaslr.c
│   │   ├── test_dtb.c
│   │   ├── test_phys.c
│   │   └── test_slub.c
│   └── CMakeLists.txt
│
├── lib/
│   └── dtb.c               # FDT parser (P1-B) + chosen helpers (P1-C-extras + P1-D)
│
├── mm/                     # P1-D + P1-E
│   ├── buddy.h, buddy.c    # Knuth buddy, orders 0..18
│   ├── magazines.h, magazines.c   # Per-CPU stacks at orders 0 and 9
│   ├── phys.h, phys.c      # Public alloc API + DTB-driven bootstrap
│   └── slub.h, slub.c      # SLUB kernel object allocator
│
├── tools/
│   ├── build.sh, run-vm.sh, test.sh    # unchanged from P1-A
│
├── docs/
│   ├── VISION.md, ARCHITECTURE.md, ROADMAP.md, COMPARISON.md, NOVEL.md, TOOLING.md
│   ├── REFERENCE.md        # as-built index (snapshot block tracks tip)
│   ├── reference/
│   │   ├── 00-overview.md
│   │   ├── 01-boot.md      # entry sequence (extended for every chunk)
│   │   ├── 02-dtb.md       # FDT parser
│   │   ├── 03-mmu.md       # MMU + W^X (TTBR0+TTBR1 post-KASLR)
│   │   ├── 04-extinction.md # ELE primitive
│   │   ├── 05-kaslr.md     # KASLR (P1-C-extras Part B)
│   │   ├── 06-allocator.md # Physical allocator (P1-D)
│   │   ├── 07-slub.md      # SLUB (P1-E)
│   │   ├── 08-exception.md # Exception handling (P1-F)
│   │   └── 09-test-harness.md   # Test runner (post-P1-F)
│   ├── USER-MANUAL.md, manual/00-overview.md   # mostly empty pre-Utopia
│   ├── phase1-status.md    # active phase pickup
│   ├── handoffs/
│   │   ├── 001-phase1-mid.md     # P1-C close
│   │   └── 002-phase1-mid-late.md   # ← this document
│
├── build/                  # gitignored
├── share/                  # gitignored (9P host share for QEMU)
├── CLAUDE.md
└── .gitignore
```

### Memory state files (auto-loaded for the next session)

```
~/.claude/projects/-Users-northkillpd-projects-thylacine/memory/
├── MEMORY.md                       # one-line index
├── project_active.md               # current state — kept current per chunk
├── project_next_session.md         # pickup pointer with decision tree
├── user_profile.md                 # Michal: senior engineer; Stratum + Thylacine
├── project_motto.md                # "The thylacine runs again."
└── feedback_reference_discipline.md  # maintain technical + user references per-chunk
```

---

## Read order for the next session

1. `CLAUDE.md` — operational framework. Mandatory.
2. **This document** — second comprehensive handoff at post-test-harness state.
3. `docs/handoffs/001-phase1-mid.md` — first handoff at P1-C close. Useful for project-wide context that hasn't changed (Halcyon-as-last-phase reorder, scripture status, naming conventions).
4. `docs/VISION.md` §1 (mission), §3 (properties), §13 (Utopia milestone).
5. `docs/ARCHITECTURE.md` §6 (memory), §12 (interrupt handling — P1-G's design intent), §24 (hardening), §28 (invariants).
6. `docs/ROADMAP.md` §4 (Phase 1).
7. `docs/TOOLING.md` §3 (run-vm.sh), §10 (boot banner / EXTINCTION ABI).
8. `docs/phase1-status.md` — landed-chunks table goes through `c3f9196` (test harness). The "Remaining work" section lists P1-G onward.
9. The relevant subsystem reference under `docs/reference/` for whatever you're touching:
   - **P1-G**: `08-exception.md` (existing IRQ slot), `02-dtb.md` (GIC compat strings), `01-boot.md` (entry sequence).
   - **P1-H**: `03-mmu.md` (W^X already in place), `06-allocator.md` (will need stack canary integration).
   - **P1-I**: `09-test-harness.md` (host-side target lands here).

When in doubt, scripture wins. Per CLAUDE.md "Reference documentation discipline": if scripture, technical reference, code, and user reference disagree, the order of authority is spec → technical reference → code → user reference, with `ARCHITECTURE.md` as the authoritative design-intent source.

---

## What landed since handoff 001 (P1-C close → today)

### `ff22ca3` / `d64f029` — P1-C-extras Part A: EL2→EL1 drop + boot-stack guard page

- `arch/arm64/start.S` gained the canonical EL2→EL1 drop sequence (HCR_EL2.RW=1, CNTHCTL_EL2 timer access, CNTVOFF_EL2=0, VTTBR_EL2=0, SCTLR_EL1=`INIT_SCTLR_EL1_MMU_OFF` (`0x30D00800`), SPSR_EL2=EL1h+DAIF, ELR_EL2=`.Lpost_el2_drop`, eret). QEMU virt always enters at EL1; this is Pi 5 prep + future-proofing.
- New BSS variable `_entered_at_el2` drives the `el-entry: EL1 (direct)` / `el-entry: EL2 -> EL1 (dropped)` banner line.
- Boot-stack guard page: 4 KiB BSS slot at `_boot_stack_guard` (immediately below `_boot_stack_bottom`) whose L3 PTE is zeroed by `mmu.c`. Stack overflow now triggers a translation fault on the guard region rather than silently corrupting BSS.
- The fault diagnostic (`extinction("kernel stack overflow", FAR_EL1)`) deferred to P1-F (when fault handlers landed).

### `74fd391` / `b1ff9e2` — P1-C-extras Part B: KASLR (invariant I-16)

- Toolchain flipped to `-fpie -fdirect-access-external-data -mcmodel=tiny`; linker to `-Wl,-pie -Wl,-z,text -Wl,-z,norelro -Wl,-z,nopack-relative-relocs -Wl,--no-dynamic-linker`.
- Kernel now links at `KERNEL_LINK_VA = 0xFFFFA00000080000` with `AT(KERNEL_LOAD_PA = 0x40080000)`. `.rela.dyn` retained (currently empty thanks to `-fdirect-access-external-data`).
- New `arch/arm64/kaslr.{h,c}` (~150 LOC). Entropy chain: `/chosen/kaslr-seed` → `/chosen/rng-seed` → `cntpct_el0` fallback. SipHash-style mix function (`mix64`). 13-bit (8192-bucket) 2 MiB-aligned offset in `[0, 16 GiB)`. `.rela.dyn` walker is in place (no entries to apply today).
- `arch/arm64/mmu.c` extended with TTBR1 high-half mapping at `KERNEL_LINK_VA + slide` using new `l0_ttbr1` / `l1_ttbr1` / `l2_ttbr1` BSS tables and the SHARED `l3_kernel`. `mmu_enable(u64 slide)` signature change.
- `arch/arm64/start.S` calls `kaslr_init` after BSS clear, passes the slide to `mmu_enable`, then long-branches into TTBR1 via `kaslr_high_va_addr` + `br x0`.
- `lib/dtb.c` split `dtb_get_chosen_kaslr_seed` (kaslr-seed only) from new `dtb_get_chosen_rng_seed` (rng-seed only).
- Banner: `kernel base: 0x...` + `KASLR offset 0x...` + `seed: <source>` varies per boot.

### `198c48c` / `40e74c8` — P1-D: physical allocator (buddy + per-CPU magazines)

- New `mm/` directory (root-level alongside `arch/`, `kernel/`, `lib/`).
- `mm/buddy.{h,c}` (~280 LOC) — Knuth buddy; one zone (single NUMA node at v1.0); orders 0..18 (4 KiB to 1 GiB); doubly-linked sentinel-headed free lists with `pfn ^ (1<<order)` merge.
- `mm/magazines.{h,c}` (~110 LOC) — per-CPU stacks of 16 entries at orders 0 and 9 (Bonwick illumos-kmem-style); refill / drain to half-full. `magazines_drain_all` for clean accounting.
- `mm/phys.{h,c}` (~170 LOC) — DTB-driven bootstrap: reserves kernel image + struct page array + DTB blob + low firmware area; pushes the rest onto buddy. Public API: `alloc_pages` / `free_pages` / `kpage_alloc` (PA-as-`void*` at v1.0; high-VA at Phase 2).
- `kernel/include/thylacine/page.h` — `struct page` (32 bytes initially; grows to 48 at P1-E).
- `kernel/include/thylacine/spinlock.h` — no-op stub at P1-D; LL/SC at P1-F+; LSE at P1-H.
- `arch/arm64/kaslr.{h,c}` gained `kaslr_kernel_pa_start` / `kaslr_kernel_pa_end` accessors with **`volatile`** storage to defeat clang `-O2 -fpie -mcmodel=tiny` constant fold (load-bearing — see Trip Hazards below).
- Banner: `ram: X MiB total, Y MiB free, Z KiB reserved` + `alloc smoke: PASS/FAIL`.

### `e867e3b` / `0478726` — P1-E: SLUB kernel object allocator

- `mm/slub.{h,c}` (~370 LOC) — per-cache partial slab list + per-slab embedded freelist (free objects' first 8 bytes thread the linked list — zero metadata overhead). Standard `kmalloc-{8..2048}` caches in BSS; sizes > 2 KiB bypass slab via direct `alloc_pages`. Bootstrap via static `g_meta_cache` for `struct kmem_cache` itself.
- Public API: `kmem_cache_create` / `_destroy` / `_alloc` / `_free` + `kmalloc` / `kzalloc` / `kcalloc` / `kfree`.
- `struct page` extended from 32 to 48 bytes with `slab_freelist` + `slab_cache` fields; new `PG_SLAB` flag. struct page array grew from 16 MiB to 24 MiB on 2 GiB RAM.
- Banner: `kmem smoke: PASS/FAIL`.

### `67a6b16` / `619426a` — P1-F: exception vector table + sync handler

- `arch/arm64/vectors.S` (~150 LOC) — 16-entry table at `_exception_vectors`, page-aligned (0x800), with KERNEL_ENTRY save (288-byte context: 31 GP regs + sp_el0 + elr + spsr + esr + far). KERNEL_EXIT defined for Phase 2's recoverable handlers (unused inline at P1-F).
- `arch/arm64/exception.{h,c}` (~250 LOC). `exception_init` sets VBAR_EL1. `exception_sync_curr_el` decodes ESR/FAR; routes stack-overflow detection (FAR in `[_boot_stack_guard, _boot_stack_bottom)`) to `extinction("kernel stack overflow", FAR_EL1)`; W^X violations on kernel image to `extinction("PTE violates W^X (kernel image)")`; other faults to specific extinctions. `exception_unexpected` catch-all for the 15 non-live entries with descriptive name table.
- The deferred fault paths from P1-C-extras Part A (boot-stack guard) and P1-C (W^X PTE constructors) finally close.
- Banner: hardening line gains `+vectors`.

### `c3f9196` / `695c0cc` — Test harness (cross-cutting)

- `kernel/test/test.{h,c}` (~120 LOC) — sentinel-terminated `g_tests[]` registry, TEST_ASSERT macro, per-test PASS/FAIL on UART. `test_run_all` walks the array; `test_all_passed` gates the boot path.
- Four leaf-API tests at landing: `kaslr.mix64_avalanche`, `dtb.chosen_kaslr_seed_present`, `phys.alloc_smoke` (refactored), `slub.kmem_smoke` (refactored).
- New `kaslr_test_mix64` wrapper in `kaslr.{h,c}` exposes the static `mix64`.
- `kernel/main.c` replaces inline smoke blocks with a single `test_run_all()` call. Banner adds `tests: N/N PASS|FAIL` summary.
- **What we don't test**: internal data-structure invariants of evolving subsystems, evolving APIs (scheduler, namespace, handles, 9P), concurrency, sanitizer-instrumented runs. These wait for P1-I (sanitizer matrix) + Phase 2 spec gates.

---

## Trip hazards — durable, learned-from-experience

(Cumulative since handoff 001. Items 1-4 carry over; items 5-12 are new since the test harness landing.)

### 1. Compiler fusion + Device memory alignment (carried from handoff 001)

**With MMU off**, all kernel data accesses are Device-nGnRnE. clang fuses adjacent 4-byte loads into 8-byte loads which fault on 4-aligned-but-not-8-aligned property data.

**Mitigation in place**: `lib/dtb.c`'s `be32_load` uses `*(const volatile uint32_t *)p`. Document: `docs/reference/02-dtb.md` "Byte-order helpers — IMPORTANT".

**At P1-C and later** the MMU is on with cacheable Normal memory; the constraint is no longer load-bearing for normal ops, but **keep the volatile** — defends bare-metal recovery paths and any future code touching pre-MMU memory.

### 2. QEMU `-kernel` ELF entry without Linux header doesn't load DTB (handoff 001)

QEMU's `load_aarch64_image()` only fires when the binary has the ARM64 image magic at offset 0x38. The mitigation (Linux ARM64 image header + flat-binary build) is in place since P1-B. If `_saved_dtb_ptr == 0` on boot, check that `tools/run-vm.sh` is using `thylacine.bin` not `thylacine.elf`.

### 3. W^X invariant is encoded in PTE constructors (handoff 001 + extended)

`arch/arm64/mmu.h` defines `PTE_KERN_TEXT` / `PTE_KERN_RO` / `PTE_KERN_RW` / `PTE_KERN_RW_BLOCK` / `PTE_DEVICE_RW_BLOCK`. **Use these helpers**. Don't construct PTEs by hand from raw bits — `_Static_assert`s fail the build if W^X is violated.

`pte_violates_wxe(pte)` is the runtime check — used by `exception_sync_curr_el` for the W^X-violation diagnostic.

### 4. EXTINCTION: prefix is kernel ABI (handoff 001)

Per TOOLING.md §10. Don't change `"EXTINCTION: "` or `"Thylacine boot OK"` without coordinated updates to `tools/run-vm.sh`, `tools/test.sh`, `tools/agent-protocol.md`, `CLAUDE.md`, and `TOOLING.md`.

### 5. **NEW** — `volatile` on `g_kernel_pa_*` in `arch/arm64/kaslr.c` is load-bearing

Without `volatile`, clang at `-O2 -fpie -mcmodel=tiny` folds `g_kernel_pa_start = (uintptr_t)_kernel_start` into "store boolean 1, return link-time address gated on it". Symbol size in `readelf` becomes 1 byte instead of 8. At runtime under PIE the link-time address ≠ load PA, so `phys_init` reads bogus values.

The fix is `static volatile u64 g_kernel_pa_start;`. Documented inline in `kaslr.c` with the diagnostic recipe (`readelf --syms | grep g_kernel_pa` should show 8-byte size).

If a similar fold appears in P1-G (e.g., when caching GIC base derived from extern DTB lookup), the recipe is the same.

### 6. **NEW** — `KASLR_LINK_VA` lives in TWO places

`KASLR_LINK_VA = 0xFFFFA00000080000` is in `arch/arm64/kaslr.h` AND `arch/arm64/kernel.ld` (as `KERNEL_LINK_VA`). Linker `ASSERT(_kernel_start == KERNEL_LINK_VA)` enforces equality. **Both must change together**. PIC mode means C code can't read absolute symbols, so the constant duplication is unavoidable.

### 7. **NEW** — TTBR0 stays active post-KASLR

After KASLR, TTBR1 holds the kernel high-half mapping but TTBR0 keeps the low-4-GiB identity map. Kernel data accesses to absolute PAs (DTB, MMIO, `kpage_alloc`'s PA-as-`void*` returns) translate through TTBR0. **Phase 2 retires TTBR0** when user namespaces start to live there. Until then, the kernel can read PAs by absolute addressing — wider attack surface, but mitigated by the absence of user code at v1.0-pre-Phase-2.

### 8. **NEW** — `l3_kernel` is shared between TTBR0 and TTBR1

The same `l3_kernel` page-grain table is referenced by L2 entries in both translation roots. Same kernel image, two VA paths. Watch for this when extending `mmu.c` — modifying `l3_kernel` affects both lookups simultaneously.

### 9. **NEW** — Magazine residency

The first allocation at order 0 or 9 triggers a refill of 8 from the buddy. The magazine retains 7 net pages. Use `magazines_drain_all` before exact `phys_free_pages()` comparisons (the smoke tests in `test_phys.c` and `test_slub.c` do this).

### 10. **NEW** — `kpage_alloc` returns PA-cast-to-`void*` at v1.0

At P1-D / P1-E, `kpage_alloc(flags)` returns `(void *)(uintptr_t)page_to_pa(p)` — a low-VA pointer dereferenceable through TTBR0 identity. Phase 2 promotes to high-VA direct-map pointer when TTBR0 retires. Callers that treat as opaque pointer don't change; callers that compute PAs from it will need review at Phase 2.

### 11. **NEW** — Recursive fault on stack overflow

P1-F's KERNEL_ENTRY pushes 288 bytes onto SP_EL1. If the fault that fired was itself a stack overflow, the push faults again, recursively, until QEMU wedges. The diagnostic chain produces no output to UART before the wedge.

**Mitigation**: per-CPU exception stack lands at Phase 2 with thread machinery. For v1.0: `boot_main` does no significant stack work, so the boot path itself is safe; stack overflow in non-boot code (P1-G+ IRQ handlers) must be careful with stack budget.

### 12. **NEW** — `struct page` is now 48 bytes (was 32 bytes pre-P1-E)

Adds two SLUB-only fields (`slab_freelist`, `slab_cache`). Valid only when `flags & PG_SLAB`. struct page array for 2 GiB RAM is 24 MiB (was 16 MiB at P1-D). Banner reports the larger reservation.

### 13. **NEW** — `kmem_cache_destroy` doesn't audit live objects

Caller is responsible for freeing every allocation before destroying the cache. Debug-mode cookies / bitmap land at P1-I.

### 14. **NEW** — No double-free detection in SLUB

`kmem_cache_free` checks `PG_SLAB` and `slab_cache` match but not whether the object is already on the freelist. Double-free corrupts the freelist (split/cycle). P1-I adds detection. Until then the audit policy is "match every kmalloc with exactly one kfree."

### 15. **NEW** — CMake cache for `THYLACINE_PHASE`

Changing `THYLACINE_PHASE` in `CMakeLists.txt` doesn't take effect on incremental rebuilds — clean-build (`rm -rf build/kernel`) or `cmake --fresh`. The banner uses the cached value.

---

## Naming conventions established

- **Kernel panic = "extinction"**. Function: `extinction(msg)`. Prefix: `EXTINCTION:`. Theme: ELE = Extinction Level Event.
- **Project motto: "The thylacine runs again."** Reserved for end-of-phase / release / milestone moments per `memory/project_motto.md`. Don't wear out.
- **Test names use dotted notation**: `<subsystem>.<test_name>` (e.g., `kaslr.mix64_avalanche`, `phys.alloc_smoke`). Subsystem prefix groups related tests in runner output.
- **Names already chosen and stable**: Thylacine (OS), Stratum (FS), Halcyon (shell, Phase 8), janus (key agent, Phase 4), `_saved_dtb_ptr`, `_image_size`, `_entered_at_el2`, `_boot_stack_guard`, `_exception_vectors`.
- **Held for explicit signoff before applying**:
  - `_hang` (the WFI loop) → `_torpor` (marsupial deep-sleep state).
  - Audit-prosecutor agent name → stays "prosecutor" for Stratum continuity.

The user said "don't be shy suggesting more fitting names along the way." If a fitting name surfaces, mention it; defer the rename to explicit signoff before touching code.

---

## What's next

### Decision tree for the next chunk

**Option A (recommended) — P1-G: GIC v2/v3 + ARM generic timer + IRQ-driven UART**.

Sub-deliverables:

- `arch/arm64/gic.{h,c}` — autodetect GIC version via DTB compatible string. QEMU virt and Pi 5 ship GICv3; GICv2 fallback for older boards (Pi 4 etc., post-v1.0). Distributor + system-register CPU interface (v3) or distributor + MMIO CPU interface (v2). API: `gic_init` / `gic_enable_irq(irq, prio)` / `gic_acknowledge() → irq` / `gic_eoi(irq)`.
- `arch/arm64/timer.{h,c}` — ARM generic timer at 1000 Hz. CNTP_TVAL_EL0 reload on each IRQ; CNTP_CTL_EL0 enable; tick counter incremented in handler.
- `arch/arm64/exception.{h,c}` extension — replace the IRQ slot's `exception_unexpected` route with `exception_irq_curr_el` that calls `gic_acknowledge` → dispatch table → `gic_eoi`.
- `arch/arm64/vectors.S` — repoint the Current-EL-SPx IRQ entry from `VEC_UNEXPECTED 5` to `KERNEL_ENTRY + bl exception_irq_curr_el + b _hang`.
- `arch/arm64/uart.c` extension — real PL011 init (CR/LCR_H/IBRD/FBRD); IRQ-driven TX with circular buffer; UART IRQ wired through GIC.
- New leaf-API tests in `kernel/test/`: `gic.init_smoke` (verifies GIC distributor enabled); `timer.tick_increments` (verifies the tick counter advances over a wfi loop).
- Banner ends with a tick counter that increments visibly: `tick: 5` after a brief wait loop.

Estimated 800-1200 LOC + docs; 150-200k tokens.

**Option B — Spec scaffolding**: write the first TLA+ spec sketch (e.g., `mmu.tla` for the page-table walker, or `phys.tla` for the buddy invariant). Per CLAUDE.md spec-first policy, page-table operations and allocator invariants are spec candidates. Optional at this phase per ARCH §25.2 (specs are mandatory from P2 onward; P1 specs are sketches).

**Option C — CI infrastructure**: GitHub Actions workflow for `make test` on every push. ~30k tokens. Independent of phase order; could land alongside any chunk.

**Recommendation**: A. Timer ticks are the last "live kernel signal" before Phase 2's process model. With the GIC live, every subsequent kernel chunk that needs to react to events (Phase 2 wakeups, Phase 3 device IRQs) builds on this surface. P1-G also closes the "the kernel is breathing" milestone — the kernel transitions from "boots and prints once" to "boots and runs forever, observing time."

### Sub-chunks (after P1-G)

- **P1-H** — Hardening enablement (CFI, PAC, MTE, BTI, LSE, stack canaries). Compile flags + runtime detection.
- **P1-I** — Phase 1 exit verification. ASan + UBSan instrumented builds. 10000-iteration alloc/free leak check. Host-side test target for sanitizer matrix runs. CI workflow. Phase 1 closing audit pass.

After P1-I, **Phase 2** begins: process model + EEVDF scheduler + handles + VMO + `scheduler.tla` / `namespace.tla` / `handles.tla` / `vmo.tla` formal specs.

---

## Operational notes

- **Autonomy level**: under "auto" mode, the user grants autonomy on implementation, testing, formal modeling, audit triage, commit. Always escalate format breaks, destructive operations, ARCH deviations, scope pivots, anything visible to others. See CLAUDE.md "Autonomy + escalation."
- **Pre-Utopia (Phases 1-4)**: human-primary, agent-assisted. Kernel **extinction** = stop, report, do not proceed without review.
- **Post-Utopia (Phases 5-8)**: agent-primary, human-directed.
- **Stratum repo**: at `~/projects/stratum/`. Phase 9 (9P server) is the integration target for Thylacine Phase 4.
- **TLA+ tools** at `/tmp/tla2tools.jar`. Install instructions in `specs/README.md`.

---

## Quick-reference commands

```bash
cd ~/projects/thylacine

# Build
tools/build.sh kernel
make kernel

# Run interactively (Ctrl-A x to quit QEMU)
tools/run-vm.sh
make run

# Boot test
tools/test.sh
make test

# GDB stub on :1234
tools/run-vm.sh --gdb
make gdb

# 5-boot KASLR + tests verification
for i in $(seq 1 5); do
  tools/test.sh 2>&1 | grep -E "kernel base|tests:|^Thylacine boot OK"
done

# Inspect kernel ELF
/opt/homebrew/opt/llvm/bin/llvm-objdump -d build/kernel/thylacine.elf | less
/opt/homebrew/opt/llvm/bin/llvm-readelf -SW build/kernel/thylacine.elf
/opt/homebrew/opt/llvm/bin/llvm-readelf -r build/kernel/thylacine.elf

# Verify volatile gotcha — g_kernel_pa_* should be 8 bytes
/opt/homebrew/opt/llvm/bin/llvm-readelf --syms build/kernel/thylacine.elf | grep g_kernel_pa

# Verify struct page is 48 bytes
/opt/homebrew/opt/llvm/bin/llvm-readelf --syms build/kernel/thylacine.elf | grep _g_struct_pages

# Inspect QEMU's auto-generated DTB
qemu-system-aarch64 -machine virt -cpu max -m 2G -machine dumpdtb=/tmp/virt.dtb -nographic
dtc -I dtb -O dts /tmp/virt.dtb | less

# Run TLA+ specs (none yet — first lands Phase 2)
cd specs && for s in *.tla; do
  java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock \
    -config "${s%.tla}.cfg" "$s" 2>&1 | tail -3
done

# Clean
tools/build.sh clean
make clean

# Force rebuild (CMake cache vars need clean rebuild)
rm -rf build/kernel && tools/build.sh kernel

# Git
git log --oneline -20
git diff HEAD^
```

---

## Posture summary

| Metric | Value |
|---|---|
| Tip commit | `695c0cc` (test-harness hash-fixup) |
| Phase | Phase 1 — P1-F + test harness closed |
| Next chunk | P1-G (timer + GIC + IRQ-driven UART) |
| Build matrix | default Debug — green |
| `tools/test.sh` | PASS |
| In-kernel tests | 4/4 PASS (kaslr.mix64_avalanche, dtb.chosen_kaslr_seed_present, phys.alloc_smoke, slub.kmem_smoke) |
| Specs | 0/9 (Phase 2 introduces first spec — `scheduler.tla`) |
| LOC | ~2280 C99 + ~370 ASM + ~75 LD + ~220 sh/cmake + ~120 test = ~3065 |
| Kernel ELF | ~175 KB debug / ~165 KB stripped |
| Kernel flat binary | ~25 KB |
| Page tables | 40 KiB BSS (TTBR0 + TTBR1; shared L3) |
| struct page array | 24 MiB BSS (struct page now 48 bytes) |
| Boot banner reserved | ~26 MiB |
| RAM free at boot | ~2022 MiB / 2048 MiB |
| KASLR offsets verified distinct | 5+ (sample) |
| Open audit findings | 0 (audit at Phase 1 exit / P1-I) |
| Open deferred items | P1-G (timer + GIC), P1-H (hardening flags), P1-I (verification + sanitizer + CI) |

---

## Stratum coordination

Stratum is feature-complete on Phases 1-7 of its own roadmap. Phase 8 (POSIX surface) is in progress. Phase 9 (9P server + Stratum extensions: `Tbind` / `Tunbind` / `Tpin` / `Tunpin` / `Tsync` / `Treflink` / `Tfallocate`) is the integration target for **Thylacine Phase 4**. Coordinate Phase 9 timeline with the Stratum project at `~/projects/stratum/`. Thylacine Phases 1-3 (where we are now) proceed in parallel with Stratum's Phase 8-9 work — no dependency.

---

## Format ABI surfaces in flight

These are stable contracts that next-session work must not break without coordinated multi-document updates:

| Surface | Where | Contract |
|---|---|---|
| Boot banner success line | `kernel/main.c` `boot_main()` | `"Thylacine boot OK\n"` |
| Extinction prefix | `kernel/extinction.c` | `"EXTINCTION: "` |
| el-entry banner line | `kernel/main.c` | `"el-entry: EL1 (direct)"` / `"EL2 -> EL1 (dropped)"` |
| KASLR banner line | `kernel/main.c` | `"kernel base: 0x..., KASLR offset 0x..., seed: <source>"` |
| ram banner line | `kernel/main.c` | `"ram: X MiB total, Y MiB free, Z KiB reserved"` |
| tests banner block | `kernel/main.c` | `"  tests:\n    [test] <name> ... PASS|FAIL"` per test, then `"  tests: N/N PASS|FAIL"` |
| Linux ARM64 image header | `arch/arm64/start.S` offset 0..0x40 | `b _real_start; nop; <text_offset>; <image_size>; <flags>; <res>; "ARM\x64"` |
| `_image_size` linker symbol | `arch/arm64/kernel.ld` | `_kernel_end - _kernel_start` |
| `_saved_dtb_ptr` BSS variable | `arch/arm64/start.S` | DTB physical address |
| `_entered_at_el2` BSS variable | `arch/arm64/start.S` | 0 / 1 EL-source flag |
| Linker section symbols | `arch/arm64/kernel.ld` | `_kernel_start` / `_text_end` / `_rodata_end` / `_data_end` / `_rela_start` / `_rela_end` / `_boot_stack_guard` / `_boot_stack_bottom` / `_boot_stack_top` / `_kernel_end` |
| `KERNEL_LINK_VA` constant | `arch/arm64/kernel.ld` + `arch/arm64/kaslr.h` | `0xFFFFA00000080000` (must match) |
| Page table layout (4-level, 4 KiB granule) | `arch/arm64/mmu.c` | TTBR0 identity (1 L0 + 1 L1 + 4 L2) + TTBR1 high-half (1 L0 + 1 L1 + 1 L2) + shared L3 = 40 KiB BSS |
| MAIR attribute indices | `arch/arm64/mmu.h` | 0=Device, 1=NormalNC, 2=NormalWB, 3=NormalWT |
| PTE constructor macros | `arch/arm64/mmu.h` | W^X-safe by `_Static_assert` |
| FDT format assumptions | `lib/dtb.c` | `#address-cells = 2`, `#size-cells = 2` (QEMU virt convention) |
| DTB consumer surface | `kernel/include/thylacine/dtb.h` | `dtb_init`, `dtb_get_memory`, `dtb_get_compat_reg`, `dtb_get_total_size`, `dtb_get_chosen_kaslr_seed`, `dtb_get_chosen_rng_seed` |
| `struct page` layout | `kernel/include/thylacine/page.h` | 48 bytes (next/prev/order/flags/refcount/_pad/slab_freelist/slab_cache); `_Static_assert` could verify (currently doesn't) |
| Page flags | `kernel/include/thylacine/page.h` | `PG_FREE` / `PG_RESERVED` / `PG_KERNEL` / `PG_SLAB` |
| Allocation flags | `kernel/include/thylacine/page.h` | `KP_ZERO` / `KP_DMA` / `KP_NOWAIT` / `KP_COMPLETE` |
| `struct exception_context` | `arch/arm64/exception.h` | 288 bytes; field offsets `_Static_assert`'d in `exception.c` |
| `struct test_case` | `kernel/test/test.h` | `name` / `fn` / `failed` / `fail_msg`; sentinel = `{NULL, NULL, false, NULL}` |

Changing any of these requires a documented rationale in the commit message + updates to `docs/reference/`.

---

## Things I would NOT recommend deviating from (cumulative lessons)

- **Always use the helper macros** (`PTE_KERN_*`, `read_reg_pair`, `be32_load`). They're load-bearing.
- **`tools/test.sh` is the canonical test gate**. Pre-commit; if it fails, don't commit. The 4 in-kernel tests run inside it now too.
- **Doc-update-per-PR**. If you touch `arch/arm64/exception.c`, update `docs/reference/08-exception.md` in the same commit. Per CLAUDE.md "Reference documentation discipline."
- **Two commits per substantive landing**: substantive + hash-fixup. The hash-fixup commits make `git log --oneline` instantly readable.
- **Volatile in `be32_load` AND `g_kernel_pa_*`** — load-bearing. Don't optimize them away.
- **Don't add tests for evolving subsystems** — write them when the API stabilizes. The user explicitly chose "test only stable" to avoid rewriting.
- **Test names use dotted notation** (`<subsystem>.<test_name>`). Stay consistent.

---

## Open questions / future-work tags

These are tracked here so they don't get lost across sessions:

- **U-1** (carried from handoff 001): Should `extinction()` print a register dump? Currently it prints just the message. Useful for fault handlers when the dump is meaningful. **DEFERRED**: P1-G has the exception infrastructure; could add at P1-G or P1-H.
- **U-2** (carried): Should we add a `dmesg`-equivalent kernel log buffer? Currently every kernel print goes straight to UART. Defer to Phase 2 once we have processes to consume it.
- **U-3** (resolved at P1-C-extras Part B): KASLR seed entropy. Implemented as kaslr-seed → rng-seed → cntpct fallback chain.
- **U-4**: `mmu.tla` formal spec. Page-table walking under concurrent updates (relevant when SMP arrives in Phase 2). Not load-bearing at v1.0; consider for Phase 2.
- **U-5**: Hardened malloc / Scudo for userspace heap. Lands when musl port arrives at Phase 5.
- **U-6** (NEW): Per-CPU exception stack. Mitigates recursive fault on stack overflow. Lands at Phase 2 with thread machinery.
- **U-7** (NEW): Multi-page slabs (`slab_order > 0`) for objects > 2048 bytes within `kmem_cache_create`. Currently rejected; large `kmalloc` works via `alloc_pages` direct path. Bump if a Phase 2+ caller needs a typed cache for large objects.
- **U-8** (NEW): SLUB debug mode (red zones, poison patterns, double-free detection). Lands at P1-I or post-v1.0.
- **U-9** (NEW): Host-side test target with sanitizer matrix. P1-I deliverable.

---

## How to write the next handoff

When the next session reaches a milestone (P1-G close, P1-I exit, Phase 2 entry, etc.), write `docs/handoffs/00N-<title>.md` following the structure of this document and handoff 001:

- TL;DR
- Current state of the world
- Read order
- What landed since the previous handoff
- Trip hazards (durable; cumulative)
- Naming conventions established
- What's next + decision tree
- Quick-reference commands
- Posture summary
- Stratum coordination status
- Format ABI surfaces in flight

Number them sequentially. The handoff thread is the project's continuity story — sessions chain through these documents. Any session reading the latest handoff plus the binding scripture can reconstruct the project's full state.

---

## Sign-off

The thylacine runs again. 🐅

(Project motto, used per `memory/project_motto.md` for milestone moments. Test-harness landing — the kernel now self-verifies on every boot — qualifies as a milestone of its own. Phase 1 has six landed chunks plus cross-cutting infrastructure; the kernel boots, allocates, faults cleanly, and tests itself. P1-G's timer ticks will close the "the kernel breathes" milestone next.)
