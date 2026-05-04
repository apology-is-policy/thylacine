# Phase 1 — status and pickup guide

Authoritative pickup guide for **Phase 1: Kernel skeleton**.

## TL;DR

Phase 1 brings up the kernel skeleton: boot path through MMU + KASLR, exception vectors, GIC v2/v3 autodetected, PL011 UART, ARM generic timer, buddy + per-CPU magazines + SLUB allocator, hardening defaults (CFI, PAC, MTE, BTI, LSE, KASLR, stack canaries), DTB parsing. Boot to UART banner in < 500ms; the kernel hangs in a debug loop with timer ticks counting on UART. No processes, no devices beyond UART, no userspace yet.

Per `ROADMAP.md §4`.

## Landed chunks

| Commit SHA | What | Tests |
|---|---|---|
| 2b332d8 | **P1-A**: toolchain + build system + minimal boot stub. CMake + clang 22 + ld.lld 22 cross-compile to `aarch64-none-elf`. `arch/arm64/start.S` + `kernel.ld` + `arch/arm64/uart.c` + `kernel/main.c`. Boot banner per `TOOLING.md §10`. `tools/run-vm.sh` + `tools/test.sh` + Makefile. Kernel ELF 81 KB debug / 74 KB stripped. | Manual + `tools/test.sh` (boot-banner regex match within 10s). PASS. |
| d3e33a8 | **P1-B**: DTB parsing + Linux ARM64 image header + flat-binary build + DTB-driven UART base. `lib/dtb.c` (~340 LOC) implements an FDT v17 parser with `dtb_init`, `dtb_get_memory`, `dtb_get_compat_reg`, `dtb_get_chosen_kaslr_seed`. `start.S` gains a 64-byte Linux ARM64 image header at offset 0 (`ARM\x64` magic at 0x38) so QEMU's `load_aarch64_image()` loads the DTB and passes it in `x0`. CMake post-link `objcopy -O binary` produces `thylacine.bin` alongside the ELF; `tools/run-vm.sh` uses the binary. Resolves I-15 violation: `uart.c` PL011 base now updated from `dtb_get_compat_reg("arm,pl011")`. Banner shows `mem: 2048 MiB at 0x40000000`, `dtb: 0x48000000 (parsed)`, `uart: 0x09000000 (DTB-driven)`. | `tools/test.sh` PASS. |

## Remaining work

(Sub-chunk plan, refined at Phase 1 entry. P1-A landed; tentative order for the rest:)

1. ✅ **P1-A: Toolchain + tools/run-vm.sh + boot stub.** Landed.
2. ✅ **P1-B: DTB parsing.** Landed. **Resolved the DTB pointer mystery via Linux ARM64 image header** (the alternative — probing — is documented in caveats but not used). FDT parser (~340 LOC). DTB-driven UART base resolves I-15. `arm,pl011` lookup uses a stack-based per-node accumulator (single-flag approach missed the match because PL011's `compatible` property comes after its `reg`). `be32_load` uses a `volatile` u32 read to prevent compiler fusion into a misaligned 8-byte load on Device memory (caught empirically — debug printfs broke fusion and made the parser work; without them it faulted silently).
3. **P1-C: MMU + KASLR.** `arch/arm64/mmu.c` enables MMU with TTBR0 (identity) + TTBR1 (kernel high half); KASLR offset applied; relocations processed. `_Static_assert` on PTE bit layout for W^X. Boot stack guard page added. EL2→EL1 drop diagnostic via UART (replaces silent `.Lnot_el1` halt). Panic infrastructure (`panic(fmt, ...)` printing `PANIC:` prefix per TOOLING ABI). Once cacheable Normal memory is in use, the `volatile` constraint in `be32_load` becomes optional (still keep — defends against bare-metal recovery paths where MMU may be off).
4. **P1-D: Physical allocator (buddy + magazines).** `mm/buddy.c` + `mm/magazines.c` per `ARCHITECTURE.md §6.3`.
5. **P1-E: Kernel object allocator (SLUB).** `mm/slub.c` with standard `kmalloc-N` caches.
6. **P1-F: GIC + exception vectors.** `arch/arm64/gic.c` + `arch/arm64/vectors.S` + `arch/arm64/exception.c`. Real UART init (CR/LCR_H/IBRD/FBRD).
7. **P1-G: ARM generic timer.** `arch/arm64/timer.c`. Timer IRQ counts ticks on UART (no scheduler yet).
8. **P1-H: Hardening enablement.** All compile flags from `ARCHITECTURE.md §24.2`. Runtime PAC + BTI + MTE + LSE detection. ELF loader rejects RWX (placeholder; full loader in Phase 2). KASLR randomization verified across 10 boots. **Removes the `FIXME(I-15)` from `uart.c`** once DTB-driven UART base is in place from P1-B.
9. **P1-I: Phase 1 exit verification.** All `ROADMAP.md §4.2` exit criteria met. ASan + UBSan instrumentation. CI workflow.

## Exit criteria status

(Copy from `ROADMAP.md §4.2`; tick as deliverables complete.)

- [x] **QEMU `virt` ARM64 boots to a UART banner without crashing.** Landed at P1-A; still passes at P1-B with DTB-driven banner.
- [ ] Boot to UART banner: < 500ms. Informally measured ~50 ms at P1-A/B (DTB parse adds ~150 µs; negligible). Rigorous measurement at P1-I.
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

- **Boot timing budget (500ms)** is tight. Profile each boot subsystem; keep the budget split per-subsystem. P1-A/B informal measurement is ~50 ms — comfortable margin.
- ✅ ~~DTB pointer at QEMU `-kernel` ELF entry~~. Resolved at P1-B. Linux ARM64 image header at offset 0 of `start.S` triggers QEMU's `load_aarch64_image()`, which loads the DTB at `0x48000000` and passes the address in `x0`.
- ✅ ~~Hardcoded UART base in P1-A~~. Resolved at P1-B. `uart.c` now defaults to `0x09000000` for early prints (before `dtb_init`), then `boot_main()` calls `uart_set_base()` with the DTB-discovered address from `dtb_get_compat_reg("arm,pl011")`.
- **Compiler fusion of `be32_load` calls into unaligned u64 loads** (NEW from P1-B): clang fuses two adjacent 4-byte loads into a single 8-byte load. On Device memory (MMU off) with property data only 4-aligned, the fused load faults. **Mitigation**: `volatile` qualifier on the u32 read in `be32_load` (`lib/dtb.c`). **Codified** in source comments + `docs/reference/02-dtb.md`. Once MMU is on (P1-C+), the constraint relaxes but the volatile is kept as a defensive cushion.
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

| Surface | Files | Why | Landed at |
|---|---|---|---|
| Exception entry | `arch/arm64/start.S`, `arch/arm64/exception.c` (P1-F), `arch/arm64/vectors.S` (P1-F) | Every fault path | P1-A (start.S only); P1-B added Linux image header |
| Boot banner ABI | `kernel/main.c`, `arch/arm64/uart.c` | Tooling ABI per TOOLING.md §10 | P1-A; updated at P1-B |
| DTB parser | `lib/dtb.c`, `kernel/include/thylacine/dtb.h` | Hardware view derives entirely from DTB (I-15); malformed DTB must be detected | P1-B |
| Linux ARM64 image header | `arch/arm64/start.S` (offset 0..0x40), `arch/arm64/kernel.ld` (`_image_size` symbol) | QEMU `load_aarch64_image()` detection; DTB delivery | P1-B |
| Allocator | `mm/buddy.c` (P1-D), `mm/slub.c` (P1-E), `mm/magazines.c` (P1-D) | Allocation correctness | (planned) |
| Page tables | `arch/arm64/mmu.c` (P1-C), `mm/wxe.c` (P1-C) | W^X invariant (I-12) | (planned) |
| KASLR | `arch/arm64/kaslr.c` (P1-C) | Entropy + relocation correctness (I-16) | (planned) |
| LSE detection | `arch/arm64/atomic.S` (P1-H) | Runtime patching correctness | (planned) |

Audit at Phase 1 exit (per `ROADMAP.md §4.2`): no P0/P1 findings on the boot path.

## Specs landing this phase

None mandatory at Phase 1. Optional: a sketch of `mmu.tla` for page table validity, but not gating.

## Performance budget contribution at this phase

- Boot to UART banner: < 500ms (full budget).
- `kmalloc(small)` p99: < 50ns (uncontested).
- Allocator scaling: linear with core count for refill operations.

(Measured at Phase 1 exit; carried forward as the floor for subsequent phases.)
