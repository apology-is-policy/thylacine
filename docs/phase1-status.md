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
| 6462227 | **P1-C**: MMU enable with W^X invariant I-12 + extinction (kernel ELE) infra. `arch/arm64/mmu.{h,c}` (~300 LOC) builds 4-level page tables (1 L0 + 1 L1 + 4 L2 + 1 L3 = 28 KiB BSS), identity-maps low 4 GiB, programs MAIR + TCR + TTBR + SCTLR. Per-section permissions (.text RX, .rodata R, .data/.bss RW) via L3 page-grain mappings; PL011 region as Device-nGnRnE block. **W^X invariant I-12 encoded in PTE constructors with `_Static_assert`**. `kernel/extinction.{c}` + `kernel/include/thylacine/extinction.h` (~50 LOC) provide `extinction(msg)`, `extinction_with_addr(msg, addr)`, `ASSERT_OR_DIE(expr, msg)`. **Kernel panic prefix renamed to `EXTINCTION:`** (thematic — thylacine extinction event); TOOLING.md §10 + CLAUDE.md + tools/test.sh updated for the rename. Banner shows `hardening: MMU+W^X+extinction (P1-C; KASLR/PAC/MTE/CFI at later sub-chunks)`. **Deferred to P1-C-extras**: KASLR (kernel image relocation into TTBR1 high half via PIE relocations + `dtb_get_chosen_kaslr_seed`), TTBR1 high-half mappings beyond stub, boot stack guard page, EL2→EL1 drop diagnostic. | `tools/test.sh` PASS. |
| ff22ca3 | **P1-C-extras Part A**: EL2 → EL1 drop sequence + boot-stack guard page. `arch/arm64/start.S` now branches to a canonical drop sequence (HCR_EL2.RW=1, CNTHCTL_EL2 timer access, CNTVOFF_EL2=0, VTTBR_EL2=0, SCTLR_EL1=`INIT_SCTLR_EL1_MMU_OFF` (`0x30D00800`), SPSR_EL2=EL1h+DAIF, ELR_EL2=`.Lpost_el2_drop`, eret) when entered at EL2; QEMU virt continues to enter at EL1. New BSS variable `_entered_at_el2` surfaces the diagnostic via the boot banner: `el-entry: EL1 (direct)` or `el-entry: EL2 -> EL1 (dropped)`. EL3 / EL0 entry halts silently (no UART without DTB-discovered base). `arch/arm64/kernel.ld` reserves a 4 KiB BSS slot at `_boot_stack_guard` immediately below `_boot_stack_bottom`; `arch/arm64/mmu.c` zeroes the corresponding L3 PTE so any access faults synchronously. Stack-overflow extinction (`extinction("kernel stack overflow", FAR_EL1)`) gets wired in P1-F when fault handlers land. **KASLR remains deferred to P1-C-extras Part B**. | `tools/test.sh` PASS. Banner now includes `el-entry: EL1 (direct)`. |
| 74fd391 | **P1-C-extras Part B**: KASLR (invariant I-16). Toolchain flipped to `-fpie -fdirect-access-external-data -mcmodel=tiny`; linker to `-Wl,-pie -Wl,-z,text -Wl,-z,norelro -Wl,-z,nopack-relative-relocs -Wl,--no-dynamic-linker`. `arch/arm64/kernel.ld` links at `KERNEL_LINK_VA = 0xFFFFA00000080000` with `AT(KERNEL_LOAD_PA = 0x40080000)`; emits `.rela.dyn`. New `arch/arm64/kaslr.{h,c}` (~150 LOC): tries `/chosen/kaslr-seed` → `/chosen/rng-seed` → `cntpct_el0` for entropy; SipHash-style mix function spreads bits; produces 13-bit (8192-bucket) 2 MiB-aligned offset in `[0, 16 GiB)`; walks `.rela.dyn` for R_AARCH64_RELATIVE entries (currently 0 thanks to `-fdirect-access-external-data`). `arch/arm64/mmu.c` builds TTBR1 mapping at `KASLR_LINK_VA + slide` using new `l0_ttbr1` / `l1_ttbr1` / `l2_ttbr1` BSS tables and the SHARED `l3_kernel`; `mmu_enable(u64 slide)` signature change. `arch/arm64/start.S` calls `kaslr_init` after BSS clear, passes slide to `mmu_enable`, then long-branches into TTBR1 via `kaslr_high_va_addr` + `br x0`. `lib/dtb.c` split `dtb_get_chosen_kaslr_seed` (kaslr-seed only) from new `dtb_get_chosen_rng_seed` (rng-seed only). Boot banner shows `kernel base: 0x...`, `KASLR offset 0x...`, `seed: <source>` (varies per boot). Page tables grew from 28 KiB to 40 KiB. | `tools/test.sh` PASS. 10 consecutive boots produced 10 distinct KASLR offsets (ranging from 0x44800000 to 0x3d0000000, all 2 MiB-aligned, all < 16 GiB). |
| 198c48c | **P1-D**: Physical allocator (buddy + per-CPU magazines + DTB-driven bootstrap). New `mm/buddy.{h,c}` (~280 LOC): one zone, sentinel-headed doubly-linked free lists at orders 0..18 (4 KiB to 1 GiB), buddy-pfn merge math. New `mm/magazines.{h,c}` (~110 LOC): per-CPU stacks of 16 entries at orders 0 (4 KiB) and 9 (2 MiB), refill/drain to half-full, `magazines_drain_all` for accounting. New `mm/phys.{h,c}` (~170 LOC): DTB-driven bootstrap reserving kernel image + struct-page array + DTB blob + low-firmware, public `alloc_pages` / `free_pages` / `kpage_alloc` API. New `kernel/include/thylacine/page.h` (struct page 32 bytes; PG_FREE/PG_RESERVED/PG_KERNEL flags; KP_ZERO/KP_DMA/KP_NOWAIT/KP_COMPLETE) and `spinlock.h` (no-op stub at P1-D; LL/SC at P1-F; LSE at P1-H). `arch/arm64/kaslr.h`/`.c` gains `kaslr_kernel_pa_start` / `kaslr_kernel_pa_end` accessors with **`volatile`** storage to defeat a clang `-O2 -fpie -mcmodel=tiny` constant fold. `kernel/main.c` calls `phys_init` post-banner, then runs an alloc/free smoke test exercising magazine fast path + non-magazine order. Banner adds `ram: X MiB total, Y MiB free, Z KiB reserved` and `alloc smoke: PASS/FAIL`. struct page array: 16 MiB BSS for 2 GiB RAM. | `tools/test.sh` PASS. 5 consecutive boots produced 5 distinct KASLR offsets, all alloc smoke PASS. Reserved ≈18 MiB (kernel + struct_page + DTB + low firmware); free ≈2030 MiB. |
| e867e3b | **P1-E**: SLUB kernel object allocator. New `mm/slub.{h,c}` (~370 LOC) with per-cache partial slab list and per-slab embedded freelist (free objects' first 8 bytes thread the linked list — zero metadata overhead). Public API: `kmem_cache_create`/`alloc`/`free`/`destroy` + `kmalloc`/`kzalloc`/`kcalloc`/`kfree`. Standard `kmalloc-{8..2048}` caches in BSS; sizes > 2 KiB bypass slab and call `alloc_pages` directly (kfree reads the order from struct page). Bootstrap via static `g_meta_cache` for `struct kmem_cache` itself; static `g_kmalloc_caches[]` for the power-of-two set. `kernel/include/thylacine/page.h` extended struct page from 32 → 48 bytes with `slab_freelist` + `slab_cache` fields; new `PG_SLAB` flag. struct page array grew from 16 MiB to 24 MiB on 2 GiB RAM. `kernel/main.c` calls `slub_init` post-`phys_init` and runs a kmem smoke test exercising small (1500 × kmalloc-8 → 3 slab pages) + mixed sizes + 8 KiB direct path + dynamic `kmem_cache_create` round-trip. Banner adds `kmem smoke: PASS/FAIL`. | `tools/test.sh` PASS. 5 consecutive boots produced 5 distinct KASLR offsets and 5 PASS kmem smokes. Reserved ≈26 MiB (was 18 MiB at P1-D; +8 MiB for the bigger struct page array). |
| 67a6b16 | **P1-F**: Exception vector table + sync handler with stack-overflow + W^X-violation detection. New `arch/arm64/vectors.S` (~150 LOC) — 16-entry table aligned 0x800, KERNEL_ENTRY save / KERNEL_EXIT restore macros (KERNEL_EXIT defined; unused inline at P1-F because every fault terminates in extinction). New `arch/arm64/exception.{h,c}` (~250 LOC) with `struct exception_context` (288 bytes; field offsets `_Static_assert`'d), `exception_init` (sets VBAR_EL1), `exception_sync_curr_el` (decodes ESR.EC + DFSC/IFSC; routes stack-overflow / W^X / translation / alignment / brk paths to specific extinctions), `exception_unexpected` (catch-all for the 15 non-live entries with descriptive name table). `kernel/main.c` calls `exception_init` post-`slub_init`. Banner: `hardening: MMU+W^X+extinction+KASLR+vectors (P1-F; ...)`. The deferred fault paths from P1-C-extras Part A (boot-stack guard) and P1-C (W^X PTE constructors) finally close. | `tools/test.sh` PASS. 5 consecutive boots PASS all smokes. Vector table at `_exception_vectors` (page-aligned in `.text`); `exception_init` disassembly: `adr+msr vbar_el1+isb`. KERNEL_ENTRY save: 24 instructions; sync slot 28 instructions; under the 32-instruction (0x80) slot budget. **Known limitation**: handler runs on the existing SP_EL1, so a fault that overflows the boot stack recurses; per-CPU exception stack lands at Phase 2. |
| (pending) | **Test harness** (cross-cutting infrastructure addition between P1-F and P1-G). New `kernel/test/test.{h,c}` (~120 LOC) with sentinel-terminated `g_tests[]` registry, TEST_ASSERT macro, per-test PASS/FAIL reporting via UART. Four leaf-API tests at landing: `kaslr.mix64_avalanche` (avalanche property of the SipHash-style mix function exposed via new `kaslr_test_mix64`), `dtb.chosen_kaslr_seed_present` (DTB parser sanity), `phys.alloc_smoke` (refactored from boot_main's inline alloc smoke), `slub.kmem_smoke` (refactored from boot_main's inline kmem smoke). Banner replaces the two inline smoke result lines with a tests block and `tests: N/N PASS|FAIL` summary; extinction on any failure. Tests cover **stable leaf APIs only** — internal data-structure invariants of evolving subsystems are deferred to avoid rewriting tests as those subsystems grow. Per-test files under `kernel/test/test_*.c`. | `tools/test.sh` PASS. 5 boots show 4/4 PASS. Boot output now lists per-test outcomes; future host-side sanitizer matrix at P1-I. |

## Remaining work

(Sub-chunk plan, refined at Phase 1 entry. P1-A landed; tentative order for the rest:)

1. ✅ **P1-A: Toolchain + tools/run-vm.sh + boot stub.** Landed.
2. ✅ **P1-B: DTB parsing.** Landed. Linux ARM64 image header resolved the DTB-pointer-zero observation; FDT parser (~340 LOC); volatile reads prevent clang fusion into misaligned 8-byte loads.
3. ✅ **P1-C: MMU + W^X + extinction infra.** Landed. Identity map of low 4 GiB; per-section perms via L3 paging; PTE constructors `_Static_assert` W^X (I-12); `extinction()` with `EXTINCTION:` ABI prefix (thematic rename from `panic`). **Deferred to P1-C-extras** (split into Parts A and B; see below).
4. ✅ **P1-C-extras Part A: EL2 → EL1 drop + boot-stack guard page.** Landed. `arch/arm64/start.S` now performs the canonical EL2 → EL1 drop sequence when entered at EL2 (Pi 5 concern; QEMU virt always at EL1). New `_entered_at_el2` BSS flag drives an `el-entry:` banner diagnostic. `arch/arm64/kernel.ld` reserves a 4 KiB guard slot at `_boot_stack_guard` below the boot stack; `arch/arm64/mmu.c` zeroes its L3 PTE so stack overflow faults synchronously instead of silently corrupting BSS.
5. ✅ **P1-C-extras Part B: KASLR.** Landed. `-fpie -fdirect-access-external-data -mcmodel=tiny` compile + `-Wl,-pie -Wl,-z,text -Wl,-z,norelro -Wl,-z,nopack-relative-relocs -Wl,--no-dynamic-linker` link. Linker script links at `KERNEL_LINK_VA = 0xFFFFA00000080000`. `arch/arm64/kaslr.{h,c}`: entropy chain (kaslr-seed → rng-seed → cntpct fallback), mix64 spread, 13-bit 2 MiB-aligned offset in `[0, 16 GiB)`, R_AARCH64_RELATIVE walker. `arch/arm64/mmu.c` builds TTBR1 high-half mapping at `KASLR_LINK_VA + slide` using shared `l3_kernel`. `arch/arm64/start.S` calls `kaslr_init` and long-branches via `kaslr_high_va_addr` + `br x0`. Banner shows live offset and seed source. Invariant **I-16** satisfied.
6. ✅ **P1-D: Physical allocator (buddy + per-CPU magazines).** Landed. `mm/buddy.c` (~280 LOC) implements one-zone Knuth buddy with orders 0..18 and split/merge via `pfn ^ (1<<order)`. `mm/magazines.c` (~110 LOC) implements per-CPU 16-entry stacks at orders 0/9 with half-full refill/drain hysteresis. `mm/phys.c` (~170 LOC) does DTB-driven bootstrap (reserves kernel image + struct page array + DTB blob + low firmware) and exposes the public `alloc_pages` / `kpage_alloc` API. `volatile` qualifier on `g_kernel_pa_*` defeats a clang `-O2 -fpie -mcmodel=tiny` constant fold. Banner shows ram total/free/reserved + smoke-test pass.
7. ✅ **P1-E: SLUB kernel object allocator.** Landed. `mm/slub.c` (~370 LOC) implements per-cache partial slab list with embedded freelist (zero metadata overhead per free object). Standard kmalloc-{8..2048} caches; >2 KiB bypasses slab. `kmem_cache_create` / `alloc` / `free` / `destroy` for typed caches (Phase 2 will use this for proc/thread/chan/vmo/handle). Bootstrap via static meta cache. `struct page` extended to 48 bytes with `slab_freelist` + `slab_cache`; `PG_SLAB` flag. struct page array grew 16→24 MiB. Banner adds kmem smoke pass.
8. ✅ **P1-F: Exception vector table + sync handler.** Landed. `arch/arm64/vectors.S` (~150 LOC) — 16-entry table aligned 0x800, KERNEL_ENTRY save (288 bytes context), KERNEL_EXIT restore (defined; unused at P1-F). `arch/arm64/exception.{h,c}` (~250 LOC) — ESR/FAR decode, stack-overflow detection (FAR in boot-stack-guard region), W^X violation detection (kernel-image permission fault), brk / alignment / generic-extinction paths, exception_unexpected catch-all. `exception_init` sets VBAR_EL1. The deferred fault paths from P1-C-extras Part A and P1-C finally close. **Known limitation**: handler shares SP_EL1 with boot stack — recursive faults wedge; per-CPU exception stack lands at Phase 2.
5. **P1-E: Kernel object allocator (SLUB).** `mm/slub.c` with standard `kmalloc-N` caches.
6. **P1-F: GIC + exception vectors.** `arch/arm64/gic.c` + `arch/arm64/vectors.S` + `arch/arm64/exception.c`. Real UART init (CR/LCR_H/IBRD/FBRD).
7. **P1-G: ARM generic timer.** `arch/arm64/timer.c`. Timer IRQ counts ticks on UART (no scheduler yet).
8. **P1-H: Hardening enablement.** All compile flags from `ARCHITECTURE.md §24.2`. Runtime PAC + BTI + MTE + LSE detection. ELF loader rejects RWX (placeholder; full loader in Phase 2). KASLR randomization verified across 10 boots. **Removes the `FIXME(I-15)` from `uart.c`** once DTB-driven UART base is in place from P1-B.
9. **P1-I: Phase 1 exit verification.** All `ROADMAP.md §4.2` exit criteria met. ASan + UBSan instrumentation. CI workflow.

## Exit criteria status

(Copy from `ROADMAP.md §4.2`; tick as deliverables complete.)

- [x] **QEMU `virt` ARM64 boots to a UART banner without crashing.** Landed at P1-A; still passes at P1-B and P1-C.
- [ ] Boot to UART banner: < 500ms. Informally measured ~50 ms through P1-C (DTB parse adds ~150 µs; MMU enable adds ~50 µs; negligible). Rigorous measurement at P1-I.
- [x] **MMU on with kernel VA map correct** (read/write kernel data, no fault). Landed at P1-C. **W^X invariant I-12 enforced at PTE bit level via `_Static_assert` on PTE constructors**.
- [ ] `kmalloc`/`kfree` round-trip 10,000 allocations without leak. P1-E (kmalloc) / P1-I (the formal 10000-iteration check). P1-D's boot smoke test does 256 × order-0 + order-9 + order-10 alloc/free with `magazines_drain_all` for clean accounting; the result is comparable in spirit but not the formal exit criterion.
- [ ] GIC initialized; timer IRQ fires at 1000 Hz (verified via UART counter). P1-F / P1-G.
- [ ] MMU on; kernel VA map correct (read/write kernel data, no fault). P1-C.
- [x] **KASLR: kernel base address differs across boots (verified across 10 boots).** Landed at P1-C-extras Part B. Banner's `kernel base: 0x...` line varies per boot in the 0xFFFFA00*  region; 13 bits of entropy (8192 distinct 2 MiB-aligned offsets). Invariant **I-16** satisfied.
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
- ✅ ~~KASLR seed entropy~~. Resolved at P1-C-extras Part B. Tries `/chosen/kaslr-seed` (UEFI on bare metal), then `/chosen/rng-seed` (QEMU virt; both also work on UEFI), then `cntpct_el0` hardware counter as a low-entropy fallback. The chosen value passes through a SipHash-style `mix64` avalanche to spread entropy. Banner exposes which source was used so a developer can spot a `cntpct (low-entropy fallback)` boot.
- **Hardening flag interactions**: `-fsanitize=cfi` + ThinLTO + custom linker scripts can interact unexpectedly. Phase 1 catches these; subsequent phases inherit a working build. P1-H work.
- **MTE performance**: measure during P1-H; if > 15% overhead on critical paths, restrict to allocations only (per `ARCHITECTURE.md §24.3`).
- **Boot banner contract**: the exact string `Thylacine boot OK` is tooling ABI per `TOOLING.md §10`. Don't change without coordinating.
- ✅ ~~Boot stack guard page~~. Resolved at P1-C-extras Part A. 4 KiB BSS slot at `_boot_stack_guard` is mapped non-present (PTE_VALID=0) by `mmu.c`. Stack overflow now triggers a translation fault on the guard region; once P1-F's exception handler lands, the diagnostic routes to `extinction("kernel stack overflow", FAR_EL1)`.
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
| Page tables / MMU | `arch/arm64/mmu.c`, `arch/arm64/mmu.h`, `arch/arm64/kernel.ld` (`_text_end` / `_rodata_end` / `_data_end`) | W^X invariant (I-12); MMU enable; TTBR0 identity + TTBR1 high-half (KASLR-aware) | P1-C; extended P1-C-extras |
| Extinction / panic | `kernel/extinction.c`, `kernel/include/thylacine/extinction.h` | EXTINCTION: ABI per TOOLING.md §10 | P1-C |
| KASLR | `arch/arm64/kaslr.{h,c}`, `arch/arm64/kernel.ld` (`_rela_start` / `_rela_end` + `KERNEL_LINK_VA`), `arch/arm64/start.S` (long-branch into TTBR1) | Entropy + relocation correctness (I-16); seed source priority; .rela.dyn walker termination | P1-C-extras Part B |
| Physical allocator | `mm/buddy.{h,c}`, `mm/magazines.{h,c}`, `mm/phys.{h,c}`, `kernel/include/thylacine/page.h` | Allocation correctness; magazine refill/drain accounting; reservation layout; future I-7 refcount surface | P1-D |
| Kernel object allocator | `mm/slub.{h,c}`, `kernel/include/thylacine/page.h` (slab fields), `kernel/main.c` (slub_init + kmem smoke) | Slab freelist invariant; partial-list discipline; meta-cache bootstrap; double-free safety (P1-I tightens) | P1-E |
| Exception entry | `arch/arm64/vectors.S`, `arch/arm64/exception.{h,c}`, `kernel/main.c` (exception_init) | Vector-table layout; KERNEL_ENTRY save correctness; sync dispatch routing; FAR/ESR decode; W^X enforcement at runtime | P1-F |
| LSE detection | `arch/arm64/atomic.S` (P1-H) | Runtime patching correctness | (planned) |

Audit at Phase 1 exit (per `ROADMAP.md §4.2`): no P0/P1 findings on the boot path.

## Specs landing this phase

None mandatory at Phase 1. Optional: a sketch of `mmu.tla` for page table validity, but not gating.

## Performance budget contribution at this phase

- Boot to UART banner: < 500ms (full budget).
- `kmalloc(small)` p99: < 50ns (uncontested).
- Allocator scaling: linear with core count for refill operations.

(Measured at Phase 1 exit; carried forward as the floor for subsequent phases.)
