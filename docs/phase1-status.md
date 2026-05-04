# Phase 1 — status and pickup guide

Authoritative pickup guide for **Phase 1: Kernel skeleton**.

## TL;DR

Phase 1 brings up the kernel skeleton: boot path through MMU + KASLR, exception vectors, GIC v2/v3 autodetected, PL011 UART, ARM generic timer, buddy + per-CPU magazines + SLUB allocator, hardening defaults (CFI, PAC, MTE, BTI, LSE, KASLR, stack canaries), DTB parsing. Boot to UART banner in < 500ms; the kernel hangs in a debug loop with timer ticks counting on UART. No processes, no devices beyond UART, no userspace yet.

Per `ROADMAP.md §4`.

## Landed chunks

| Commit SHA | What | Tests |
|---|---|---|
| *(pending)* | **P1-A**: toolchain + build system + minimal boot stub. CMake + clang 22 + ld.lld 22 cross-compile to `aarch64-none-elf`. `arch/arm64/start.S` + `kernel.ld` + `arch/arm64/uart.c` + `kernel/main.c`. Boot banner per `TOOLING.md §10`. `tools/run-vm.sh` + `tools/test.sh` + Makefile. Kernel ELF 81 KB debug / 74 KB stripped. | Manual + `tools/test.sh` (boot-banner regex match within 10s). PASS. |

## Remaining work

(Sub-chunk plan, refined at Phase 1 entry. P1-A landed; tentative order for the rest:)

1. ✅ **P1-A: Toolchain + tools/run-vm.sh + boot stub.** Landed.
2. **P1-B: DTB parsing.** `lib/dtb.c` extracts memory regions, GIC base, UART base, timer IRQ, KASLR seed. No libfdt; minimal hand-rolled parser. **Resolves the DTB pointer mystery** (observed as `0x0` at P1-A): probe a known location (`0x40000000`) and validate via FDT magic, since QEMU `-kernel` with ELF doesn't synthesize the Linux ARM64 boot protocol.
3. **P1-C: MMU + KASLR.** `arch/arm64/mmu.c` enables MMU with TTBR0 (identity) + TTBR1 (kernel high half); KASLR offset applied; relocations processed. `_Static_assert` on PTE bit layout for W^X. Boot stack guard page added. EL2→EL1 drop diagnostic via UART (replaces silent `.Lnot_el1` halt). Panic infrastructure (`panic(fmt, ...)` printing `PANIC:` prefix per TOOLING ABI).
4. **P1-D: Physical allocator (buddy + magazines).** `mm/buddy.c` + `mm/magazines.c` per `ARCHITECTURE.md §6.3`.
5. **P1-E: Kernel object allocator (SLUB).** `mm/slub.c` with standard `kmalloc-N` caches.
6. **P1-F: GIC + exception vectors.** `arch/arm64/gic.c` + `arch/arm64/vectors.S` + `arch/arm64/exception.c`. Real UART init (CR/LCR_H/IBRD/FBRD).
7. **P1-G: ARM generic timer.** `arch/arm64/timer.c`. Timer IRQ counts ticks on UART (no scheduler yet).
8. **P1-H: Hardening enablement.** All compile flags from `ARCHITECTURE.md §24.2`. Runtime PAC + BTI + MTE + LSE detection. ELF loader rejects RWX (placeholder; full loader in Phase 2). KASLR randomization verified across 10 boots. **Removes the `FIXME(I-15)` from `uart.c`** once DTB-driven UART base is in place from P1-B.
9. **P1-I: Phase 1 exit verification.** All `ROADMAP.md §4.2` exit criteria met. ASan + UBSan instrumentation. CI workflow.

## Exit criteria status

(Copy from `ROADMAP.md §4.2`; tick as deliverables complete.)

- [x] **QEMU `virt` ARM64 boots to a UART banner without crashing.** Landed at P1-A; verified via `tools/test.sh`.
- [ ] Boot to UART banner: < 500ms. Informally measured ~50 ms at P1-A; rigorous measurement at P1-I.
- [ ] `kmalloc`/`kfree` round-trip 10,000 allocations without leak. P1-D / P1-E.
- [ ] GIC initialized; timer IRQ fires at 1000 Hz (verified via UART counter). P1-F / P1-G.
- [ ] MMU on; kernel VA map correct (read/write kernel data, no fault). P1-C.
- [ ] KASLR: kernel base address differs across boots (verified across 10 boots). P1-C.
- [ ] LSE atomic ops verified via runtime detection; LL/SC fallback works. P1-H.
- [ ] PAC return-address signing verified (forge a return address; expect kernel panic with PAC-mismatch info). P1-H.
- [ ] BTI enabled (deliberate indirect branch to non-BTI target panics cleanly). P1-H.
- [ ] MTE enabled where supported (deliberate UAF detected by MTE). P1-H.
- [ ] Sanitizer build runs without false positives on boot path. P1-I.
- [ ] No P0/P1 audit findings on the boot path. P1-I (audit at phase exit).

## Build + verify commands

```bash
# Build the kernel ELF (build/kernel/thylacine.elf, ~81 KB debug / ~74 KB stripped)
tools/build.sh kernel
# Or via Makefile alias:
make kernel

# Run interactively (kernel boots, prints banner, hangs in WFI loop;
# Ctrl-A x to quit QEMU)
tools/run-vm.sh
make run

# Run + GDB stub on :1234 (kernel halted at entry; connect with lldb)
tools/run-vm.sh --gdb
make gdb

# Integration test: boot + verify "Thylacine boot OK" banner within 10s
tools/test.sh
make test
```

Reference output of a successful boot:

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

Toolchain dependencies (Apple Silicon Mac via Homebrew):
- `clang` + `ld.lld` from `brew install llvm lld` (clang 22, lld 22 at P1-A).
- `qemu-system-aarch64` from `brew install qemu` (10.0+).
- `cmake` (≥ 3.20) and `make`.
- `openjdk` for TLA+ (no specs at Phase 1; install for P2).

## Trip hazards

- **Boot timing budget (500ms)** is tight. Profile each boot subsystem; keep the budget split per-subsystem. P1-A informal measurement is ~50 ms — comfortable margin.
- **DTB pointer at QEMU `-kernel` ELF entry**: observed as `0x0` at P1-A. QEMU's `-kernel` direct loader for ELF kernels (without the Linux ARM64 image header) does not pass DTB via `x0`. **P1-B must probe** a known location (`0x40000000`) and validate via FDT magic (`0xd00dfeed`). Linux's recovery path uses the same probe; we follow it.
- **Hardcoded UART base in P1-A** (`arch/arm64/uart.c:21`): `PL011_BASE = 0x09000000` violates invariant I-15 (DTB-driven discovery). **Explicit one-chunk-only shortcut**; P1-B replaces with DTB-discovered base. The `FIXME(I-15)` comment in source is the in-tree reminder.
- **DTB parsing edge cases**: QEMU's DTB is well-formed; real hardware (Pi 5, post-v1.0) is not. v1.0 doesn't see Pi 5; risk is low.
- **GIC v2 vs v3 autodetection**: QEMU `virt` defaults to GICv2 on older versions, GICv3 on newer. P1-A uses `gic-version=3` explicitly in `tools/run-vm.sh`; P1-F adds DTB-based autodetection.
- **MMU enablement sequence**: order matters — TTBR registers, TCR, MAIR, then SCTLR.M=1. Get this wrong and the kernel crashes silently or in unexpected ways. P1-C work.
- **KASLR seed entropy**: Phase 1 reads from `/chosen/kaslr-seed` in DTB. UEFI provides high-entropy seed; missing seed falls back to a weak boot-counter source with a logged warning. P1-C work.
- **Hardening flag interactions**: `-fsanitize=cfi` + ThinLTO + custom linker scripts can interact unexpectedly. Phase 1 catches these; subsequent phases inherit a working build. P1-H work.
- **MTE performance**: measure during P1-H; if > 15% overhead on critical paths, restrict to allocations only (per `ARCHITECTURE.md §24.3`).
- **Boot banner contract**: the exact string `Thylacine boot OK` is tooling ABI per `TOOLING.md §10`. Don't change without coordinating.
- **Boot stack guard page**: missing at P1-A (no MMU). Stack overflow corrupts BSS. Risk is low because `boot_main()` does no significant stack work. P1-C adds the guard.
- **No exception handling at P1-A**: any fault wedges QEMU. P1-F installs the exception vector table.

## Known deltas from ARCH

- **DTB delivery via QEMU `-kernel` ELF**: ARCH §5.1 documents the standard Linux ARM64 boot protocol (DTB ptr in `x0`). QEMU's `-kernel` direct ELF entry doesn't synthesize this for ELF kernels without the Linux image header. P1-B must probe; ARCH §5 is being updated implicitly by this delta. If the probe approach proves unreliable, we may add the Linux image header to the kernel binary (which restores the standard protocol). Decision deferred to P1-B implementation.

## References

- `docs/ARCHITECTURE.md §4` (target architecture), `§5` (boot sequence), `§6` (memory), `§12` (interrupt handling), `§22` (hardware platform model), `§24` (hardening).
- `docs/ROADMAP.md §4` (Phase 1 deliverables, exit criteria, risks).
- `docs/TOOLING.md §3` (run-vm.sh canonical flags), `§10` (boot banner contract).
- `CLAUDE.md` (operational framework, audit-trigger surfaces).

## Audit-trigger surfaces introduced this phase

| Surface | Files | Why |
|---|---|---|
| Exception entry | `arch/arm64/start.S`, `arch/arm64/exception.c`, `arch/arm64/vectors.S` | Every fault path |
| Allocator | `mm/buddy.c`, `mm/slub.c`, `mm/magazines.c` | Allocation correctness |
| Page tables | `arch/arm64/mmu.c`, `mm/wxe.c` | W^X invariant (I-12) |
| KASLR | `arch/arm64/kaslr.c` | Entropy + relocation correctness (I-16) |
| LSE detection | `arch/arm64/atomic.S` | Runtime patching correctness |

Audit at Phase 1 exit (per `ROADMAP.md §4.2`): no P0/P1 findings on the boot path.

## Specs landing this phase

None mandatory at Phase 1. Optional: a sketch of `mmu.tla` for page table validity, but not gating.

## Performance budget contribution at this phase

- Boot to UART banner: < 500ms (full budget).
- `kmalloc(small)` p99: < 50ns (uncontested).
- Allocator scaling: linear with core count for refill operations.

(Measured at Phase 1 exit; carried forward as the floor for subsequent phases.)
