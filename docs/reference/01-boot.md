# 01 — Boot path (as-built reference)

The kernel's boot path: from QEMU `-kernel` direct entry through `_start` (assembly), `boot_main()` (C), to a halt loop. This document describes what exists in the tree as of **P1-A**; subsequent sub-chunks (P1-B DTB parser, P1-C MMU + KASLR, etc.) extend it.

Scope: `arch/arm64/start.S`, `arch/arm64/kernel.ld`, `arch/arm64/uart.c` + `arch/arm64/uart.h`, `kernel/main.c`, `kernel/include/thylacine/types.h`.

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

QEMU's `-kernel` direct loader hands control to `_start` with:

- **EL**: EL1h (kernel mode, `SP_EL1` selected). QEMU virt boots at EL1 by default; if we ever come in at EL2 (Pi 5 bare metal post-v1.0), the EL2→EL1 drop sequence lands at P1-C / `arch/arm64/rpi5/boot.S`.
- **x0**: DTB physical address (Linux ARM64 boot protocol convention). At P1-A, observed value is `0` for QEMU `-kernel` ELF entry — see [Caveats](#caveats).
- **x1..x3**: 0 (reserved by Linux protocol).
- **MMU off, caches off**, interrupts masked. Single CPU executing.

`_start` performs, in order:

1. **EL check**. Reads `CurrentEL`, branches to `.Lnot_el1` if not EL1. The wrong-EL handler in `start.S:88` halts silently — P1-C will replace this with a proper EL2→EL1 drop and a UART diagnostic.
2. **DTB save into x19**. The DTB pointer in `x0` is moved to a callee-saved register because the BSS clear in step 4 would zero `_saved_dtb_ptr` if we wrote it first. (This was caught and fixed mid-implementation; the bug-and-fix is documented in `start.S` comments.)
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

The banner is emitted line-by-line via `uart_puts`. At P1-A all dynamic fields except `dtb` and `kernel base` are placeholders annotated with the sub-chunk that fills them:

```
Thylacine v0.1.0-dev booting...
  arch: arm64
  cpus: 1 (P1-A; SMP at P1-F)
  mem:  unknown (DTB at P1-B)
  dtb:  0x0000000000000000
  hardening: minimal (P1-A baseline; full stack at P1-H)
  kernel base: 0x0000000040080000 (KASLR at P1-C)
  phase: P1-A
Thylacine boot OK
```

The version string comes from `THYLACINE_VERSION_STRING` (set by `CMakeLists.txt` from the project version `0.1.0`); the phase string from `THYLACINE_PHASE_STRING` (set by the `THYLACINE_PHASE` cache variable, default `P1-A`).

### PL011 UART driver (`arch/arm64/uart.c`)

Minimal polled-output driver. Functions:

- `uart_putc(char c)`: spins on `FR.TXFF` (TX FIFO full bit, position 5), writes to `DR`. Translates `\n` to `\r\n` for terminal correctness via `uart_puts`.
- `uart_puts(const char *s)`: byte-by-byte; CR-translation as above.
- `uart_puthex64(uint64_t v)`: zero-padded 16-digit hex with `0x` prefix.
- `uart_putdec(uint64_t v)`: unsigned decimal, no padding.

The PL011 base is hardcoded to `0x09000000` (QEMU virt fixed mapping). **This violates invariant I-15 (DTB-driven discovery) for one chunk only** — `uart.c:21` carries a `FIXME(I-15)` comment and the trip-hazard log in `docs/phase1-status.md` tracks the violation. P1-B parses the DTB, replaces the constant with the discovered base, and clears the FIXME.

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

P1-A measurements on QEMU virt under Hypervisor.framework on Apple Silicon:

| Metric | Measured (P1-A) | VISION §4.5 budget | Notes |
|---|---|---|---|
| Boot to UART banner | ~50 ms | < 500 ms | Comfortable margin; budget is for the full Phase 1 boot. |
| Kernel ELF size (debug build) | 81 KB | < 1 MB target | Mostly DWARF debug info. |
| Kernel ELF size (stripped) | 74 KB | (not gated) | Will grow as subsystems land; target < 100 KB through Phase 1. |
| `uart_putc` latency | ~µs (host-bound) | (not gated) | QEMU PL011 emulation; P1-F adds IRQ-driven TX. |

The boot-to-banner figure is informal; rigorous measurement lands in P1-I with the Phase 1 exit benchmark suite.

---

## Status

**Implemented at P1-A** (this sub-chunk):

- ELF kernel that builds via `tools/build.sh kernel`.
- ARM64 entry at EL1 with stack setup, BSS clear, DTB pointer save.
- PL011 polled UART output.
- Boot banner per `TOOLING.md §10` ABI contract.
- `_hang` clean-halt path.
- `tools/run-vm.sh` QEMU launcher with `-cpu max -smp 4 -m 2G -nographic` defaults.
- CMake + Cargo-ready build infrastructure.

**Not yet implemented**:

- DTB parsing (P1-B). The DTB pointer is observed as `0x0` under QEMU `-kernel` ELF entry; investigate at P1-B entry whether QEMU passes DTB at a fixed address (typical for ARM64 virt: `0x40000000`) versus through `x0`.
- MMU enablement (P1-C). All addresses are physical at P1-A; W^X is unenforced.
- KASLR (P1-C). Kernel base is fixed at `0x40080000`.
- Physical frame allocator (P1-D).
- SLUB kernel object allocator (P1-E).
- GIC + exception vectors (P1-F).
- ARM generic timer (P1-G).
- Hardening flags (P1-H).
- Phase 1 exit verification (P1-I).

**Landed**: commit `(pending — will be filled in by the hash-fixup commit per CLAUDE.md "Audit-close commit anatomy")`.

---

## Caveats

### DTB pointer observed as `0x0`

QEMU virt's `-kernel` direct loader, when given an ELF kernel without the Linux ARM64 image header (`MZ` magic, branch instruction, `arm64` magic-string at offset 0x38), does not synthesize the Linux ARM64 boot protocol's `x0 = DTB ptr` calling convention. The DTB is still loaded into memory — typically near the start of physical RAM at `0x40000000` — but the kernel must locate it.

**Resolution at P1-B**: switch to one of:

- Add the Linux ARM64 image header to the kernel binary and rely on the standard protocol.
- Pass the DTB explicitly via QEMU's `-dtb` flag with a known address (less portable).
- Probe the conventional location (`0x40000000`) and validate via the FDT magic (`0xd00dfeed`).

The probe approach is least invasive and matches Linux's recovery path. Decision deferred to P1-B implementation.

### Hardcoded UART base (I-15 violation, scoped to P1-A)

`arch/arm64/uart.c:21` hardcodes `PL011_BASE = 0x09000000`. This violates invariant I-15 (DTB-driven hardware discovery). The violation is **explicit and time-bounded**: it lasts for one sub-chunk (P1-A only); P1-B parses the DTB and replaces the hardcoded base. The trip-hazard log in `docs/phase1-status.md` tracks this; the `FIXME(I-15)` comment in `uart.c` is the in-source reminder.

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
