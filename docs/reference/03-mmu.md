# 03 — MMU + W^X (as-built reference)

The kernel's MMU bring-up. Identity-mapped low 4 GiB with per-section permissions for the kernel image, Device-nGnRnE for MMIO, and the W^X invariant (I-12) enforced at PTE bit level. P1-C deliverable; KASLR + TTBR1 high-half + boot stack guard are deferred to P1-C-extras.

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
// Build identity tables, program MAIR/TCR/TTBR/SCTLR, turn on the MMU.
// Returns with caches enabled and per-section permissions in force.
void mmu_enable(void);

// Inspect a PTE for W^X compliance. Returns true iff writable AND
// executable-at-EL1 — the forbidden combination. Used by the audit
// path; future fault-handler can call it on a fault PTE to print
// "extinction: PTE violates W^X".
bool pte_violates_wxe(u64 pte);
```

The header also exposes the PTE bit constants (`PTE_VALID`, `PTE_AF`, `PTE_AP_*`, `PTE_PXN`, `PTE_UXN`, etc.), the MAIR attribute indices (`MAIR_IDX_DEVICE`, `MAIR_IDX_NORMAL_WB`, ...), and the four canonical W^X-safe constructors (`PTE_KERN_TEXT`, `PTE_KERN_RO`, `PTE_KERN_RW`, `PTE_KERN_RW_BLOCK`, `PTE_DEVICE_RW_BLOCK`).

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

P1-C maps the low 4 GiB:

- 1× **L0 table** (only entry 0 used, but the whole 4 KiB allocated; `L0[0] → l1_table`).
- 1× **L1 table** (entries 0..3 used, one per GiB; `L1[i] → l2_table[i]`).
- 4× **L2 tables** (one per GiB; 512 entries × 2 MiB = 1 GiB each).
  - Default: 2 MiB Normal-WB RW blocks.
  - Override: the 2 MiB block containing `0x09000000` (PL011) → Device-nGnRnE block.
  - Override: the 2 MiB block containing the kernel image → table descriptor pointing at `l3_kernel`.
- 1× **L3 table** for the kernel-image 2 MiB region (page-granular permissions).

Total page-table footprint: **7 × 4 KiB = 28 KiB**, BSS-allocated and zeroed by `start.S`.

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
    "msr ttbr0_el1, %2\n"         // user/identity table base
    "msr ttbr1_el1, xzr\n"        // kernel-high table (empty at P1-C)
    "isb\n"                        // sync before SCTLR change
    "mrs x9, sctlr_el1\n"
    "orr x9, x9, %3\n"             // set M | C | I
    "msr sctlr_el1, x9\n"
    "isb\n"                        // sync after MMU enable
    :: "r" ((u64)MAIR_VALUE),
       "r" ((u64)TCR_VALUE),
       "r" ((u64)(uintptr_t)l0_table),
       "r" ((u64)(SCTLR_M | SCTLR_C | SCTLR_I))
    : "x9", "memory"
);
```

The kernel runs through this transition seamlessly because TTBR0 identity-maps the addresses where the kernel currently executes. Post-MMU, every load and store goes through the page tables — but lookups resolve to the same physical addresses, so PCs and SP remain valid.

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

**Deferred to P1-C-extras** (next sub-chunk):

- KASLR (kernel image relocation into TTBR1 high half; PIE relocations; randomized offset from `dtb_get_chosen_kaslr_seed()`).
- TTBR1 high-half mapping populated.
- Boot stack guard page (non-present mapping below `_boot_stack_bottom`; catches stack overflow as a fault).
- EL2→EL1 drop diagnostic (Pi 5 concern; not load-bearing for QEMU virt).

**Landed**: P1-C at commit `(pending hash-fixup)`.

---

## Caveats

### Caches enabled before testing them

`SCTLR.C=1` and `SCTLR.I=1` are set in the same write that enables the MMU. If the page tables are misconfigured, we lose visibility immediately (caches mask the symptom). Mitigation: test with `SCTLR.C=0` first if MMU enable goes silent in the future. We don't bother at P1-C because the implementation is cleanly traced from ARM ARM B2.5 and known-good Linux/seL4/Fuchsia patterns.

### Page tables are static (BSS-allocated)

At P1-C the page tables are fixed-size BSS arrays. This is fine for identity-mapping the low 4 GiB, but won't scale to per-process address spaces (Phase 2). P1-D's physical allocator + Phase 2's per-Proc page tables build out from this baseline.

### W^X enforcement is build-time + runtime, not formally verified

`_Static_assert` catches PTE constructor regressions; `pte_violates_wxe()` is a runtime check available to fault handlers. Neither is a formal proof. A `mmu.tla` spec could prove that the page-table-walker / fault-handler interaction never violates I-12; that's post-v1.0 unless a real bug surfaces.

### Identity mapping is transitional

The eventual layout is kernel in TTBR1 high half (`0xFFFF_*`), userspace in TTBR0 low half. P1-C keeps kernel in TTBR0 because we haven't done KASLR yet (which moves kernel to high half). P1-C-extras handles the transition.

---

## See also

- `docs/reference/00-overview.md` — system-wide layer cake.
- `docs/reference/01-boot.md` — boot path + integration with `mmu_enable`.
- `docs/reference/04-extinction.md` — kernel ELE infrastructure used by future fault handlers.
- `docs/ARCHITECTURE.md §6` — memory management design intent.
- `docs/ARCHITECTURE.md §24` — hardening policies including W^X.
- `docs/ARCHITECTURE.md §28` — enumerated invariants (I-12 specifically).
- ARM Architecture Reference Manual ARMv8 — section B2.5 (MMU enable sequencing).
