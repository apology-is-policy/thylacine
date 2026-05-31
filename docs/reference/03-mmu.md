# 03 — MMU + W^X (as-built reference)

The kernel's MMU bring-up. After P1-C-extras, the kernel runs with TWO live mappings sharing the kernel-image page-grain L3 tables: TTBR0 identity for the low 4 GiB (DTB and MMIO access) and TTBR1 kernel high-half at `KASLR_LINK_VA + slide`. Since P5-kernel-l3-4mib the kernel image is mapped page-grained across a **4 MiB region** by **two contiguous L3 tables** (was a single 2 MiB L3 block). Per-section permissions for the kernel image enforce W^X (invariant **I-12**) at PTE bit level. P1-C delivered the TTBR0 identity + W^X enforcement; P1-C-extras Part A added the boot-stack guard page; P1-C-extras Part B added the KASLR slide-aware TTBR1 mapping (see `docs/reference/05-kaslr.md`).

Scope: `arch/arm64/mmu.h`, `arch/arm64/mmu.c`, plus `_text_end` / `_rodata_end` / `_data_end` symbols added to `arch/arm64/kernel.ld` and the `bl mmu_enable` call inserted between BSS clear and `boot_main()` in `arch/arm64/start.S`.

Reference: `ARCHITECTURE.md §6` (memory management), `§24` (W^X invariant I-12), `§28` (enumerated invariants).

---

## Purpose

Until P1-C, every kernel data access is to **Device-nGnRnE** memory (the default with MMU off). That has two ugly consequences: aligned-only access widths (the bug we hit in P1-B with `be32_load` fusion), and uncacheable behavior — every load goes to physical memory, every write commits visibly. Boot-time enabling the MMU with cacheable normal-memory mappings, separated by section attributes, gives us:

- **Cacheable .text + .rodata** (RX / R) so instruction fetch and read-only data are fast.
- **Cacheable .data + .bss** (RW) for kernel state.
- **Device-nGnRnE for MMIO** (PL011 region; later GIC, VirtIO transports) — preserves ordering semantics.
- **W^X enforcement at PTE bit level** — invariant I-12. The PTE constructors `PTE_KERN_TEXT` / `PTE_KERN_RO` / `PTE_KERN_RW` / `PTE_DEVICE_RW_BLOCK` are guaranteed by `_Static_assert` to never set both writable and executable-at-EL1. Adding a new helper that violates this triggers a build break.

The MMU enable call sits between BSS clear and `boot_main()` in `_real_start`. From `boot_main()`'s perspective, the MMU is already on — there is no "MMU-off" path in C code.

---

## Public API

`arch/arm64/mmu.h`:

```c
// Build TTBR0 (identity, low 4 GiB) and TTBR1 (kernel high-half at
// KASLR_LINK_VA + slide) page tables, program MAIR/TCR/TTBR/SCTLR,
// turn on the MMU. Returns with caches enabled and per-section
// permissions in force; PC is still at load PA via TTBR0 (the boot
// stub long-branches into TTBR1 next).
//
// Pass slide=0 to disable KASLR (debug only).
void mmu_enable(u64 slide);

// Inspect a PTE for W^X compliance. Returns true iff writable AND
// executable-at-EL1 — the forbidden combination. Used by the audit
// path; future fault-handler can call it on a fault PTE to print
// "extinction: PTE violates W^X".
bool pte_violates_wxe(u64 pte);

// Convert MMIO region [pa, pa+size) in TTBR0's identity map from
// Normal-WB cacheable to Device-nGnRnE attributes. Granularity is
// the 2 MiB block (TTBR0's L2 entries cover the kernel-image GiB as
// 2 MiB blocks). Caller must guarantee no kernel code holds a cached
// value of any address in [pa, pa+size) before calling — used at
// boot_main time for GIC bring-up (P1-G), virtio devices (Phase 3).
//
// Implemented as break-before-make per ARM ARM B2.7: invalidate L2
// entries, dsb ishst + tlbi vmalle1is + dsb ish + isb (break), then
// write Device descriptors + dsb ishst + isb (make).
//
// Constraint: pa + size must fit in [0, 4 GiB) — TTBR0's identity
// covers only the low 4 GiB at v1.0. Returns false if unaligned or
// out of range; true on success.
bool mmu_map_device(paddr_t pa, u64 size);
```

The header also exposes the PTE bit constants (`PTE_VALID`, `PTE_AF`, `PTE_AP_*`, `PTE_PXN`, `PTE_UXN`, etc.), the MAIR attribute indices (`MAIR_IDX_DEVICE`, `MAIR_IDX_NORMAL_WB`, ...), and the canonical W^X-safe constructors (`PTE_KERN_TEXT`, `PTE_KERN_RO`, `PTE_KERN_RW`, `PTE_KERN_RW_BLOCK`, `PTE_DEVICE_RW_BLOCK`).

---

## Implementation

### Page table layout

48-bit VA, 4 KiB granule, 4 levels:

```
VA[47:39]  L0 index (each entry covers 512 GiB)
VA[38:30]  L1 index (each entry covers 1 GiB)
VA[29:21]  L2 index (each entry covers 2 MiB; 2 MiB blocks at this level)
VA[20:12]  L3 index (each entry covers 4 KiB; 4 KiB pages at this level)
VA[11:0]   page offset
```

**TTBR0 — low 4 GiB identity** (P1-C; unchanged at P1-C-extras):

- 1× **L0 table** `l0_ttbr0` (only entry 0 used; `L0[0] → l1_ttbr0`).
- 1× **L1 table** `l1_ttbr0` (entries 0..3 used, one per GiB; `L1[i] → l2_ttbr0[i]`).
- 4× **L2 tables** `l2_ttbr0[0..3]` (one per GiB; 512 entries × 2 MiB = 1 GiB each).
  - Default: 2 MiB Normal-WB RW blocks.
  - Override: the 2 MiB block containing `0x09000000` (PL011) → Device-nGnRnE block.
  - Override: the **two consecutive 2 MiB blocks** covering the kernel-image 4 MiB region → table descriptors, one pointing at `l3_kernel[0..511]` (first 2 MiB) and one at `l3_kernel[512..1023]` (second 2 MiB).
- 1× **shared L3 region** `l3_kernel`, declared `[ENTRIES_PER_TABLE * 2]` (1024 entries = two contiguous page-aligned 512-entry L3 tables) for the kernel-image 4 MiB region (page-granular permissions). `l3_kernel[0..511]` maps the first 2 MiB, `[512..1023]` the second; each half is wired as its own L3 table.
  - PTEs left at zero (PTE_VALID=0): the slot for `_boot_stack_guard`, and — since P5-secondary-stack-guard — the leading guard page of each `g_secondary_boot_stacks` slot. A stack overflow into any guard page faults synchronously rather than corrupting prior BSS (P1-C-extras Part A; secondary guards P5-secondary-stack-guard).
  - `l3_kernel` is **shared** between TTBR0 and TTBR1 — same physical kernel image accessed via both.

**TTBR1 — kernel high-half at `KASLR_LINK_VA + slide`** (P1-C-extras Part B):

- 1× **L0 table** `l0_ttbr1` (only one entry used — `L0[KASLR_L0_IDX = 0x140]` for the `0xFFFFA000_*` slot).
- 1× **L1 table** `l1_ttbr1` (one entry used — index = bits 38..30 of `KASLR_LINK_VA + slide`).
- 1× **L2 table** `l2_ttbr1` (**two consecutive entries used** — indices = bits 29..21 of `KASLR_LINK_VA + slide` and the next, one per `l3_kernel` half; the 4 MiB-aligned KASLR slide keeps both inside this one table).

Total page-table footprint: **11 × 4 KiB = 44 KiB**, BSS-allocated and zeroed by `start.S`. (Up from 28 KiB at P1-C; +12 KiB for TTBR1's L0/L1/L2, +4 KiB for the second `l3_kernel` table at P5-kernel-l3-4mib.)

### MAIR_EL1 (memory attribute encodings)

Per `mmu.h`:

| Index | Attribute | Use |
|---|---|---|
| 0 | Device-nGnRnE | MMIO (PL011, future GIC, future VirtIO transports) |
| 1 | Normal Non-Cacheable | Reserved (unused at P1-C) |
| 2 | Normal Inner+Outer Write-Back, Read+Write Allocate | Kernel memory (default) |
| 3 | Normal Inner+Outer Write-Through | Reserved (unused at P1-C) |

`MAIR_VALUE` packs the four encodings into the 64-bit MAIR register.

### Per-section permissions (W^X enforcement)

The kernel image lives at `0x40080000..0x40088000` (~32 KiB after P1-C; .bss expanded by mmu.c page tables and accumulated globals). All sections are page-aligned per the linker script.

Mapped via `l3_kernel[]` 4 KiB page descriptors:

| Section | VA range | Attributes | Helper |
|---|---|---|---|
| `.text` | `_kernel_start` .. `_text_end` | RX (AP=RO_EL1, PXN=0, UXN=1, MAIR_NORMAL_WB) | `PTE_KERN_TEXT` |
| `.rodata` | `_text_end` .. `_rodata_end` | R (AP=RO_EL1, PXN=1, UXN=1, MAIR_NORMAL_WB) | `PTE_KERN_RO` |
| `.data + .bss` | `_data_end` .. `_kernel_end` | RW (AP=RW_EL1, PXN=1, UXN=1, MAIR_NORMAL_WB) | `PTE_KERN_RW` |

**W^X invariant I-12** is encoded in the helper definitions:

- `PTE_KERN_TEXT` sets `PTE_AP_RO_EL1` (writable=false) AND clears `PTE_PXN` (executable at EL1=true). Composite: not-writable AND executable. **W^X OK.**
- `PTE_KERN_RW` sets `PTE_AP_RW_EL1` AND `PTE_PXN`. Composite: writable AND not-executable. **W^X OK.**
- `PTE_KERN_RO`: not-writable AND not-executable. **W^X OK.**
- `PTE_DEVICE_RW_BLOCK`: writable AND not-executable (Device memory; PXN). **W^X OK.**

Five `_Static_assert`s in `mmu.c` validate these claims at compile time. A future helper that breaks the rule fails the build.

`pte_violates_wxe(pte)` is the runtime check, callable from fault handlers (P1-F+) and audit tooling. It returns true exactly when both the AP bits indicate writability and `PXN=0`.

### MMU enable sequence

The order is load-bearing per ARM ARM B2.5:

```c
__asm__ __volatile__(
    "msr mair_el1, %0\n"          // memory attribute encodings
    "msr tcr_el1, %1\n"           // translation control
    "msr ttbr0_el1, %2\n"         // identity table base
    "msr ttbr1_el1, %3\n"         // kernel high-half table base (KASLR-aware)
    "isb\n"                        // sync before SCTLR change
    "mrs x9, sctlr_el1\n"
    "orr x9, x9, %4\n"             // set M | C | I
    "msr sctlr_el1, x9\n"
    "isb\n"                        // sync after MMU enable
    :: "r" ((u64)MAIR_VALUE),
       "r" ((u64)TCR_VALUE),
       "r" ((u64)(uintptr_t)l0_ttbr0),
       "r" ((u64)(uintptr_t)l0_ttbr1),
       "r" ((u64)(SCTLR_M | SCTLR_C | SCTLR_I))
    : "x9", "memory"
);
```

The kernel runs through this transition seamlessly because TTBR0 identity-maps the addresses where the kernel currently executes (PC = load PA via TTBR0). Post-MMU, every load and store goes through the page tables — but lookups resolve to the same physical addresses, so PCs and SP remain valid. The boot stub then long-branches into the high VA via `kaslr_high_va_addr` + `br x0`; from then on, code runs through TTBR1.

### TCR_EL1 configuration

| Field | Value | Meaning |
|---|---|---|
| T0SZ | 16 | TTBR0 VA size = 64 - 16 = 48 bits |
| TG0 | 0b00 | TTBR0 granule = 4 KiB |
| SH0 / IRGN0 / ORGN0 | inner-shareable + WB cacheable table walks | |
| T1SZ | 16 | TTBR1 VA size = 48 bits (unused at P1-C) |
| TG1 | 0b10 | TTBR1 granule = 4 KiB (note: TCR_EL1.TG1 encoding differs from TG0; 2 = 4KB) |
| SH1 / IRGN1 / ORGN1 | inner-shareable + WB cacheable | |
| IPS | 0b010 | 40-bit physical address (1 TiB max — sufficient) |

---

## Spec cross-reference

No formal TLA+ spec at P1-C. MMU enable is structurally simple (sequence of register writes) and the W^X enforcement is encoded in PTE constants + `_Static_assert`. A future `mmu.tla` could prove the page-table walking algorithm correct under concurrent updates (relevant when SMP arrives in Phase 2), but at P1-C it isn't load-bearing.

---

## Tests

P1-C integration test: `tools/test.sh` boots and confirms the boot banner. Banner now shows `hardening: MMU+W^X+extinction (P1-C; ...)`. PASS at landing.

Future tests (P1-F+ once exception infra lands):
- Deliberate write to `.rodata` should `extinction(...)` with a synchronous abort.
- Deliberate execute on `.data` should `extinction(...)` with an instruction abort.
- These exercise W^X enforcement at runtime and the new fault-handler path.

---

## Error paths

P1-C error paths are minimal:

| Condition | Behavior |
|---|---|
| Invalid PTE attribute combination at build time | `_Static_assert` fails the build |
| Future PTE construction violating W^X | `pte_violates_wxe()` returns true; callers `extinction(...)` |
| Page-table walker faults (e.g., TCR misconfig) | Synchronous abort; un-handled at P1-C (no exception infra yet); kernel resets |

The third row is the main risk during MMU bring-up: a misconfigured TCR or TTBR makes the MMU enable instruction itself fault. Diagnosis is by reading `ESR_EL1` and `FAR_EL1` post-mortem in QEMU — but with no exception handler installed, QEMU resets. Mitigation: keep the canonical `MAIR_VALUE` / `TCR_VALUE` / page-table-base sequence simple and well-understood. P1-F installs proper exception handlers and an `extinction()` path for these.

---

## Performance characteristics

P1-C measurements on QEMU virt under Hypervisor.framework:

| Metric | Measured | Notes |
|---|---|---|
| Kernel ELF size (debug) | 95 KB | +4 KB from P1-B; mostly mmu.c code + bigger BSS for page tables |
| Kernel flat binary | 8.2 KB | unchanged (BSS doesn't go in flat binary) |
| Page-table footprint at runtime | 28 KiB | BSS-allocated, page-aligned |
| `mmu_enable()` total cost | ~0.05 ms | one-shot at boot; dominated by TLB invalidation + cache enable |
| Boot to UART banner | ~50 ms (informal) | unchanged from P1-B; MMU adds < 0.1 ms |

---

## Status

**Implemented at P1-C**:

- 4-level page tables with 4 GiB identity map.
- Per-section permissions for kernel image (.text RX, .rodata R, .data/.bss RW).
- Device-nGnRnE for the PL011 2 MiB region.
- MAIR / TCR / TTBR / SCTLR setup in canonical order.
- MMU + caches enabled.
- W^X invariant I-12 encoded in PTE constants + `_Static_assert`.
- Linker script exposes `_text_end`, `_rodata_end`, `_data_end` for per-section page assignment.
- `bl mmu_enable` inserted in `_real_start` between BSS clear and `boot_main()`.

**Implemented at P1-C-extras Part A**:

- Boot-stack guard page: 4 KiB non-present mapping at `_boot_stack_guard` (immediately below `_boot_stack_bottom`). `build_page_tables()` zeroes the L3 PTE for the guard slot after laying down the per-section mappings. Stack overflow now faults synchronously instead of silently corrupting BSS. The fault diagnostic ("kernel stack overflow") gets wired in P1-F when exception vectors land. P5-secondary-stack-guard extends the same `build_page_tables()` step to zero the leading guard page of every `g_secondary_boot_stacks` slot — in both `l3_kernel` and the direct-map alias `l3_directmap_kernel` — so secondary-CPU boot-stack overflows fault too (closes el1h audit F1).
- EL2 → EL1 drop diagnostic. (Lives in `arch/arm64/start.S`, not `mmu.c`. Cross-referenced from `docs/reference/01-boot.md`.)

**Implemented at P1-C-extras Part B**:

- TTBR1 high-half mapping at `KASLR_LINK_VA + slide` using new BSS tables (`l0_ttbr1`, `l1_ttbr1`, `l2_ttbr1`) and the SHARED `l3_kernel` page-grain region.
- `mmu_enable(u64 slide)` signature change. Boot stub passes the slide from `kaslr_init()`. mmu.c is now KASLR-aware.
- Page table footprint grew from 28 KiB to 40 KiB (later 44 KiB at P5-kernel-l3-4mib's second `l3_kernel` table).
- KASLR slide-aware kernel high-VA mapping invariant **I-16** satisfied. See `docs/reference/05-kaslr.md` for the entropy chain, .rela.dyn walker, and long-branch.

**Implemented at P5-kernel-l3-4mib**:

- Kernel-image L3 mapping extended 2 MiB → 4 MiB: `l3_kernel` and `l3_directmap_kernel` are each declared `[ENTRIES_PER_TABLE * 2]` (two contiguous 512-entry L3 tables). TTBR0 identity, TTBR1 high-half, and the direct-map alias each now wire two consecutive 2 MiB L2 entries (one per L3-table half).
- The `kernel.ld` image assert widened to `image_size + (KERNEL_LOAD_PA & (0x400000 - 1)) < 0x400000` — cap raised from 1.5 MiB to 3.5 MiB. Driven by the UBSan-instrumented image (~1.51 MiB) outgrowing the old 1.5 MiB ceiling.
- The KASLR slide is now 4 MiB-aligned so the kernel's two 2 MiB L2 entries always land consecutively in a single `l2_ttbr1` table; pinned by a `_Static_assert(KASLR_ALIGN_BITS >= 22, ...)` in `mmu.c`. See `docs/reference/05-kaslr.md`.

**Landed**: P1-C at commit `6462227`; P1-C-extras Part A at commit `ff22ca3`; P1-C-extras Part B at commit `74fd391`.

---

## Caveats

### Caches enabled before testing them

`SCTLR.C=1` and `SCTLR.I=1` are set in the same write that enables the MMU. If the page tables are misconfigured, we lose visibility immediately (caches mask the symptom). Mitigation: test with `SCTLR.C=0` first if MMU enable goes silent in the future. We don't bother at P1-C because the implementation is cleanly traced from ARM ARM B2.5 and known-good Linux/seL4/Fuchsia patterns.

### Page tables are static (BSS-allocated)

At P1-C-extras the page tables are fixed-size BSS arrays. This is fine for identity-mapping the low 4 GiB plus a single TTBR1 kernel mapping, but won't scale to per-process address spaces (Phase 2). P1-D's physical allocator + Phase 2's per-Proc page tables build out from this baseline.

### W^X enforcement is build-time + runtime, not formally verified

`_Static_assert` catches PTE constructor regressions; `pte_violates_wxe()` is a runtime check available to fault handlers. Neither is a formal proof. A `mmu.tla` spec could prove that the page-table-walker / fault-handler interaction never violates I-12; that's post-v1.0 unless a real bug surfaces.

### TTBR0 identity stays active post-KASLR

After P1-C-extras Part B, TTBR1 holds the kernel high-half mapping but TTBR0 keeps the low 4 GiB identity map. Kernel data accesses to absolute PAs (the saved DTB pointer, PL011 MMIO, future MMIO regions) translate through TTBR0. Phase 2 will retire TTBR0 when user territories start to live there; until then, the kernel can read low PAs by absolute addressing. The trade-off is a wider attack surface (TTBR0 has the kernel's full RAM mapped); the mitigation is that nothing in user code runs during this period.

### L3 tables are shared between TTBR0 and TTBR1

The same `l3_kernel` page-grain region (two contiguous L3 tables since P5-kernel-l3-4mib) is referenced by L2 entries in both TTBR0 (via `l2_ttbr0[gib][idx]` and the next) and TTBR1 (via `l2_ttbr1[l2_idx]` and the next). This works because the L3 PTEs map specific physical addresses (PAs) — the two paths reach the same memory through different VAs. Saves 8 KiB of BSS and ensures both translation roots see identical kernel-image semantics.

### Kernel image must fit in the 4 MiB L3 mapping

The L3 mapping covers exactly `[kernel_2mib_pa, kernel_2mib_pa + 4 MiB)` where `kernel_2mib_pa = pa_kernel_start & ~(BLOCK_SIZE_L2 - 1)`. With `KERNEL_LOAD_PA = 0x40080000` on QEMU virt, that's `[0x40000000, 0x40400000)`. The 512 KiB firmware reservation at `[0x40000000, 0x40080000)` is mapped invalid (R6-B F114); the kernel image follows. Any kernel section past `0x40400000` is **not mapped at all** — any post-MMU access (read or write, via either the high-VA or direct-map alias) takes an unhandled translation fault.

Before P5-kernel-l3-4mib the mapping was a single 2 MiB L3 block ending at `0x40200000`, and history shows that ceiling was a real cliff. The image-only assert in `kernel.ld` was once conservative-low: it checked `image_size < 2 MiB` without accounting for the firmware offset. A P4-Ic7 commit briefly fell off the cliff by pushing image_size from 1440 → 1572 KiB while the firmware reservation was still 512 KiB — total 2080 KiB > 2 MiB — and the kernel silently faulted on post-MMU accesses past the L3-mapped region. R12-bss-2mib (`docs/phase4-status.md` deferred-audit row) tightened the assert to express the actual constraint: `image_size + (KERNEL_LOAD_PA & (BLOCK_SIZE_L2 - 1)) < 2 MiB`, capping image_size at 1.5 MiB under the 512 KiB firmware offset.

The 2 MiB era ended at P5-kernel-l3-4mib: the UBSan-instrumented image grew past the 1.5 MiB usable ceiling, so `build_page_tables` was extended to map a second 2 MiB block adjacent to the first (the option-(b) fix that earlier revisions of this section anticipated). `l3_kernel` and `l3_directmap_kernel` are now each `[ENTRIES_PER_TABLE * 2]`, and the `kernel.ld` assert checks `image_size + (KERNEL_LOAD_PA & (0x400000 - 1)) < 0x400000` — capping image_size at **3.5 MiB** with the 512 KiB firmware offset. The current UBSan-instrumented image is ~1.51 MiB, leaving ~2 MiB of headroom under the new cap. If a future image outgrows 3.5 MiB, the remaining options are (a) reduce the firmware reservation (move `KERNEL_LOAD_PA` closer to `0x40000000`) or (c) extend the L3 region again with more contiguous tables.

A companion `kernel.ld` assert pins that the 4 MiB region lies **within a single 1 GiB L2 table** — `((KERNEL_LOAD_PA & ~(2 MiB - 1)) & (1 GiB - 1)) + 4 MiB <= 1 GiB`. `build_page_tables` wires the region's two 2 MiB halves as L2 entries `idx` and `idx + 1` of one per-GiB table (`l2_ttbr0[gib]`, and the single `l2_directmap_kernel`); were `kernel_2mib_pa` in the last 2 MiB of a GiB, `idx + 1` would overrun the 512-entry table. The TTBR1 high-half path guards the same condition at *runtime* (its L2 index derives from the KASLR slide, not a link-time constant); the fixed-PA TTBR0 + direct-map paths are pinned at *link time* by this assert. Added at the P5-kernel-l3-4mib MMU/KASLR audit close (finding F1).

### MMIO vmalloc pool sized for the CPU max

This is a *separate* region from the kernel-image L3 above. Runtime MMIO mappings — GIC distributor + per-CPU redistributors, the PL011 UART, the virtio-pci ECAM window, virtio-mmio transports — live in a page-grain vmalloc range at `VMALLOC_BASE` (`0xFFFF_8000_0000_0000`), populated by the `mmu_map_mmio` bump allocator. Since the P5-mmio-pool-8cpu fix (2026-05-31) the range is backed by **two contiguous L3 tables** (`l3_vmalloc` + `l3_vmalloc2`, 1024 entries = 4 MiB) instead of one (512 entries = 2 MiB).

The driver was the per-CPU GIC redistributor mapping: `gic_init` maps `cpu_count × GICR_FRAME_STRIDE` (32 pages/CPU) as one block, so the redist alone is 128 pages at `-smp 4` but **256 pages at the `DTB_MAX_CPUS = 8` design max** — which, with the 1 MiB ECAM (256 pages) + GIC dist (16) + virtio/uart, overran a single 512-entry table and made `-smp 8` extinct at early boot (`mmu_map_mmio: l3_vmalloc exhausted`, 20/20 deterministic) even though the static secondary/exception stacks were *already* sized for 8. The kernel `_Static_assert`ed `DTB_MAX_CPUS == 8` but could not actually boot 8 CPUs — surfaced by the 2026-05-31 SMP adverse-condition soundness sweep (finding "F-C").

A `_Static_assert` now ties the pool capacity to the CPU max — `VMALLOC_MMIO_ENTRIES >= 256 + 16 + 32 * DTB_MAX_CPUS + 64` — so a future CPU-max bump that outgrows the pool fails the **build**, not the boot. `mmu_map_mmio` places each page by its global index across the two tables (`idx < 512` → `l3_vmalloc`, else `l3_vmalloc2[idx % 512]`); a single mapping may straddle the 512-entry boundary, and the returned VA stays contiguous because `l2_vmalloc[0]`/`[1]` map adjacent 2 MiB windows. To raise the cap further, add another `l3_vmalloc` table + `l2_vmalloc[2]` wiring.

### Direct-map demote BBM runs IRQ-masked (#806 root cause)

`directmap_walk_to_l2` / `directmap_walk_to_l3` demote a 1 GiB / 2 MiB direct-map block to a finer table when a kstack guard page must be made no-access (`mmu_set_no_access_range`, called per `thread_create`). The demote is a **break-before-make**: write the entry to 0, `tlbi vmalle1is` + `dsb ish`, then write the new table descriptor. Between break and make, **that entire 1 GiB / 2 MiB of the direct map is unmapped**, and the window spans a TLBI + DSB (microseconds, waiting for inner-shareable completion).

This was the **year-long "rfork_stress kernel stack overflow" root cause (#806).** The window was **not IRQ-masked**: a timer IRQ taken inside it runs a handler that dereferences `current_thread()` — a `struct Thread` reached via its **direct-map KVA** — and when that thread's PA falls in the block being demoted, the read takes a translation fault. The fault handler (`arch_fault_handle` → `stack_guard_overflow_msg`) re-dereferences the same now-wild `current_thread()->magic`, recursing one exception frame per fault down the boot stack until it crosses the guard — which is why the symptom *presented* as a boot-stack overflow. The honest dump (captured once the #806 re-entrancy guard was in place, via a KASLR-seed-pinned repro) showed `ESR` DFSC=0x06 (an L2 translation fault, a read) on a `0xffff0000_…` direct-map address, with the backtrace `thread_create_internal → mmu_set_no_access_range → exception_irq_curr_el → exception_sync_curr_el → arch_fault_handle`.

**Fix:** mask IRQs (`spin_lock_irqsave(NULL)` — the bare-DAIF idiom) across both BBM windows so no handler **on the demoting CPU** can observe the transient unmap. This closes the **same-CPU** IRQ-in-window race — the proven #806 root cause. The rarity (~1/24 KASLR seeds, and probabilistic even on a pinned seed) came from needing *both* the layout coincidence (current_thread's PA in the demoted block) *and* the timing coincidence (IRQ inside the µs window).

**Known residual — cross-CPU concurrent demote (tracked Phase-5+; reachable today, NOT future-only).** A *peer* CPU that dereferences any direct-map PA inside the 1 GiB / 2 MiB block while this CPU is mid-demote takes the identical translation fault, and a per-CPU IRQ mask cannot prevent it. The audit (F1) corrected an earlier "demote is single-CPU at v1.0" claim: this hazard is reachable on the **default `-smp 4` + work-stealing** harness — plain `rfork` on two CPUs produces concurrent `directmap_walk_*` demotes, and any peer's running-kstack / `current_thread` / `struct page` deref into the demoted GiB faults. It has not surfaced as a *second* recurring crash only because the µs window × the exact peer-deref-into-the-being-demoted-block coincidence is rare, and because demotes are one-way (block→table) so the demotable working set is exhausted early in a boot — not because demote is single-CPU. The IRQ-mask fix is **orthogonal** (it neither closes nor worsens this). The **durable fix** is a *non-BBM, OA-preserving* refinement (audit F2): this demote reproduces the same output address + attributes, so the `= 0` break is architecturally unnecessary — ARM ARM B2.7.1's break-before-make requirement targets size/attribute *changes*, and a pure block→table refinement of identical coverage can install the table descriptor without the intervening invalidation. That eliminates the transient unmap for **both** the IRQ and the cross-CPU races, needs no lock and no quiesce, and is the recommended Phase-5+ resolution (it is implementation-subtle under the strict B2.7.1 reading, so it warrants its own audit). Note a global `mmu_lock` alone does **not** fix the cross-CPU race — the faulting peer holds no lock. See `25-fault-dispatcher.md` footgun #6 and the `mmu.c::directmap_walk_to_l2` comment.

---

## See also

- `docs/reference/00-overview.md` — system-wide layer cake.
- `docs/reference/01-boot.md` — boot path + integration with `mmu_enable`.
- `docs/reference/04-extinction.md` — kernel ELE infrastructure used by future fault handlers.
- `docs/ARCHITECTURE.md §6` — memory management design intent.
- `docs/ARCHITECTURE.md §24` — hardening policies including W^X.
- `docs/ARCHITECTURE.md §28` — enumerated invariants (I-12 specifically).
- ARM Architecture Reference Manual ARMv8 — section B2.5 (MMU enable sequencing).
