# 05 — KASLR (as-built reference)

The kernel's address-space layout randomization. KASLR randomizes the runtime virtual address of the kernel image at every boot, defending against speculative-execution side channels and similar attacks that leak kernel pointers. P1-C-extras Part B deliverable; load-bearing for invariant **I-16**.

Scope: `arch/arm64/kaslr.{h,c}`, the toolchain flip to `-fpie -fdirect-access-external-data -mcmodel=tiny` in `cmake/Toolchain-aarch64-thylacine.cmake`, the link change to `-Wl,-pie -Wl,-z,text -Wl,-z,norelro -Wl,-z,nopack-relative-relocs -Wl,--no-dynamic-linker` and the high-VA link in `arch/arm64/kernel.ld`, the slide-aware `mmu_enable(u64 slide)` and TTBR1 high-half mapping in `arch/arm64/mmu.c`, the `kaslr_init` call + long-branch in `arch/arm64/start.S`, and the banner integration in `kernel/main.c`. Also see `docs/reference/01-boot.md` (entry sequence) and `docs/reference/03-mmu.md` (page tables).

Reference: `ARCHITECTURE.md §5.3` (KASLR design intent), `§6.2` (kernel VA layout), `§24` (hardening), `§28` invariant **I-16**.

---

## Purpose

The kernel image is linked at a fixed high VA (`KASLR_LINK_VA = 0xFFFFA00000080000`) inside TTBR1's "kernel modules + KASLR" region (0xFFFF_A000_*). At boot, the kernel chooses a random page-block-aligned (2 MiB) offset from the best available entropy source, applies any `R_AARCH64_RELATIVE` relocations in the embedded `.rela.dyn` section, builds a TTBR1 page-table mapping at `KASLR_LINK_VA + offset`, and long-branches into the runtime VA. From then on, kernel code executes through TTBR1; data accesses to PAs (DTB, MMIO) continue to go through TTBR0's identity map.

The runtime kernel base differs at every boot. An attacker that can leak a single kernel pointer learns the slide (and therefore the kernel base) but only for that boot — the next boot's slide is independent.

---

## Public API

`arch/arm64/kaslr.h`:

```c
// Choose a slide, apply relocations. Returns slide in bytes.
u64 kaslr_init(void);

// Diagnostic accessors used by boot_main() for the banner.
u64 kaslr_get_offset(void);
kaslr_seed_source_t kaslr_get_seed_source(void);
const char *kaslr_seed_source_str(kaslr_seed_source_t s);

// Translate a load-PA address into its post-KASLR high VA. Used by
// the boot stub for the long-branch into TTBR1.
u64 kaslr_high_va_addr(void *pa);

// Runtime kernel high-VA base (= KASLR_LINK_VA + offset). For the
// banner's "kernel base: 0x..." line.
u64 kaslr_kernel_high_base(void);
```

`KASLR_LINK_VA = 0xFFFFA00000080000` is hardcoded in `kaslr.h` and asserted equal to `KERNEL_LINK_VA` in `kernel.ld`. PIE-mode PC-relative addressing means C code can't read absolute symbols, so the constant lives on both sides with a linker `ASSERT()`.

---

## Implementation

### Entropy chain

`kaslr_init` tries seed sources in priority order:

1. **DTB `/chosen/kaslr-seed`** (8 bytes, two FDT cells). UEFI populates this on bare metal (Pi 5 post-v1.0); modern QEMU virt also populates it. Highest-priority — assumed hardware-derived high entropy.
2. **DTB `/chosen/rng-seed`** (typically 32 bytes / 8 cells = 256 bits). QEMU virt always populates it. We XOR-fold the cells into a u64, alternating between high and low halves of the accumulator so adjacent cells don't trivially cancel.
3. **`cntpct_el0` hardware counter**. Generic-timer physical counter, readable at EL1 (CNTHCTL_EL2.EL1PCTEN was set during the EL2→EL1 drop or the EL1-direct path). Counter advances at 24 MHz on QEMU virt / 19.2 MHz on Pi 5; a few ms of boot is millions of cycles, which gives some entropy from boot-time variance — but well below kaslr-seed / rng-seed.

The chosen value passes through a SipHash-style avalanche mix (`mix64`) — XOR + shift + multiply with two odd 64-bit constants — to spread bits before masking. This protects against weak low-bit entropy in the cntpct path.

### Offset choice

```c
#define KASLR_OFFSET_MASK   0x3FFE00000ull  // bits 33..21 set
#define KASLR_ALIGN_BITS    21              // 2 MiB alignment

u64 offset = mixed & KASLR_OFFSET_MASK;
if (offset == 0) offset = 1ull << KASLR_ALIGN_BITS;   // minimum 2 MiB
```

The mask preserves bits 33..21 = **13 bits of entropy = 8192 distinct 2 MiB-aligned offsets** in `[0, 16 GiB)`. We enforce a non-zero minimum (one 2 MiB block) so KASLR never trivially returns slide=0.

The 16 GiB upper bound is conservative compared to ARCH §6.2's allowable 32 TiB range. Two reasons:
- The TTBR1 page-table builder uses a single L0 entry (kernel is in one 512 GiB L0 slot). Capping well below 512 GiB keeps the table code linear.
- 13 bits is comparable to Linux ARM64 KASLR's effective entropy on most platforms (also bounded by L1 walk constraints).

A future bump to 18 bits (256 K offsets, ~512 GiB range) is mechanical — change the mask + verify the L0 carry math in `mmu.c`.

### .rela.dyn relocation walker

```c
struct elf64_rela { u64 r_offset; u64 r_info; s64 r_addend; };
#define R_AARCH64_RELATIVE 1027u

static void apply_relocations(u64 slide) {
    u64 pa_kernel_start = (u64)(uintptr_t)_kernel_start;   // PIC: PC-rel = PA
    s64 link_to_pa = (s64)pa_kernel_start - (s64)KASLR_LINK_VA;

    const struct elf64_rela *rel = (const struct elf64_rela *)(uintptr_t)_rela_start;
    const struct elf64_rela *end = (const struct elf64_rela *)(uintptr_t)_rela_end;

    while (rel < end) {
        u32 type = (u32)(rel->r_info & 0xFFFFFFFFu);
        if (type == R_AARCH64_RELATIVE) {
            u64 target_pa = (u64)((s64)rel->r_offset + link_to_pa);
            u64 *target = (u64 *)(uintptr_t)target_pa;
            *target = (u64)(rel->r_addend + (s64)slide);
        }
        rel++;
    }
}
```

Each `.rela.dyn` entry's `r_offset` is a **link-time VA** (in 0xFFFFA00*); we convert to PA via `link_to_pa` because the boot stub runs with MMU off. The patched value is `addend + slide` — the runtime high VA the entry should point to.

**On our minimal kernel, `.rela.dyn` is empty (0 entries)** because:
- `-fpie` makes all references PC-relative or via GOT.
- `-fdirect-access-external-data` forces `extern` data references to use direct adrp+add (no GOT entries).
- `-mcmodel=tiny` constrains relocations to the small-code-model 4 GiB range.

The relocator is therefore a no-op walker today. The infrastructure is in place for future code that introduces absolute pointer references — e.g., function-pointer tables in static data — each such reference will land in `.rela.dyn` automatically and be patched here.

### TTBR1 mapping

`mmu.c`'s `build_page_tables(slide)` constructs both:

- TTBR0 identity for low 4 GiB (unchanged from P1-C).
- TTBR1 high-half mapping at `KASLR_LINK_VA + slide`.

The TTBR1 walk uses three new BSS tables (`l0_ttbr1`, `l1_ttbr1`, `l2_ttbr1`) plus the **shared** `l3_kernel` page-grain table (the same L3 used by TTBR0's identity-map override for the kernel-image 2 MiB block). Because the kernel image stays at the same PA across boots and only the L0/L1/L2 indices into it change, sharing the L3 is safe and saves 4 KiB.

L0/L1/L2 indices for the high VA:

```c
u64 kernel_high_va = KASLR_LINK_VA + slide;
u64 high_va_2mib   = kernel_high_va & ~(BLOCK_SIZE_L2 - 1);

u32 l0_idx = (high_va_2mib >> 39) & 0x1FF;   // always 0x140 for KASLR slot
u32 l1_idx = (high_va_2mib >> 30) & 0x1FF;   // varies with slide
u32 l2_idx = (high_va_2mib >> 21) & 0x1FF;   // varies with slide

l0_ttbr1[l0_idx] = make_table_pte(l1_ttbr1);
l1_ttbr1[l1_idx] = make_table_pte(l2_ttbr1);
l2_ttbr1[l2_idx] = make_table_pte(l3_kernel);   // shared L3
```

Page-table footprint after KASLR:
- TTBR0: 1 L0 + 1 L1 + 4 L2 = 24 KiB.
- TTBR1: 1 L0 + 1 L1 + 1 L2 = 12 KiB.
- Shared: 1 L3 = 4 KiB.
- **Total: 40 KiB BSS-allocated.**

### Long-branch into high VA

After `mmu_enable(slide)` returns, PC is still at load PA via TTBR0's identity map. The boot stub computes the high VA of `boot_main` and branches:

```asm
adrp    x0, boot_main           // PA of boot_main (PIC: PC-rel)
add     x0, x0, :lo12:boot_main
bl      kaslr_high_va_addr      // x0 = high VA = (pa - pa_kernel_start) + KASLR_LINK_VA + offset
br      x0                      // long-branch into TTBR1
```

The `br` instruction sets PC to the high VA. The CPU's MMU determines the translation root from PC bit 55 (high VAs have bit 55 = 1 → TTBR1). Instruction fetch from PC translates via TTBR1 → PA → execute.

After the branch, all PC-relative addressing in C code (`adrp+add` for global/static references) gives **high VAs**, since PC = high VA. Data references to absolute PAs (the saved DTB pointer, MMIO regions) continue to go through TTBR0's identity map.

The boot stack remains at PA initially (SP was set to `_boot_stack_top` PA before MMU enable). Stack accesses use TTBR0 because SP is a low VA. Phase 2 will introduce per-thread stacks at high VAs once thread management lands.

---

## Data structures

### `kaslr_seed_source_t`

```c
typedef enum {
    KASLR_SEED_NONE             = 0,    // pre-init / no entropy at all
    KASLR_SEED_DTB_KASLR_SEED   = 1,    // /chosen/kaslr-seed
    KASLR_SEED_DTB_RNG_SEED     = 2,    // /chosen/rng-seed
    KASLR_SEED_CNTPCT           = 3,    // ARM generic counter fallback
} kaslr_seed_source_t;
```

Surfaced in the boot banner via `kaslr_seed_source_str()`. A development build that shows `cntpct (low-entropy fallback)` is an immediate red flag — it means neither DTB seed was published.

### Static state

```c
static u64 g_kaslr_offset;
static kaslr_seed_source_t g_kaslr_seed_source;
```

Both are BSS (zero-cleared at boot). `kaslr_init()` sets them; `kaslr_get_offset()` and `kaslr_get_seed_source()` read them.

---

## Spec cross-reference

No formal spec at P1-C-extras. A future `kaslr.tla` could prove:
- The slide is always 2 MiB-aligned.
- The slide is bounded such that the L0 walk never crosses a slot boundary.
- The .rela.dyn walker terminates and applies each entry exactly once.

These are structurally simple (no concurrency at boot) and enforced by the code today; the TLA+ model is post-v1.0 hardening.

---

## Tests

P1-C-extras Part B integration test: `tools/test.sh` boots and verifies the boot banner. Each boot prints a different `kernel base` and `KASLR offset`. `tools/test.sh` doesn't gate on the offset value — only on the `Thylacine boot OK` line.

Cross-boot verification (informal, P1-I will formalise):

```bash
for i in $(seq 1 10); do
  tools/test.sh 2>&1 | grep "kernel base:" | head -1
done
```

Expected: 10 distinct offsets ranging across [0x200000, 0x3FFE00000) (2 MiB to 16 GiB - 2 MiB), 2 MiB-aligned.

---

## Error paths

| Condition | Behavior |
|---|---|
| All seed sources return 0 | `g_kaslr_seed_source = KASLR_SEED_NONE`; `mixed = 0` after mix64; offset becomes the 2 MiB minimum (slide == 1 << 21). Banner shows `seed: none`. **Boot continues** — KASLR is degenerate but the kernel still runs at a non-trivial high VA. |
| `.rela.dyn` malformed | Walker reads `r_info` field as native-endian; if the entry's type isn't `R_AARCH64_RELATIVE`, the walker silently skips. With our build flags, no other types should appear. |
| TTBR1 page-table walk hits an unmapped region | Synchronous data abort. Pre-P1-F this wedges QEMU; P1-F's fault handler will route to `extinction()`. |
| The kernel crosses the 2 MiB block during link-time growth | Linker `ASSERT()` `_kernel_end - _kernel_start < 0x200000` fails the build. |

---

## Performance characteristics

| Metric | Measured | Notes |
|---|---|---|
| Kernel ELF size (debug) | ~117 KB | +18 KB from P1-C-extras Part A; PIE adds dynamic-section family + extra GOT + alignment slack. |
| Kernel flat binary | 16 KB | +8 KB from Part A. The `.dynamic` / `.dynsym` / `.gnu.hash` / `.hash` / `.got` family adds ~3 KB; alignment to PAGE_SIZE adds ~5 KB. |
| Page-table footprint | 40 KiB | +12 KiB from P1-C (which had 28 KiB). Three new tables for TTBR1 (`l0_ttbr1`, `l1_ttbr1`, `l2_ttbr1`). |
| `kaslr_init` total cost | ~5 µs | Dominated by `dtb_get_chosen_*` walks; mix64 + offset compute is sub-µs. |
| `mmu_enable` total cost | ~0.05 ms | Unchanged from P1-C. |
| Boot to UART banner | ~50 ms (informal) | Unchanged from P1-C. KASLR adds < 0.01 ms. |

---

## Status

**Implemented at P1-C-extras Part B**:

- `-fpie -fdirect-access-external-data -mcmodel=tiny` compile flags (PIE, no GOT for extern data).
- `-Wl,-pie -Wl,-z,text -Wl,-z,norelro -Wl,-z,nopack-relative-relocs -Wl,--no-dynamic-linker` link flags (static-PIE).
- Linker script links at `KERNEL_LINK_VA = 0xFFFFA00000080000` with `AT(KERNEL_LOAD_PA = 0x40080000)`.
- `.rela.dyn` section retained in loaded image (currently empty).
- `arch/arm64/kaslr.{h,c}` (~150 LOC) — entropy chain, mix function, offset choice, R_AARCH64_RELATIVE walker, slide accessors.
- `arch/arm64/mmu.c` extended with TTBR1 mapping at `KASLR_LINK_VA + slide` using the shared `l3_kernel` table. `mmu_enable(u64 slide)` signature change.
- `arch/arm64/start.S` calls `kaslr_init()` after BSS clear, passes slide to `mmu_enable()`, then long-branches to high VA `boot_main` via `kaslr_high_va_addr`.
- `lib/dtb.c` split `dtb_get_chosen_kaslr_seed` (kaslr-seed only) from `dtb_get_chosen_rng_seed` (rng-seed only); `kaslr.c` tries them in priority order.
- Boot banner shows `kernel base: 0x...`, `KASLR offset 0x...`, `seed: <source>`.

**Verified**:

- `tools/test.sh` PASS.
- 10 consecutive boots produce 10 distinct offsets (manual).
- Disassembly confirms `bl kaslr_init`, `bl mmu_enable`, `adrp boot_main`, `bl kaslr_high_va_addr`, `br x0` sequence.
- Linker `ASSERT(_kernel_start == KERNEL_LINK_VA, ...)` enforces the C / linker-script constants agree.

**Not yet implemented**:

- Per-thread KASLR (Phase 2, with thread stacks at high VAs).
- TTBR0 retire (Phase 2, when user mappings move into TTBR0). Today TTBR0 stays identity-mapped for low PA access.
- `mmu.tla` formal spec (post-v1.0 unless a real bug surfaces).

**Landed**: P1-C-extras Part B at commit `74fd391`.

---

## Caveats

### TTBR0 stays active

After KASLR, TTBR0 still holds the low-4-GiB identity map. Kernel data accesses to absolute PAs (e.g., the saved DTB pointer, PL011 MMIO) translate through TTBR0. This is by design at P1-C-extras — Phase 2 will retire TTBR0 when user territories start to live there. Until then, the kernel can read PAs by absolute addressing.

### `.rela.dyn` is empty today

`-fdirect-access-external-data` plus `-mcmodel=tiny` plus PC-relative branches eliminate every R_AARCH64_RELATIVE entry our minimal kernel would have generated. The relocator is therefore an empty-loop on the current build. **This is fine** — the infrastructure is in place; if future code introduces an absolute pointer reference (e.g., a static initializer that takes the address of another global), the linker will emit a R_AARCH64_RELATIVE entry and the relocator will pick it up automatically.

### Single-CPU only

`mmu_enable` runs on the boot CPU. SMP secondary CPUs (Phase 2) need their own MMU enable — they'll start with the same TTBR0 / TTBR1 / MAIR / TCR / SCTLR values via per-CPU init. Each CPU has its own translation registers; the page tables are shared.

### Constant duplication: `KASLR_LINK_VA`

`KASLR_LINK_VA = 0xFFFFA00000080000` lives in two places:
- `arch/arm64/kernel.ld` as `KERNEL_LINK_VA` (controls where the linker places `_kernel_start`).
- `arch/arm64/kaslr.h` as `KASLR_LINK_VA` (used by C code for slide math).

A linker `ASSERT()` enforces equality. Both must change together.

### KASLR offset entropy is 13 bits

8192 distinct offsets is reasonable for a v1.0 baseline but lower than e.g. desktop Linux ARM64 (which gets ~17-18 bits depending on platform). Future hardening pass can bump the mask to bits 38..21 (18 bits, ~256 K offsets, ~512 GiB range) — the only constraint is keeping the L0 index fixed (true up to ~512 GiB - text_offset ≈ 512 GiB).

---

## See also

- `docs/reference/00-overview.md` — system-wide layer cake.
- `docs/reference/01-boot.md` — boot path; updated for KASLR slide and long-branch.
- `docs/reference/03-mmu.md` — MMU + W^X; TTBR1 high-half mapping documented there.
- `docs/reference/04-extinction.md` — kernel ELE infra (used by future fault handlers including the stack-overflow guard).
- `docs/ARCHITECTURE.md §5.3` — KASLR design intent.
- `docs/ARCHITECTURE.md §6.2` — kernel VA layout.
- `docs/ARCHITECTURE.md §24` — hardening policies.
- `docs/ARCHITECTURE.md §28` — invariant I-16.
- ARM Architecture Reference Manual ARMv8 — section D5 (VMSAv8-64 page tables).
- Linux kernel `arch/arm64/kernel/head.S` + `arch/arm64/kernel/pi/` — reference KASLR boot path.
