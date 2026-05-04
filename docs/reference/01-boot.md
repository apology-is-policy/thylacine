# 01 — Boot path (as-built reference)

The kernel's boot path: from QEMU `-kernel` direct entry through `_start` (assembly), `boot_main()` (C), to a halt loop. This document describes what exists in the tree as of **P1-A**; subsequent sub-chunks (P1-B DTB parser, P1-C MMU + KASLR, etc.) extend it.

Scope: `arch/arm64/start.S` (incl. Linux ARM64 image header at offset 0; added at P1-B), `arch/arm64/kernel.ld` (incl. `_image_size` linker-resolved symbol; added at P1-B), `arch/arm64/uart.c` + `arch/arm64/uart.h` (runtime-configurable PL011 base; updated at P1-B), `kernel/main.c` (DTB integration; updated at P1-B), `kernel/include/thylacine/types.h`. Also see `docs/reference/02-dtb.md` for the FDT parser introduced at P1-B.

Reference: `ARCHITECTURE.md §5` (boot sequence design intent), `TOOLING.md §10` (boot banner contract).

---

## Purpose

Bring the kernel to a state where it has run its initial setup and printed a recognizable signal — the boot banner — that the host-side tooling can match against. At P1-A there is no scheduler, no MMU, no devices beyond the polled UART; the kernel halts in a `wfi` loop after the banner. This is the foundation every later phase composes on top of.

The boot banner is **kernel ABI with the development tooling** (`TOOLING.md §10`). The strings `Thylacine boot OK` (success) and `PANIC:` (panic prefix; not yet emitted at P1-A — added at P1-C panic handler) are how `tools/run-vm.sh` and the agentic loop detect boot status. Their format does not change without coordinated updates to `tools/run-vm.sh`, `tools/agent-protocol.md`, `CLAUDE.md`, and `TOOLING.md`.

---

## Public API

The boot path has no caller-facing API surface in the C sense; the entry point is `_start` (machine-code address `0x40080000`, the ELF entry symbol). The only externally-meaningful interfaces are:

| Surface | Type | Where |
|---|---|---|
| ELF entry symbol `_start` | Linker entry point | `arch/arm64/start.S:21` (the `.text._start` section, kept first by `kernel.ld`) |
| Boot banner output to UART | Stdout text contract | Emitted by `kernel/main.c:boot_main()` |
| `_hang` halt loop | Entry point for clean halts | `arch/arm64/start.S:64` |
| `_saved_dtb_ptr` | BSS variable holding DTB physical address | Populated by `_start`; read by `boot_main()` to print the banner; consumed by the DTB parser at P1-B |
| `uart_putc / uart_puts / uart_puthex64 / uart_putdec` | Polled UART output | `arch/arm64/uart.h:18-35`. Replaced by `/dev/cons` (kernel `Dev`) at Phase 3 |

---

## Implementation

### Entry sequence (`arch/arm64/start.S`)

QEMU's `-kernel` direct loader hands control to the kernel image. **At P1-B** (after adding the Linux ARM64 image header), QEMU recognizes the kernel as a Linux Image (`is_linux = 1`) and:

- **EL**: EL1h on QEMU virt; EL2h on Pi 5 bare metal (and most modern firmware). The kernel handles both: at EL1 we proceed directly; at EL2 we drop to EL1h via the canonical SPSR_EL2 / ELR_EL2 / `eret` sequence (P1-C-extras). EL3 / EL0 entry is unsupported and halts silently (no UART available without a stack + DTB-discovered base — diagnostics surface at the boot banner via `_entered_at_el2`).
- **x0**: DTB physical address. With the Linux ARM64 image header in place from P1-B, QEMU loads the DTB at `loader_start + min(ram_size/2, 128 MiB)` — for our 2 GiB RAM, that's `0x48000000` — and passes the address in `x0`. (At P1-A, before the image header was added, x0 was 0 and the DTB was never loaded — see `docs/reference/02-dtb.md` "Linux ARM64 image header" for the investigation that surfaced this.)
- **x1..x3**: 0 (reserved by Linux protocol).
- **MMU off, caches off**, interrupts masked. Single CPU executing.

The Linux ARM64 image header (64 bytes) lives at offset 0 of the binary. `code0` at offset 0 is `b _real_start` — a branch over the header into the actual boot code. The header carries `text_offset = 0x80000`, `image_size = _kernel_end - _kernel_start` (linker-resolved via `_image_size` symbol in `kernel.ld`), and `flags = 0xa` (little-endian + 4 KiB granule + placement-anywhere). `magic` at offset 0x38 is `0x644d5241` ("ARM\x64") — this is what QEMU detects.

After the branch, `_real_start` performs, in order:

1. **EL check + drop**. Reads `CurrentEL[3:2]`. If EL1, sets `x20 = 0` and continues. If EL2, runs the drop sequence (below). If EL3 or EL0, branches to `_hang` (silent halt — these are not realistic targets).
2. **DTB save into x19**. The DTB pointer in `x0` is moved to a callee-saved register because the BSS clear in step 4 would zero `_saved_dtb_ptr` if we wrote it first. (This was caught and fixed mid-implementation at P1-A; the bug-and-fix is documented in `start.S` comments.) `x0` is preserved across the EL2 drop's `eret` per the AArch64 GP-register-passthrough rule.
3. **Stack setup**. SP is set to `_boot_stack_top` (defined by the linker script, top of a 16 KiB BSS-allocated buffer; SP grows down).
4. **BSS clear**. Zero `[_bss_start, _bss_end)` in 8-byte stride. The linker script guarantees both bounds are 8-byte-aligned (in fact, page-aligned).
5. **DTB store**. After BSS is cleared, write the saved x19 value to `_saved_dtb_ptr`. Write the saved x20 value (0 / 1) to `_entered_at_el2` so `boot_main()` can surface the el-entry diagnostic.
6. **KASLR init** (P1-C-extras Part B). `kaslr_init()` parses the DTB seed (`/chosen/kaslr-seed` then `/chosen/rng-seed`, with `cntpct_el0` fallback), chooses a 2 MiB-aligned slide in `[0, 16 GiB)`, and applies any `R_AARCH64_RELATIVE` entries in the embedded `.rela.dyn` section. Returns the slide; the boot stub stashes it in `x21` (callee-saved). See `docs/reference/05-kaslr.md`.
7. **MMU enable** (P1-C, slide-aware at P1-C-extras). `mmu_enable(slide)` builds TTBR0 (low 4 GiB identity) and TTBR1 (kernel high-half at `KASLR_LINK_VA + slide`). Per-section permissions on the kernel image; W^X invariant I-12 enforced at PTE bit level via `_Static_assert` on PTE constructors. The L3 page-grain table is shared between TTBR0 and TTBR1. After return, kernel runs with caches enabled; PC is still at load PA via TTBR0.
8. **Long-branch to high VA**. Compute `kaslr_high_va_addr(boot_main)` to get the high VA of `boot_main`, then `br x0` into TTBR1. From this point, all PC-relative addressing in C code resolves to high VAs.
9. **`boot_main()`** runs at high VA. Prints the first half of the banner (arch, el-entry, cpus, mem, dtb, uart, hardening, kernel base).
10. **`phys_init()`** (P1-D). Reads RAM range from DTB, reserves kernel image / struct-page array / DTB blob / low firmware, pushes the rest onto the buddy. Initializes per-CPU magazines. See `docs/reference/06-allocator.md`. Followed by an alloc/free smoke test that exercises the magazine fast path and a non-magazine order; gates on `phys_free_pages() == baseline` after a `magazines_drain_all`.
11. **Banner finishes** (ram, alloc smoke, phase) and prints `Thylacine boot OK`.
12. **Fallthrough to `_hang`**. `boot_main()` is `noreturn`; if it ever returns, the assembly falls through into the `wfi` loop.

#### EL2 → EL1 drop (P1-C-extras)

If `CurrentEL == 2`, `_real_start` runs the canonical drop sequence per ARM ARM D1.6 (process state on exception return):

| Step | Register | Value | Purpose |
|---|---|---|---|
| 1 | `HCR_EL2.RW` | 1 | EL1 is AArch64 |
| 2 | `CNTHCTL_EL2.{EL1PCEN, EL1PCTEN}` | 1 | Allow EL1 to read physical timer + counter (used at P1-G) |
| 3 | `CNTVOFF_EL2` | 0 | No virtual time offset |
| 4 | `VTTBR_EL2` | 0 | No Stage 2 translation (we are not a hypervisor) |
| 5 | `SCTLR_EL1` | `0x30D00800` | `INIT_SCTLR_EL1_MMU_OFF` — RES1 bits set, MMU/caches off, little-endian |
| 6 | `SPSR_EL2` | `0x3C5` | Target = EL1h, DAIF masked |
| 7 | `ELR_EL2` | `.Lpost_el2_drop` | Return target |
| 8 | (eret) | — | Drop to EL1; GP regs (incl. x0 = DTB) preserved |

`SCTLR_EL1 = 0x30D00800` sets bits 11, 20, 22, 23, 28, 29 (RES1 in ARMv8.0+ per ARM ARM D13.2.118) and clears everything else — MMU off, caches off, alignment checks off, little-endian. This matches Linux's `INIT_SCTLR_EL1_MMU_OFF`.

`SPSR_EL2 = 0x3C5` is `M[3:0]=0b0101` (EL1h target with SP_EL1 selected) plus `D|A|I|F = 1` (DAIF masked) — interrupts stay masked across the drop and through subsequent boot until P1-F's GIC + exception vector setup unmasks them.

After `eret`, control resumes at `.Lpost_el2_drop`, which sets `x20 = 1` and joins the `.Lel1_main` flow. `boot_main()` reads `_entered_at_el2` to print the diagnostic line:

```
  el-entry: EL2 -> EL1 (dropped)        # if entered at EL2
  el-entry: EL1 (direct)                # if entered at EL1
```

QEMU virt always shows `EL1 (direct)`. Pi 5 bare metal (post-v1.0) will show `EL2 -> EL1 (dropped)`. The diagnostic is informational; the test gate is still the `Thylacine boot OK` line.

The drop is **not** position-independent code on the entry side — it's plain assembly with PC-relative `adrp`. PIE relocations land at P1-C-extras KASLR (see "Status").

### Memory layout (`arch/arm64/kernel.ld`)

| Section | Mapping intent (post-MMU at P1-C) | Address (P1-A measured) | Size (P1-A measured) |
|---|---|---|---|
| `.text._start` | RX | 0x40080000 (first; ENTRY) | embedded in `.text` |
| `.text` | RX | 0x40080000 | 0x270 (624 B) |
| `.rodata` | R | 0x40081000 | 0x107 (263 B) |
| `.data` | RW | 0x40082000 | 0 (empty at P1-A) |
| `.bss` | RW | 0x40082000 | 0x5000 (20 KiB; mostly the 16 KiB boot stack) |
| `_kernel_end` | — | 0x40087000 | — |

Section ordering enforces the future W^X invariant (`I-12`): `.text` is RX, everything else is non-executable. The linker script's `ALIGN(PAGE_SIZE)` per section ensures no inter-section spillover when the MMU is enabled at P1-C.

Three `ASSERT()` statements in the linker script catch regressions at link time:

- `_kernel_start == 0x40080000` (load address invariant).
- Boot stack size matches `BOOT_STACK_SIZE` (16 KiB).
- Total kernel image < 1 MiB at P1-A. (Currently ~81 KB with debug info, ~74 KB stripped — well under.)

### Boot banner (`kernel/main.c`)

The banner is emitted line-by-line via `uart_puts`. P1-B fills in `mem`, `dtb`, and `uart` from DTB-driven discovery (per `docs/reference/02-dtb.md`); P1-C-extras Part A added the `el-entry` line; P1-C-extras Part B fills in the runtime kernel base via the KASLR slide.

Reference output of a P1-D boot:

```
Thylacine v0.1.0-dev booting...
  arch: arm64
  el-entry: EL1 (direct)
  cpus: 1 (P1-C-extras; SMP at P1-F)
  mem:  2048 MiB at 0x0000000040000000
  dtb:  0x0000000048000000 (parsed)
  uart: 0x0000000009000000 (DTB-driven)
  hardening: MMU+W^X+extinction+KASLR (P1-C-extras; PAC/MTE/CFI at P1-H)
  kernel base: 0xffffa00071480000 (KASLR offset 0x0000000071400000, seed: DTB /chosen/kaslr-seed)
  ram: 2048 MiB total, 2030 MiB free, 18008 KiB reserved (kernel + struct_page + DTB)
  alloc smoke: PASS (256 x 4 KiB + 2 MiB + 4 MiB alloc+free; free count restored)
  phase: P1-D
Thylacine boot OK
```

The `kernel base` line varies per boot (see `docs/reference/05-kaslr.md`). The version string comes from `THYLACINE_VERSION_STRING` (set by `CMakeLists.txt` from the project version `0.1.0`); the phase string from `THYLACINE_PHASE_STRING` (set by the `THYLACINE_PHASE` cache variable, currently `P1-C-extras`).

`boot_main()` calls `dtb_init(_saved_dtb_ptr)` before printing the banner. On success, it calls `dtb_get_memory()` for the memory line and `dtb_get_compat_reg("arm,pl011", ...)` for the UART base. The latter calls `uart_set_base()` to update the active PL011 base, replacing the P1-A hardcoded fallback. If DTB parsing fails, the fallback `0x09000000` (QEMU virt) remains in use and the banner annotates the field as "fallback". `kaslr_kernel_high_base()` / `kaslr_get_offset()` / `kaslr_seed_source_str(kaslr_get_seed_source())` provide the KASLR fields.

### PL011 UART driver (`arch/arm64/uart.c`)

Minimal polled-output driver. Functions:

- `uart_set_base(uintptr_t base)`: update the active PL011 base. Called by `boot_main()` after DTB parse to install the discovered address. If never called, the default base remains the P1-A fallback (`0x09000000`, QEMU virt).
- `uart_get_base()`: query the active base. Used by `boot_main()` to print it in the banner so a developer can confirm DTB-driven discovery worked.
- `uart_putc(char c)`: spins on `FR.TXFF` (TX FIFO full bit, position 5), writes to `DR`. Translates `\n` to `\r\n` for terminal correctness via `uart_puts`.
- `uart_puts(const char *s)`: byte-by-byte; CR-translation as above.
- `uart_puthex64(uint64_t v)`: zero-padded 16-digit hex with `0x` prefix.
- `uart_putdec(uint64_t v)`: unsigned decimal, no padding.

The PL011 base is now runtime-configurable, defaulting to the QEMU virt fallback `0x09000000` for early-boot prints (before `dtb_init` runs). After `dtb_init`, `boot_main()` calls `dtb_get_compat_reg("arm,pl011", ...)` and updates the base via `uart_set_base()`. **Invariant I-15 (DTB-driven discovery) is now satisfied for normal operation**; the fallback exists only for the pre-DTB-parse window plus a recovery path if the DTB is corrupt. The P1-A `FIXME(I-15)` comment was removed at P1-B.

QEMU's `-kernel` direct loader leaves the PL011 in a usable state — TX works without explicit `CR/LCR_H/IBRD/FBRD` programming. P1-F adds the full UART init when interrupt-driven I/O lands.

---

## Data structures

### `_saved_dtb_ptr` (BSS, 8 bytes, aligned)

```
.section .bss
.globl _saved_dtb_ptr
.align 3
_saved_dtb_ptr: .skip 8
```

Holds the DTB physical address handed to us by the bootloader in `x0`. Populated by `_start` after BSS clear; read by `boot_main()` via the `extern volatile u64 _saved_dtb_ptr` declaration in `main.c`.

### Boot stack

16 KiB BSS-allocated buffer, page-aligned. `_boot_stack_bottom` and `_boot_stack_top` bracket it; `_start` sets `SP = _boot_stack_top` (highest address; SP grows down).

A 4 KiB **guard page** sits between BSS general data and the stack at `[_boot_stack_guard, _boot_stack_bottom)`. The page is BSS-allocated (zero-cleared at boot) but its L3 PTE is set to zero in `arch/arm64/mmu.c` — so any access (including a stack overflow that walks below `_boot_stack_bottom`) triggers a translation fault at EL1 stage 1 with `FAR_EL1` inside the guard region. Until P1-F's exception handler lands, the fault wedges QEMU; once the handler exists, it routes to `extinction("kernel stack overflow", FAR_EL1)`.

The guard layout — 4 KiB page below the stack, mapped non-present — defends `boot_main()` and any future early kernel code. Each Phase 2 thread gets its own guard page once per-thread stacks land.

### Kernel image symbols (linker-script-defined)

| Symbol | Meaning | Used by |
|---|---|---|
| `_kernel_start` | First byte of `.text` (== load address) | `boot_main()` for the banner; future relocator (P1-C-extras KASLR) |
| `_kernel_end` | First byte past `.bss` (page-aligned) | Future allocator bootstrap (P1-D) — first usable physical page is `_kernel_end` |
| `_bss_start` / `_bss_end` | BSS bounds | `_start` BSS clear loop |
| `_boot_stack_guard` | First byte of the 4 KiB stack guard page (in BSS) | `mmu.c` — overrides this PTE to non-present in L3 |
| `_boot_stack_bottom` | First usable byte of the stack (== `_boot_stack_guard + 4 KiB`) | linker ASSERT |
| `_boot_stack_top` | Top of the boot stack | `_start` stack setup |
| `_saved_dtb_ptr` | DTB physical address (BSS variable) | `_start` writes; `boot_main()` reads |
| `_entered_at_el2` | 1 if dropped from EL2; 0 if direct EL1 entry (BSS variable) | `_start` writes; `boot_main()` reads for the el-entry banner line |

---

## Spec cross-reference

No formal specs at P1-A. P1-C's KASLR work introduces optional `mmu.tla` (page table validity); the boot path itself stays spec-free at P1-A.

---

## Tests

P1-A has no unit-test infrastructure yet (test harness lands in P1-I). Verification at this sub-chunk is the **boot integration test**:

```
tools/build.sh kernel
tools/run-vm.sh
# Expect: boot banner emitted, then silence (kernel in WFI loop).
# `Thylacine boot OK` is the agentic-loop success signal.
```

Manual verification confirmed at commit landing (this sub-chunk's commit). Automated verification via `tools/test.sh` lands in P1-I.

---

## Error paths

Boot-path error handling at P1-C-extras:

| Condition | Behavior | Status |
|---|---|---|
| Entered at EL3 / EL0 | Silent halt at `_hang` | Defensive; not realistic on QEMU virt or Pi 5 (both deliver EL1 or EL2) |
| Entered at EL2 | Drop to EL1; flag in `_entered_at_el2` | Designed (P1-C-extras); banner reports it |
| Boot-stack overflow | Translation fault on guard page | Designed (P1-C-extras); P1-F's handler routes to `extinction("kernel stack overflow", FAR_EL1)` |
| `boot_main()` returns | Falls through to `_hang` | Designed; defense in depth |
| UART TX FIFO full | Spin on `FR.TXFF` | Bounded by host-side QEMU draining; not a real concern |
| BSS clear miscount | Linker `ASSERT()` catches misalignment | Compile-time guard |
| W^X PTE constructor regression | `_Static_assert` in `mmu.c` fails the build | P1-C compile-time guard |
| Unhandled CPU exception | Wedged QEMU (no vector table yet) | Provisional; P1-F installs `arch/arm64/vectors.S` |

`extinction(msg)` (P1-C) is the kernel ELE primitive: prints `EXTINCTION: <msg>` then halts. Stack-overflow / W^X-violation / unhandled-fault paths route through it once P1-F's exception infrastructure lands.

---

## Performance characteristics

Measurements on QEMU virt under Hypervisor.framework on Apple Silicon:

| Metric | Measured (P1-B) | VISION §4.5 budget | Notes |
|---|---|---|---|
| Boot to UART banner | ~50 ms | < 500 ms | Comfortable margin. DTB parse adds ~150 µs; negligible. |
| Kernel ELF size (debug build) | 91 KB | < 1 MB target | Mostly DWARF debug info; +10 KB from P1-A for DTB parser. |
| Kernel ELF size (stripped) | 84 KB | (not gated) | Target < 100 KB through Phase 1. |
| Kernel flat binary | 8.2 KB | (not gated) | `objcopy -O binary`; loaded sections only (no BSS). |
| DTB parse total | ~150 µs | (not gated) | `dtb_init` + `dtb_get_memory` + `dtb_get_compat_reg("arm,pl011")`. |
| `uart_putc` latency | ~µs (host-bound) | (not gated) | QEMU PL011 emulation; P1-F adds IRQ-driven TX. |

The boot-to-banner figure is informal; rigorous measurement lands in P1-I with the Phase 1 exit benchmark suite.

---

## Status

**Implemented at P1-A**:

- ELF kernel that builds via `tools/build.sh kernel`.
- ARM64 entry at EL1 with stack setup, BSS clear, DTB pointer save.
- PL011 polled UART output (hardcoded base, fallback now).
- Boot banner per `TOOLING.md §10` ABI contract.
- `_hang` clean-halt path.
- `tools/run-vm.sh` QEMU launcher with `-cpu max -smp 4 -m 2G -nographic` defaults.
- CMake + Cargo-ready build infrastructure.

**Implemented at P1-B**:

- Linux ARM64 image header in `start.S` (64 bytes; `ARM\x64` magic; `_image_size` linker-resolved). Triggers QEMU's `load_aarch64_image()` path so the DTB is loaded into RAM and its address passed in `x0`.
- Flat-binary build target via `objcopy -O binary` (produces `thylacine.bin` alongside `thylacine.elf`).
- `tools/run-vm.sh` switched to `-kernel thylacine.bin`.
- DTB parser (`lib/dtb.c` + `kernel/include/thylacine/dtb.h`); see `docs/reference/02-dtb.md`.
- DTB-driven UART base via `uart_set_base()`; resolves invariant I-15.
- DTB-driven memory size in the banner (`mem: 2048 MiB at 0x40000000`).

**Implemented at P1-C**:

- MMU enable in `_real_start` between BSS clear and `boot_main()`; identity map of low 4 GiB; per-section permissions for the kernel image; W^X invariant I-12 enforced at PTE bit level via `_Static_assert` on PTE constructors. See `docs/reference/03-mmu.md`.
- `extinction()` / `extinction_with_addr()` / `ASSERT_OR_DIE()` — the kernel ELE primitive with the `EXTINCTION:` ABI prefix. See `docs/reference/04-extinction.md`.

**Implemented at P1-C-extras** (Part A — EL drop + guard page):

- EL2 → EL1 drop sequence in `_real_start` for firmware that hands the kernel control at EL2 (Pi 5). QEMU virt continues to enter at EL1 and falls through directly. The boot banner now carries an `el-entry: <EL1 (direct) | EL2 -> EL1 (dropped)>` diagnostic via the new `_entered_at_el2` BSS variable. EL3 / EL0 entry halts silently.
- Boot-stack guard page: a 4 KiB BSS slot at `_boot_stack_guard` (immediately below `_boot_stack_bottom`) whose L3 PTE is zeroed by `arch/arm64/mmu.c`. A stack overflow now triggers a translation fault on the guard region — much louder than silently corrupting BSS. The fault diagnostic (`extinction("kernel stack overflow", FAR_EL1)`) gets wired in P1-F when exception vectors land.

**Implemented at P1-C-extras** (Part B — KASLR):

- Toolchain flipped to `-fpie -fdirect-access-external-data -mcmodel=tiny`; linker flipped to `-Wl,-pie -Wl,-z,text -Wl,-z,norelro -Wl,-z,nopack-relative-relocs -Wl,--no-dynamic-linker`. Kernel is now a static-PIE ELF.
- Linker script links at `KERNEL_LINK_VA = 0xFFFFA00000080000` with `AT(KERNEL_LOAD_PA = 0x40080000)`. `.rela.dyn` retained in the loaded image.
- New module `arch/arm64/kaslr.{h,c}` (~150 LOC). Tries `/chosen/kaslr-seed` then `/chosen/rng-seed` then `cntpct_el0` for entropy; mixes via SipHash-style avalanche; produces 13-bit (8192-bucket) 2 MiB-aligned offset in `[0, 16 GiB)`; walks `.rela.dyn` applying `R_AARCH64_RELATIVE` entries (currently 0).
- `arch/arm64/mmu.c` now builds TTBR1 mapping at `KASLR_LINK_VA + slide` using the SHARED `l3_kernel` page-grain table. Page-table footprint: 40 KiB BSS-allocated.
- `arch/arm64/start.S` calls `kaslr_init` after BSS clear, passes the slide to `mmu_enable(slide)`, then long-branches into the high VA via `kaslr_high_va_addr` + `br x0`.
- Boot banner shows `kernel base: 0x..., KASLR offset 0x..., seed: <source>` (varies per boot).
- Invariant **I-16** satisfied: kernel base differs across 10 consecutive boots.

**Implemented at P1-D**:

- Physical frame allocator: buddy + per-CPU magazines + DTB-driven bootstrap. New `mm/buddy.{h,c}` (~280 LOC), `mm/magazines.{h,c}` (~110 LOC), `mm/phys.{h,c}` (~170 LOC), `kernel/include/thylacine/page.h` (struct page + flags + PFN/page conversion), `kernel/include/thylacine/spinlock.h` (lock stub). `boot_main` calls `phys_init()` then runs an alloc/free smoke test. See `docs/reference/06-allocator.md`. The kaslr.c PA-range accessors (`kaslr_kernel_pa_start` / `kaslr_kernel_pa_end`) gain a `volatile` qualifier to defeat a clang `-O2 -fpie -mcmodel=tiny` constant-fold; the comment in `kaslr.c` documents the fragile interaction.

**Not yet implemented**:

- SLUB kernel object allocator (P1-E).
- GIC + exception vectors (P1-F). The guard page mapping is in place but the fault handler that observes it lands here.
- ARM generic timer (P1-G).
- Hardening flags (P1-H).
- Phase 1 exit verification (P1-I).

**Landed**: P1-A at commit `2b332d8`; P1-B at commit `d3e33a8`; P1-C at commit `6462227`; P1-C-extras Part A at commit `ff22ca3`; P1-C-extras Part B at commit `74fd391`; P1-D at commit `198c48c`.

---

## Caveats

### DTB delivery via QEMU `-kernel` ELF (resolved at P1-B)

At P1-A the kernel was loaded as an ELF; QEMU's ELF loader runs `is_linux = 0` and skips the DTB load entirely. The DTB pointer in `x0` was therefore `0`, and the DTB itself was nowhere in RAM. P1-B added the Linux ARM64 image header (64 bytes at offset 0 of the binary, with `ARM\x64` magic at 0x38) and switched the build to produce a flat binary alongside the ELF; QEMU now detects the Linux Image, loads the DTB at `0x48000000` (per `arm_load_dtb()`), and passes the address in `x0`. See `docs/reference/02-dtb.md` "Linux ARM64 image header" for the investigation.

`tools/run-vm.sh` and `tools/test.sh` use `-kernel thylacine.bin` (the flat binary) for this reason. The ELF `thylacine.elf` remains for lldb / objdump / readelf use.

### Hardcoded UART base (I-15) — resolved at P1-B

P1-A used a hardcoded PL011 base in `uart.c` and tracked it as a one-chunk-only `FIXME(I-15)`. P1-B replaced the hardcoded base with DTB-driven discovery via `dtb_get_compat_reg("arm,pl011", ...)`. The fallback (`0x09000000`, QEMU virt) remains in `uart.c` for the pre-`dtb_init` window plus recovery when the DTB is unparseable, but the normal-operation path is fully DTB-driven. Invariant I-15 satisfied.

### Boot stack guard page (resolved at P1-C-extras)

P1-A through P1-C had no guard page — stack overflow silently corrupted the BSS region below `_boot_stack_bottom`. P1-C-extras adds a 4 KiB non-present mapping at `_boot_stack_guard` (`mmu.c`'s `build_identity_map()` zeroes the L3 PTE for the guard slot). Stack overflow now triggers a translation fault on the guard region. The fault diagnostic — routing to `extinction("kernel stack overflow", FAR_EL1)` — lands in P1-F when exception vectors are installed; until then, the fault wedges QEMU but is recognisable in the QEMU trace as a stage-1 EL1 abort with `FAR_EL1` inside `[_boot_stack_guard, _boot_stack_bottom)`.

### No exception handling

If anything in `_start` or `boot_main()` faults, the CPU takes a synchronous exception with no handler — the result is QEMU reset or a wedged state. P1-F installs the exception vector table. Until then, treat boot as a fragile linear path.

### Pi 5 entry-EL diagnostic only

The EL2 → EL1 drop is exercised in code review (the disassembled sequence is correct) but not yet on a real Pi 5 — QEMU virt always enters at EL1. The first time it runs end-to-end is when the Pi 5 board target arrives (post-v1.0). Until then, the `el-entry: EL2 -> EL1 (dropped)` banner line is the diagnostic that confirms it worked.

---

## See also

- `docs/ARCHITECTURE.md §5` — boot sequence design intent (the target-state shape).
- `docs/ARCHITECTURE.md §22` — hardware platform model + DTB discipline.
- `docs/TOOLING.md §3` — `run-vm.sh` canonical flags.
- `docs/TOOLING.md §10` — boot banner ABI contract.
- `docs/phase1-status.md` — Phase 1 pickup guide; trip hazards.
- `docs/reference/00-overview.md` — system-wide layer cake.
