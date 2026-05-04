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

- **EL**: EL1h (kernel mode, `SP_EL1` selected). QEMU virt boots at EL1 by default; if we ever come in at EL2 (Pi 5 bare metal post-v1.0), the EL2→EL1 drop sequence lands at P1-C / `arch/arm64/rpi5/boot.S`.
- **x0**: DTB physical address. With the Linux ARM64 image header in place from P1-B, QEMU loads the DTB at `loader_start + min(ram_size/2, 128 MiB)` — for our 2 GiB RAM, that's `0x48000000` — and passes the address in `x0`. (At P1-A, before the image header was added, x0 was 0 and the DTB was never loaded — see `docs/reference/02-dtb.md` "Linux ARM64 image header" for the investigation that surfaced this.)
- **x1..x3**: 0 (reserved by Linux protocol).
- **MMU off, caches off**, interrupts masked. Single CPU executing.

The Linux ARM64 image header (64 bytes) lives at offset 0 of the binary. `code0` at offset 0 is `b _real_start` — a branch over the header into the actual boot code. The header carries `text_offset = 0x80000`, `image_size = _kernel_end - _kernel_start` (linker-resolved via `_image_size` symbol in `kernel.ld`), and `flags = 0xa` (little-endian + 4 KiB granule + placement-anywhere). `magic` at offset 0x38 is `0x644d5241` ("ARM\x64") — this is what QEMU detects.

After the branch, `_real_start` performs, in order:

1. **EL check**. Reads `CurrentEL`, branches to `.Lnot_el1` if not EL1. The wrong-EL handler halts silently — P1-C will replace this with a proper EL2→EL1 drop and a UART diagnostic.
2. **DTB save into x19**. The DTB pointer in `x0` is moved to a callee-saved register because the BSS clear in step 4 would zero `_saved_dtb_ptr` if we wrote it first. (This was caught and fixed mid-implementation at P1-A; the bug-and-fix is documented in `start.S` comments.)
3. **Stack setup**. SP is set to `_boot_stack_top` (defined by the linker script, top of a 16 KiB BSS-allocated buffer; SP grows down).
4. **BSS clear**. Zero `[_bss_start, _bss_end)` in 8-byte stride. The linker script guarantees both bounds are 8-byte-aligned (in fact, page-aligned).
5. **DTB store**. After BSS is cleared, write the saved x19 value to `_saved_dtb_ptr`.
6. **Branch to `boot_main()`**.
7. **Fallthrough to `_hang`**. `boot_main()` is `noreturn`; if it ever returns, the assembly falls through into the `wfi` loop.

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

The banner is emitted line-by-line via `uart_puts`. P1-B fills in `mem`, `dtb`, and `uart` from DTB-driven discovery (per `docs/reference/02-dtb.md`); remaining fields are placeholders annotated with the sub-chunk that fills them.

Reference output of a P1-B boot:

```
Thylacine v0.1.0-dev booting...
  arch: arm64
  cpus: 1 (P1-B; SMP at P1-F)
  mem:  2048 MiB at 0x0000000040000000
  dtb:  0x0000000048000000 (parsed)
  uart: 0x0000000009000000 (DTB-driven)
  hardening: minimal (P1-B baseline; full stack at P1-H)
  kernel base: 0x0000000040080000 (KASLR at P1-C)
  phase: P1-B
Thylacine boot OK
```

The version string comes from `THYLACINE_VERSION_STRING` (set by `CMakeLists.txt` from the project version `0.1.0`); the phase string from `THYLACINE_PHASE_STRING` (set by the `THYLACINE_PHASE` cache variable, default `P1-B` after P1-A close).

`boot_main()` calls `dtb_init(_saved_dtb_ptr)` before printing the banner. On success, it calls `dtb_get_memory()` for the memory line and `dtb_get_compat_reg("arm,pl011", ...)` for the UART base. The latter calls `uart_set_base()` to update the active PL011 base, replacing the P1-A hardcoded fallback. If DTB parsing fails, the fallback `0x09000000` (QEMU virt) remains in use and the banner annotates the field as "fallback".

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

P1-C will add a guard page below `_boot_stack_bottom` once the MMU is enabled — a non-present mapping that traps stack overflow into a recoverable fault.

### Kernel image symbols (linker-script-defined)

| Symbol | Meaning | Used by |
|---|---|---|
| `_kernel_start` | First byte of `.text` (== load address) | `boot_main()` for the banner; future relocator (P1-C KASLR) |
| `_kernel_end` | First byte past `.bss` (page-aligned) | Future allocator bootstrap (P1-D) — first usable physical page is `_kernel_end` |
| `_bss_start` / `_bss_end` | BSS bounds | `_start` BSS clear loop |
| `_boot_stack_bottom` / `_boot_stack_top` | Boot stack bracket | `_start` stack setup; future overflow-detection (P1-C) |
| `_saved_dtb_ptr` | DTB physical address (BSS variable) | `_start` writes; `boot_main()` reads |

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

P1-A error paths are minimal:

| Condition | Behavior | P1-A status |
|---|---|---|
| Wrong-EL entry (not EL1) | Silent halt at `.Lnot_el1` | Provisional; P1-C adds UART diagnostic |
| `boot_main()` returns | Falls through to `_hang` | Designed; defense in depth |
| UART TX FIFO full | Spin on `FR.TXFF` | Bounded by host-side QEMU draining; not a real concern |
| BSS clear miscount | Linker `ASSERT()` catches misalignment | Compile-time guard |

P1-C lands the panic infrastructure: `panic(fmt, ...)` prints `PANIC: <message>` then halts. Until then, any unrecoverable condition is a silent halt or a bare-metal exception (which we don't yet handle — exception vectors land at P1-F).

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

**Not yet implemented**:

- MMU enablement (P1-C). All addresses are physical at P1-A/B; W^X is unenforced.
- KASLR (P1-C). Kernel base is fixed at `0x40080000`.
- Physical frame allocator (P1-D).
- SLUB kernel object allocator (P1-E).
- GIC + exception vectors (P1-F).
- ARM generic timer (P1-G).
- Hardening flags (P1-H).
- Phase 1 exit verification (P1-I).

**Landed**: P1-A at commit `2b332d8`; P1-B at commit `(pending hash-fixup)`.

---

## Caveats

### DTB delivery via QEMU `-kernel` ELF (resolved at P1-B)

At P1-A the kernel was loaded as an ELF; QEMU's ELF loader runs `is_linux = 0` and skips the DTB load entirely. The DTB pointer in `x0` was therefore `0`, and the DTB itself was nowhere in RAM. P1-B added the Linux ARM64 image header (64 bytes at offset 0 of the binary, with `ARM\x64` magic at 0x38) and switched the build to produce a flat binary alongside the ELF; QEMU now detects the Linux Image, loads the DTB at `0x48000000` (per `arm_load_dtb()`), and passes the address in `x0`. See `docs/reference/02-dtb.md` "Linux ARM64 image header" for the investigation.

`tools/run-vm.sh` and `tools/test.sh` use `-kernel thylacine.bin` (the flat binary) for this reason. The ELF `thylacine.elf` remains for lldb / objdump / readelf use.

### Hardcoded UART base (I-15) — resolved at P1-B

P1-A used a hardcoded PL011 base in `uart.c` and tracked it as a one-chunk-only `FIXME(I-15)`. P1-B replaced the hardcoded base with DTB-driven discovery via `dtb_get_compat_reg("arm,pl011", ...)`. The fallback (`0x09000000`, QEMU virt) remains in `uart.c` for the pre-`dtb_init` window plus recovery when the DTB is unparseable, but the normal-operation path is fully DTB-driven. Invariant I-15 satisfied.

### Boot stack guard page

The 16 KiB boot stack has no guard page at P1-A — the MMU isn't enabled. Stack overflow corrupts the BSS region just below `_boot_stack_bottom`. Risk is low because `boot_main()` does no significant stack work. P1-C adds an unmapped guard page once the MMU is up.

### No exception handling

If anything in `_start` or `boot_main()` faults, the CPU takes a synchronous exception with no handler — the result is QEMU reset or a wedged state. P1-F installs the exception vector table. Until then, treat boot as a fragile linear path.

---

## See also

- `docs/ARCHITECTURE.md §5` — boot sequence design intent (the target-state shape).
- `docs/ARCHITECTURE.md §22` — hardware platform model + DTB discipline.
- `docs/TOOLING.md §3` — `run-vm.sh` canonical flags.
- `docs/TOOLING.md §10` — boot banner ABI contract.
- `docs/phase1-status.md` — Phase 1 pickup guide; trip hazards.
- `docs/reference/00-overview.md` — system-wide layer cake.
