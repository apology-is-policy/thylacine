# Phase 1 — status and pickup guide

Authoritative pickup guide for **Phase 1: Kernel skeleton**.

## TL;DR

Phase 1 brings up the kernel skeleton: boot path through MMU + KASLR, exception vectors, GIC v2/v3 autodetected, PL011 UART, ARM generic timer, buddy + per-CPU magazines + SLUB allocator, hardening defaults (CFI, PAC, MTE, BTI, LSE, KASLR, stack canaries), DTB parsing. Boot to UART banner in < 500ms; the kernel hangs in a debug loop with timer ticks counting on UART. No processes, no devices beyond UART, no userspace yet.

Per `ROADMAP.md §4`.

## Landed chunks

| Commit SHA | What | Tests |
|---|---|---|
| (empty — phase not started) | | |

## Remaining work

(Sub-chunk plan, refined at Phase 1 entry. Tentative order:)

1. **P1-A: Toolchain + tools/run-vm.sh + boot stub.** Cross-compilation toolchain installed; `tools/run-vm.sh` produces a boot to UART; kernel ELF stub prints "Thylacine boot OK" via PL011 polled writes. Boot banner contract per TOOLING.md §10.
2. **P1-B: DTB parsing.** `lib/dtb.c` extracts memory regions, GIC base, UART base, timer IRQ, KASLR seed. No libfdt; minimal hand-rolled parser.
3. **P1-C: MMU + KASLR.** `arch/arm64/mmu.c` enables MMU with TTBR0 (identity) + TTBR1 (kernel high half); KASLR offset applied; relocations processed. `_Static_assert` on PTE bit layout for W^X.
4. **P1-D: Physical allocator (buddy + magazines).** `mm/buddy.c` + `mm/magazines.c` per `ARCHITECTURE.md §6.3`.
5. **P1-E: Kernel object allocator (SLUB).** `mm/slub.c` with standard `kmalloc-N` caches.
6. **P1-F: GIC + exception vectors.** `arch/arm64/gic.c` + `arch/arm64/vectors.S` + `arch/arm64/exception.c`.
7. **P1-G: ARM generic timer.** `arch/arm64/timer.c`. Timer IRQ counts ticks on UART (no scheduler yet).
8. **P1-H: Hardening enablement.** All compile flags from `ARCHITECTURE.md §24.2`. Runtime PAC + BTI + MTE + LSE detection. ELF loader rejects RWX (placeholder; full loader in Phase 2). KASLR randomization verified across 10 boots.
9. **P1-I: Phase 1 exit verification.** All `ROADMAP.md §4.2` exit criteria met.

## Exit criteria status

(Copy from `ROADMAP.md §4.2`; tick as deliverables complete.)

- [ ] QEMU `virt` ARM64 boots to a UART banner without crashing.
- [ ] Boot to UART banner: < 500ms.
- [ ] `kmalloc`/`kfree` round-trip 10,000 allocations without leak.
- [ ] GIC initialized; timer IRQ fires at 1000 Hz (verified via UART counter).
- [ ] MMU on; kernel VA map correct (read/write kernel data, no fault).
- [ ] KASLR: kernel base address differs across boots (verified across 10 boots).
- [ ] LSE atomic ops verified via runtime detection; LL/SC fallback works.
- [ ] PAC return-address signing verified (forge a return address; expect kernel panic with PAC-mismatch info).
- [ ] BTI enabled (deliberate indirect branch to non-BTI target panics cleanly).
- [ ] MTE enabled where supported (deliberate UAF detected by MTE).
- [ ] Sanitizer build runs without false positives on boot path.
- [ ] No P0/P1 audit findings on the boot path.

## Build + verify commands

(Filled in during P1-A when the build system lands. Tentative:)

```bash
# Build
tools/build.sh kernel
# Or: make kernel

# Run (interactive UART)
tools/run-vm.sh

# Run + GDB attach
tools/run-vm.sh --gdb

# Test (boot + verify banner via UART pattern match)
tools/test.sh
```

## Trip hazards

- **Boot timing budget (500ms)** is tight. Profile each boot subsystem; keep the budget split per-subsystem.
- **DTB parsing edge cases**: QEMU's DTB is well-formed; real hardware (Pi 5, post-v1.0) is not. v1.0 doesn't see Pi 5; risk is low.
- **GIC v2 vs v3 autodetection**: QEMU `virt` defaults to GICv2 on older versions, GICv3 on newer. Test both.
- **MMU enablement sequence**: order matters — TTBR registers, TCR, MAIR, then SCTLR.M=1. Get this wrong and the kernel crashes silently or in unexpected ways.
- **KASLR seed entropy**: Phase 1 reads from `/chosen/kaslr-seed` in DTB. UEFI provides high-entropy seed; missing seed falls back to a weak boot-counter source with a logged warning.
- **Hardening flag interactions**: `-fsanitize=cfi` + ThinLTO + custom linker scripts can interact unexpectedly. Phase 1 catches these; subsequent phases inherit a working build.
- **MTE performance**: measure during P1-H; if > 15% overhead on critical paths, restrict to allocations only (per `ARCHITECTURE.md §24.3`).
- **Boot banner contract**: the exact string `Thylacine boot OK` is tooling ABI per `TOOLING.md §10`. Don't change without coordinating.

## Known deltas from ARCH

(Empty at Phase 1 entry; appended as implementation surfaces gaps.)

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
