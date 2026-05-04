# Handoff 003 — Phase 1 mid-late, post-P1-G (the kernel breathes)

**Date**: 2026-05-04
**Tip commit**: `39eafb4` (P1-G + audit R1 close)
**Author**: Claude Opus 4.7 (1M context)
**Audience**: the next Claude Code session (or human collaborator) picking this up.
**Predecessors**: `docs/handoffs/002-phase1-mid-late.md` (post-test-harness state; P1-G queued at the time), `docs/handoffs/001-phase1-mid.md` (P1-C close).

This is the third formal handoff. It covers the substantial P1-G chunk — GIC v3 + ARM generic timer + IRQ vector wiring — plus the round-1 adversarial audit closure that landed in the same commit. The kernel transitions from "boots and prints once" to "boots and runs forever, observing time." A fresh session should read this top-to-bottom; everything in handoff 002 still applies for design intent and project-wide policies, but the implementation state and audit posture have moved.

---

## TL;DR

- **Phase 1 momentum**: P1-A through P1-G all landed. The kernel is now interrupt-driven; the IRQ vector slot at offset `0x280` dispatches via `gic_acknowledge → gic_dispatch → gic_eoi` and resumes via the shared `.Lexception_return` trampoline.
- **Invariants live**: I-12 (W^X), I-15 (DTB-driven HW discovery — extended at P1-G to GIC + timer), I-16 (KASLR).
- **Kernel posture**: 2048 MiB total, 2022 MiB free, 26240 KiB reserved. `tests: 6/6 PASS`, `ticks: 9 (kernel breathing)`. ELF ~190 KB; flat binary 25 KB.
- **First adversarial audit closed**: round 1 prosecuted P1-G; 4 P1 + 4 P2 + 3 P3 findings closed (1 withdrawn after trace). All fixes folded into the substantive commit.
- **Next chunk: P1-H** (hardening flag enablement: CFI, PAC, MTE, BTI, LSE, stack canaries) per ARCH §24.2.

---

## Current state of the world

### Build

- Toolchain unchanged from P1-G: clang 22 + ld.lld 22 + cmake 4 + qemu 10.
- Build flags unchanged. P1-H will append CFI / PAC / BTI / canaries / LSE flags.
- Build invocation: `tools/build.sh kernel` or `make kernel`. ELF is ~190 KB debug; flat binary ~25 KB.
- `THYLACINE_PHASE = "P1-G"` in `CMakeLists.txt`. Bump to `P1-H` when P1-H lands.

### What runs

```
$ make test
==> PASS: boot banner observed.
Thylacine v0.1.0-dev booting...
  arch: arm64
  el-entry: EL1 (direct)
  cpus: 1 (P1-C-extras; SMP at P1-F)
  mem:  2048 MiB at 0x0000000040000000
  dtb:  0x0000000048000000 (parsed)
  uart: 0x0000000009000000 (DTB-driven)
  hardening: MMU+W^X+extinction+KASLR+vectors+IRQ (P1-G; PAC/MTE/CFI at P1-H)
  kernel base: 0xffffa00... (KASLR offset 0x..., seed: DTB /chosen/kaslr-seed)
  ram: 2048 MiB total, 2022 MiB free, 26240 KiB reserved (kernel + struct_page + DTB)
  gic:  v3 dist=0x0000000008000000 redist=0x00000000080a0000
  timer: 1000000 kHz freq, 1000 Hz tick (PPI 14 / INTID 30)
  tests:
    [test] kaslr.mix64_avalanche ... PASS
    [test] dtb.chosen_kaslr_seed_present ... PASS
    [test] phys.alloc_smoke ... PASS
    [test] slub.kmem_smoke ... PASS
    [test] gic.init_smoke ... PASS
    [test] timer.tick_increments ... PASS
  tests: 6/6 PASS
  ticks: 9 (kernel breathing)
  phase: P1-G
Thylacine boot OK
```

5+ verified distinct KASLR offsets across consecutive boots; 6/6 PASS each; tick counter consistently observes 9 ticks across the boot timeline.

### Layout on disk (additions since handoff 002)

```
arch/arm64/
├── gic.h, gic.c              # GICv3 driver (P1-G)
├── timer.h, timer.c          # ARM generic timer @ 1000 Hz (P1-G)
├── exception.{h,c}           # +exception_irq_curr_el (P1-G)
├── vectors.S                 # IRQ slot live; new .Lexception_return trampoline (P1-G)
├── mmu.{h,c}                 # +mmu_map_device (P1-G)

lib/
└── dtb.c                     # +dtb_get_compat_reg_n, dtb_has_compat (P1-G)

kernel/include/thylacine/
└── dtb.h                     # +dtb_get_compat_reg_n, dtb_has_compat (P1-G)

kernel/test/
├── test_gic.c                # gic.init_smoke (P1-G)
├── test_timer.c              # timer.tick_increments (P1-G)
└── test.c                    # registry extended for new tests

docs/reference/
├── 10-gic.md                 # GIC v3 driver (P1-G; new)
├── 11-timer.md               # ARM generic timer (P1-G; new)
├── 01-boot.md                # entry sequence updated through step 17
├── 02-dtb.md                 # multi-region API + has_compat
├── 03-mmu.md                 # +mmu_map_device API
└── 08-exception.md           # IRQ slot live; .Lexception_return trampoline

docs/
├── REFERENCE.md              # snapshot bumped (6/6 tests, ~3565 LOC, ~190 KB ELF)
└── phase1-status.md          # P1-G row + remaining work + audit-trigger surfaces

memory/ (auto-loaded in next session)
└── audit_r1_closed_list.md   # closed findings preamble (NEW)
```

---

## Read order for the next session

1. `CLAUDE.md` — operational framework. Mandatory.
2. **This document** — third comprehensive handoff at post-P1-G state.
3. `docs/handoffs/002-phase1-mid-late.md` — second handoff at post-test-harness state. Useful for P1-G design context that didn't change since P1-G landed.
4. `docs/handoffs/001-phase1-mid.md` — first handoff at P1-C close. Project-wide context.
5. `docs/VISION.md` §1 (mission), §3 (properties).
6. `docs/ARCHITECTURE.md` §6 (memory), §12 (interrupt handling — P1-G's design intent), §24 (hardening — P1-H's design intent), §28 (invariants).
7. `docs/ROADMAP.md` §4 (Phase 1).
8. `docs/TOOLING.md` §3 (run-vm.sh), §10 (boot banner / EXTINCTION ABI).
9. `docs/phase1-status.md` — landed-chunks table goes through `39eafb4`. Remaining work: P1-H + P1-I.
10. The relevant subsystem reference under `docs/reference/` for whatever you're touching:
    - **P1-H**: `03-mmu.md` (W^X already in place), `06-allocator.md` (canary integration), `01-boot.md` (PAC + BTI runtime detection slot).
    - **P1-I**: `09-test-harness.md` (host-side target lands here).

When in doubt, scripture wins. Per CLAUDE.md "Reference documentation discipline": if scripture, technical reference, code, and user reference disagree, the order of authority is spec → technical reference → code → user reference, with `ARCHITECTURE.md` as the authoritative design-intent source.

---

## What landed in P1-G (since handoff 002)

### `39eafb4` — P1-G: GIC v3 + ARM generic timer + IRQ vector wiring (+ audit R1 close)

**Driver code** (~545 LOC C + ~10 LOC ASM):

- `arch/arm64/gic.{h,c}`: GICv3 driver. DTB-driven autodetect (probes `arm,gic-v3` first, then v2 candidates → extinction with deferred diagnostic). Distributor init (`GICD_CTLR = ARE_NS` BEFORE IROUTER, then group enable last; per audit F3), redistributor init (bounded `GICR_WAKER.ChildrenAsleep` poll with cntpct deadline per F4; pending + active state cleared per F12), system-register CPU interface (ICC_SRE/PMR/BPR/CTLR/IGRPEN1). IRQ enable/disable for SGI/PPI (banked) + SPI (non-banked). `gic_acknowledge` + `gic_eoi` via system registers; spurious INTIDs (1020..1023) filtered. Handler dispatch table (1020 INTIDs × 16 bytes = 16320 bytes BSS); rejects NULL handler at attach (per F11). IPRIORITYR uses byte stores per IHI 0069 §12.9.16 (per F9).
- `arch/arm64/timer.{h,c}`: ARM generic EL1 non-secure phys timer at 1000 Hz on PPI 14 (INTID 30). `CNTP_TVAL_EL0` reload pattern; `g_ticks` is `volatile u64` (per F6). Reload bounded `[100, INT32_MAX]` per F7 (CNTP_TVAL is 32-bit signed per ARMv8 ARM D11.2.4). `timer_busy_wait_ticks` is a WFI-loop wait used by `boot_main` for tick observation.

**Plumbing** (~150 LOC across 4 files):

- `arch/arm64/exception.{h,c}`: `exception_irq_curr_el(ctx)` calls `gic_acknowledge → gic_dispatch → gic_eoi`. Spurious INTIDs (`>= GIC_NUM_INTIDS == 1020`) skip dispatch and EOI per IHI 0069 §3.7.
- `arch/arm64/vectors.S`: IRQ slot at `0x280` repointed to `KERNEL_ENTRY + bl exception_irq_curr_el + b .Lexception_return`. New `.Lexception_return` shared trampoline factors `KERNEL_EXIT` out of vector slots so they fit in 0x80 bytes. **Local label** (per F10 — keeps it out of the symbol table to prevent privilege-laundering ERET gadget).
- `arch/arm64/mmu.{h,c}`: `mmu_map_device(pa, size)` for post-MMU MMIO mapping. Full break-before-make per ARM ARM B2.7.1 (per F2): `dc civac` cache clean+invalidate over the region, then L2 invalidate + TLB flush + isb, then Device descriptor write + dsb_ishst + isb. Constraint: `pa + size ≤ 4 GiB` (TTBR0 identity coverage at v1.0).
- `lib/dtb.c`, `kernel/include/thylacine/dtb.h`: `dtb_get_compat_reg_n(compat, idx, base, size)` for multi-region reg properties (GICv3 has dist + redist as reg[0] + reg[1]); idx bounded ≤ 64 per F8. `dtb_has_compat` for cheap version-detection probes.

**boot_main wiring** (~50 LOC):

- `kernel/main.c`: `gic_init() → timer_init(1000) → gic_attach(INTID 30, timer_irq_handler, NULL) → gic_enable_irq(INTID 30) → msr daifclr, #2 → timer_busy_wait_ticks(5)`. Banner adds `gic:`, `timer:`, `ticks:` lines and `+IRQ` to hardening.

**Tests** (~50 LOC):

- `kernel/test/test_gic.c`: `gic.init_smoke` — verifies `gic_version() == V3` and dist + redist bases populated.
- `kernel/test/test_timer.c`: `timer.tick_increments` — end-to-end IRQ delivery smoke (2-tick busy-wait observed).
- `kernel/test/test.c`: registry extended.

**Build**:

- `CMakeLists.txt`: `THYLACINE_PHASE = "P1-G"`.
- `kernel/CMakeLists.txt`: KERNEL_SRCS extended with gic.c, timer.c, test_gic.c, test_timer.c.

**Reference docs**:

- `docs/reference/10-gic.md` (new), `docs/reference/11-timer.md` (new).
- `docs/reference/{01-boot,02-dtb,03-mmu,08-exception}.md` updated.
- `docs/REFERENCE.md` snapshot bumped.
- `docs/phase1-status.md` P1-G row + audit-trigger surfaces.

**Audit R1 closure** (in the same commit):

| # | Sev | Closure |
|---|---|---|
| F1 | (withdrawn) | mmu_map_device OOB on 4 GiB boundary — bound check correct after trace |
| F2 | P1 | mmu_map_device cache discipline — added `dc civac` BBM step |
| F3 | P1 | dist_init IROUTER ordering — ARE_NS now set before IROUTER loop |
| F4 | P1 | GICR_WAKER timeout — bounded poll with cntpct deadline (100 ms) |
| F5 | P1 (closed by F12) | Stale SGI from kexec — F12 fix removes precondition; kexec out of v1.0 scope |
| F6 | P1 | g_ticks volatile — applied |
| F7 | P2 | Timer reload range — bounded `[100, INT32_MAX]` |
| F8 | P2 | dtb_get_compat_reg_n idx overflow — bounded idx ≤ 64 |
| F9 | P2 | IPRIORITYR RMW non-atomic — switched to byte stores |
| F10 | P2 | _exception_return privilege gadget — renamed to local `.Lexception_return` |
| F11 | P3 | gic_attach NULL handler — rejected |
| F12 | P3 | Pending SGI/PPI not cleared — GICR_ICPENDR0 + ICACTIVER0 cleared at bring-up |
| F13 | P3 | GIC_PPI_TO_INTID macro contract assert — added |

Closed list at `memory/audit_r1_closed_list.md` for round-2 preamble.

---

## Trip hazards — durable, learned-from-experience

(Cumulative since handoff 002. Items 1-15 carry over; items 16-22 are new at P1-G.)

### Carried from handoff 002

1. Compiler fusion + Device memory alignment (volatile in `be32_load`).
2. QEMU `-kernel` ELF needs Linux image header for DTB delivery.
3. W^X invariant encoded in PTE constructors.
4. EXTINCTION: prefix is kernel ABI.
5. `volatile` on `g_kernel_pa_*` in kaslr.c is load-bearing.
6. `KASLR_LINK_VA` lives in two places (kaslr.h + kernel.ld).
7. TTBR0 stays active post-KASLR for low-PA access.
8. `l3_kernel` is shared between TTBR0 and TTBR1.
9. Magazine residency requires `magazines_drain_all` for exact accounting.
10. `kpage_alloc` returns PA-cast-to-`void*` at v1.0.
11. Recursive fault on stack overflow (per-CPU exception stack at Phase 2).
12. struct page is 48 bytes since P1-E.
13. `kmem_cache_destroy` doesn't audit live objects.
14. No double-free detection in SLUB.
15. CMake cache for `THYLACINE_PHASE` requires clean rebuild after change.

### NEW at P1-G

16. **`mmu_map_device` requires the assumption "no caller has touched the region under the prior cacheable mapping"**. P1-G's cache clean-invalidate (`dc civac` sweep before BBM) handles dirty data only if the cache lines are still resident; if some unrelated code path consumed those lines and evicted dirty data via writeback, the writeback could land after we flip to Device. Documented in `arch/arm64/mmu.c` `mmu_map_device` comment. v1.0 callers are device drivers running before any access through the prior mapping.
17. **GICv3 only at v1.0**; v2 detection extincts cleanly. ARCH §12 commits to autodetect — autodetect mechanism is in place but the v2 path is deferred until there's a Pi 4 testbed. Don't silently implement v2; if a v2 platform appears, the failure mode is loud.
18. **Pi 5 GIC at PA > 4 GiB** is not supported at P1-G. `mmu_map_device` rejects PA + size > 4 GiB. Pi 5 port plan: extend TTBR0 or map GIC into TTBR1 high VA. Defer.
19. **`g_dist_base`, `g_redist_base` are non-volatile static storage** in gic.c. They're written once by `gic_init` and read by the public API + dispatch. The kaslr.c `volatile` clang-fold gotcha doesn't apply here because gic_init returns a value (the boolean) and the read sites are in different functions; clang doesn't have a SCC to fold. **But**: if a future change moves gic_init's body into an inline function in the same TU as a reader, the gotcha could re-emerge. Watch for it.
20. **`.Lexception_return` is a LOCAL label** (per F10 audit fix). Don't make it global without reverting the privilege-gadget mitigation. Phase 2 recoverable sync handlers should branch to `.Lexception_return` from within `vectors.S` — same TU, no global symbol needed.
21. **Timer reload range is `[100, INT32_MAX]`** (per F7 audit fix). 1000 Hz on a 1 GHz counter gives reload = 1 000 000 — well within range. If a future caller passes hz that produces reload < 100 (e.g., hz > freq/100), `timer_init` returns false. Don't bypass the bounds.
22. **GIC handler table is 16 KiB BSS** (1020 INTIDs × 16 bytes). Future SMP optimization may split into per-CPU SGI/PPI tables (32 INTIDs × NCPUs) + shared SPI table; v1.0 keeps it simple. The table is `static` in gic.c — accessed only via the public API.

---

## Naming conventions established

- **Kernel panic = "extinction"**. Function: `extinction(msg)`. Prefix: `EXTINCTION:`.
- **Project motto: "The thylacine runs again."** P1-G earns the motto for the "kernel breathes" milestone, but reserve it for end-of-phase / release. Don't wear out.
- **Test names use dotted notation**: `<subsystem>.<test_name>`.
- **`.Lexception_return`**: local label (not exposed in symbol table). Established at P1-G for the IRQ trampoline to avoid privilege-gadget exposure.
- **Held for explicit signoff before applying**:
  - `_hang` → `_torpor`.
  - Audit prosecutor agent name → stays "prosecutor" for Stratum continuity.

The user said "don't be shy suggesting more fitting names along the way." A v1.0+ candidate to consider: `gic_dispatch` could become something more thematic, but "dispatch" is the right word and a thematic alternative would obscure the API. Hold.

---

## What's next

### Decision tree for the next chunk

**Option A (recommended) — P1-H: hardening flag enablement**.

Sub-deliverables per ARCH §24.2:

- `cmake/Toolchain-aarch64-thylacine.cmake` — append `-fsanitize=cfi -flto=thin` (CFI over LTO), `-mbranch-protection=pac-ret+leaf` (PAC return-address signing), `-mbranch-protection=bti` (BTI on indirect branches), `-fstack-protector-strong` (stack canaries), `-march=armv8-a+lse` (LSE atomics).
- Runtime detection in `arch/arm64/start.S` (post-EL-drop): read `ID_AA64ISAR1_EL1.{APA,API,GPA,GPI}` for PAC + BTI feature bits; fall back gracefully if absent.
- `arch/arm64/atomic.S` (new) — LSE atomic primitives (`ldadd`, `swp`, `cas`) used by Phase 2 spinlocks. Runtime patching from LL/SC fallback if `ID_AA64ISAR0_EL1.Atomic == 0`.
- ELF loader stub — placeholder (full loader at Phase 2). P1-H asserts "no RWX segments" at link time via linker-script ASSERTs.
- MTE: `-march=armv8.5-a+memtag` if compiler supports + runtime detection in start.S. Restrict to allocations only if overhead > 15% per ARCH §24.3.
- Banner: hardening line bumped to `MMU+W^X+extinction+KASLR+vectors+IRQ+CFI+PAC+BTI+canaries+LSE` (or whatever subset is live).
- Tests: deliberate-PAC-mismatch (forge return; expect kernel panic with PAC-mismatch); deliberate-BTI (indirect branch to non-BTI target); LSE detection runtime check.
- Estimated 700-1000 LOC + reference doc updates; 150-200k tokens.

**Option B — CI infrastructure**: GitHub Actions workflow for `make test` + `make test --asan` + `make test --ubsan`. ~30k tokens. Independent of phase order; could land alongside any chunk.

**Option C — First TLA+ spec sketch**: e.g., `mmu.tla` for the page-table walker. Optional at P1; mandatory from Phase 2 onward.

**Recommendation**: A. P1-H closes the v1.0 hardening commitment from `docs/VISION.md §3` and is a prerequisite for the P1-I closing audit (sanitizer matrix wants a clean P1-H build).

### Sub-chunks (after P1-H)

- **P1-I** — Phase 1 exit verification. ASan + UBSan instrumented builds. 10000-iteration alloc/free leak check. Host-side test target for sanitizer matrix runs. CI workflow. Phase 1 closing audit pass.

After P1-I, **Phase 2** begins: process model + EEVDF scheduler + handles + VMO + `scheduler.tla` / `namespace.tla` / `handles.tla` / `vmo.tla` formal specs.

---

## Operational notes

- **Autonomy level**: under "auto" mode the user grants autonomy on implementation, testing, formal modeling, audit triage, commit. Always escalate format breaks, destructive operations, ARCH deviations, scope pivots, anything visible to others.
- **Pre-Utopia (Phases 1-4)**: human-primary, agent-assisted. Kernel **extinction** = stop, report, do not proceed without review.
- **Post-Utopia (Phases 5-8)**: agent-primary, human-directed.
- **Stratum repo**: at `~/projects/stratum/`. Phase 9 (9P server) is the integration target for Thylacine Phase 4.
- **TLA+ tools** at `/tmp/tla2tools.jar`.
- **Audit policy**: per CLAUDE.md, exception entry / GIC + IRQ entry / timer / multi-region DTB / MMIO mapping are all audit-trigger surfaces. P1-H + P1-I will trigger fresh audits before merge — load `memory/audit_r1_closed_list.md` as the do-not-re-report preamble.

---

## Quick-reference commands

```bash
cd ~/projects/thylacine

# Build
tools/build.sh kernel
make kernel

# Run interactively (Ctrl-A x to quit QEMU)
tools/run-vm.sh

# Boot test
tools/test.sh
make test

# 5-boot KASLR + tests verification
for i in $(seq 1 5); do
  tools/test.sh 2>&1 | grep -E "kernel base|tests:|ticks:|^Thylacine boot OK"
done

# Inspect kernel ELF
/opt/homebrew/opt/llvm/bin/llvm-readelf -SW build/kernel/thylacine.elf
/opt/homebrew/opt/llvm/bin/llvm-readelf -r build/kernel/thylacine.elf

# Verify .Lexception_return is NOT exported (P1-G F10 audit fix)
/opt/homebrew/opt/llvm/bin/llvm-readelf --syms build/kernel/thylacine.elf | grep exception_return
# Should return 0 lines

# Verify volatile gotchas (g_kernel_pa_* should be 8 bytes; not 1)
/opt/homebrew/opt/llvm/bin/llvm-readelf --syms build/kernel/thylacine.elf | grep g_kernel_pa

# Run TLA+ specs (none yet — first lands Phase 2)
cd specs && for s in *.tla; do
  java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock \
    -config "${s%.tla}.cfg" "$s" 2>&1 | tail -3
done
```

---

## Posture summary

| Metric | Value |
|---|---|
| Tip commit | `39eafb4` (P1-G + audit R1 close) |
| Phase | Phase 1 — P1-A through P1-G complete |
| Next chunk | P1-H (hardening flags) |
| Build matrix | default Debug — green |
| `tools/test.sh` | PASS |
| In-kernel tests | 6/6 PASS (kaslr.mix64_avalanche, dtb.chosen_kaslr_seed_present, phys.alloc_smoke, slub.kmem_smoke, gic.init_smoke, timer.tick_increments) |
| Specs | 0/9 (Phase 2 introduces first spec — `scheduler.tla`) |
| LOC | ~2750 C99 + ~370 ASM + ~75 LD + ~220 sh/cmake + ~150 test = ~3565 |
| Kernel ELF | ~190 KB debug |
| Kernel flat binary | ~25 KB |
| Page tables | 40 KiB BSS |
| struct page array | 24 MiB BSS |
| GIC handler table | 16 KiB BSS |
| Boot banner reserved | ~26 MiB |
| RAM free at boot | ~2022 MiB / 2048 MiB |
| KASLR offsets verified distinct | 5+ (sample) |
| Tick counter at end of boot | 9 |
| Open audit findings | 0 (round 1 closed at the chunk landing) |
| Open deferred items | P1-H (hardening), P1-I (verification + sanitizer + CI) |

---

## Stratum coordination

Stratum is feature-complete on Phases 1-7 of its own roadmap. Phase 8 (POSIX surface) is in progress. Phase 9 (9P server + Stratum extensions) is Thylacine Phase 4's integration target. Coordinate Phase 9 timeline with the Stratum project at `~/projects/stratum/`. Thylacine Phases 1-3 (where we are now) proceed in parallel with Stratum's Phase 8-9 work — no dependency.

---

## Format ABI surfaces in flight

These are stable contracts that next-session work must not break without coordinated multi-document updates. Additions since handoff 002 marked NEW.

| Surface | Where | Contract |
|---|---|---|
| Boot banner success line | `kernel/main.c` | `"Thylacine boot OK\n"` |
| Extinction prefix | `kernel/extinction.c` | `"EXTINCTION: "` |
| el-entry banner line | `kernel/main.c` | `"el-entry: EL1 (direct)"` / `"EL2 -> EL1 (dropped)"` |
| KASLR banner line | `kernel/main.c` | `"kernel base: 0x..., KASLR offset 0x..., seed: <source>"` |
| ram banner line | `kernel/main.c` | `"ram: X MiB total, Y MiB free, Z KiB reserved"` |
| **NEW** gic banner line | `kernel/main.c` | `"  gic:  v<N> dist=0x... redist=0x..."` |
| **NEW** timer banner line | `kernel/main.c` | `"  timer: <kHz> kHz freq, 1000 Hz tick (PPI 14 / INTID 30)"` |
| tests banner block | `kernel/main.c` | `"  tests:\n    [test] <name> ... PASS\|FAIL"` per test, then `"  tests: N/N PASS\|FAIL"` |
| **NEW** ticks banner line | `kernel/main.c` | `"  ticks: N (kernel breathing)"` |
| Linux ARM64 image header | `arch/arm64/start.S` offset 0..0x40 | `b _real_start; nop; <text_offset>; <image_size>; <flags>; <res>; "ARM\x64"` |
| `_image_size` linker symbol | `arch/arm64/kernel.ld` | `_kernel_end - _kernel_start` |
| `_saved_dtb_ptr` BSS variable | `arch/arm64/start.S` | DTB physical address |
| `_entered_at_el2` BSS variable | `arch/arm64/start.S` | 0 / 1 EL-source flag |
| Linker section symbols | `arch/arm64/kernel.ld` | `_kernel_start` / `_text_end` / `_rodata_end` / `_data_end` / `_rela_start` / `_rela_end` / `_boot_stack_guard` / `_boot_stack_bottom` / `_boot_stack_top` / `_kernel_end` |
| `KERNEL_LINK_VA` constant | `arch/arm64/kernel.ld` + `arch/arm64/kaslr.h` | `0xFFFFA00000080000` (must match) |
| Page table layout (4-level, 4 KiB granule) | `arch/arm64/mmu.c` | TTBR0 identity (1 L0 + 1 L1 + 4 L2) + TTBR1 high-half (1 L0 + 1 L1 + 1 L2) + shared L3 = 40 KiB BSS |
| MAIR attribute indices | `arch/arm64/mmu.h` | 0=Device, 1=NormalNC, 2=NormalWB, 3=NormalWT |
| PTE constructor macros | `arch/arm64/mmu.h` | W^X-safe by `_Static_assert` |
| **NEW** `mmu_map_device(pa, size)` API | `arch/arm64/mmu.{h,c}` | break-before-make + cache clean+invalidate; `pa + size ≤ 4 GiB` |
| FDT format assumptions | `lib/dtb.c` | `#address-cells = 2`, `#size-cells = 2` |
| DTB consumer surface | `kernel/include/thylacine/dtb.h` | `dtb_init`, `dtb_get_memory`, `dtb_get_compat_reg`, `dtb_get_compat_reg_n` (NEW), `dtb_has_compat` (NEW), `dtb_get_total_size`, `dtb_get_chosen_kaslr_seed`, `dtb_get_chosen_rng_seed` |
| **NEW** GIC API | `arch/arm64/gic.{h,c}` | `gic_init`, `gic_version`, `gic_dist_base`, `gic_redist_base`, `gic_attach(intid, h, arg)` (rejects NULL), `gic_enable_irq`, `gic_disable_irq`, `gic_acknowledge`, `gic_eoi`, `gic_dispatch` |
| **NEW** GIC INTID conventions | `arch/arm64/gic.h` | `GIC_NUM_INTIDS=1020`, `GIC_INTID_SPURIOUS=1023`, `GIC_PPI_TO_INTID(ppi) = (ppi)+16` (asserted at compile time); 0..15 SGI, 16..31 PPI, 32..1019 SPI, 1020..1023 reserved |
| **NEW** Timer API | `arch/arm64/timer.{h,c}` | `timer_init(hz)` (reject reload outside `[100, INT32_MAX]`), `timer_irq_handler`, `timer_get_ticks` (volatile), `timer_get_counter`, `timer_get_freq`, `timer_busy_wait_ticks`, `TIMER_INTID_EL1_PHYS_NS = 30` |
| `struct page` layout | `kernel/include/thylacine/page.h` | 48 bytes |
| Page flags | `kernel/include/thylacine/page.h` | `PG_FREE` / `PG_RESERVED` / `PG_KERNEL` / `PG_SLAB` |
| Allocation flags | `kernel/include/thylacine/page.h` | `KP_ZERO` / `KP_DMA` / `KP_NOWAIT` / `KP_COMPLETE` |
| `struct exception_context` | `arch/arm64/exception.h` | 288 bytes; field offsets `_Static_assert`'d |
| **NEW** `.Lexception_return` symbol | `arch/arm64/vectors.S` | LOCAL label (not exposed); shared trampoline for recoverable exception paths |
| `struct test_case` | `kernel/test/test.h` | `name` / `fn` / `failed` / `fail_msg`; sentinel = `{NULL, NULL, false, NULL}` |

Changing any of these requires a documented rationale in the commit message + updates to `docs/reference/`.

---

## Things I would NOT recommend deviating from

- **Always use the helper macros / API**. PTE constructors, mmio_w*, gic_attach/enable, timer_busy_wait_ticks. They're load-bearing.
- **`tools/test.sh` is the canonical test gate**. Pre-commit; if it fails, don't commit. The 6 in-kernel tests run inside it.
- **Doc-update-per-PR**. If you touch `arch/arm64/gic.c`, update `docs/reference/10-gic.md` in the same commit.
- **Two commits per substantive landing** (or one well-structured commit if the chunk + audit close fit naturally). The hash-fixup pattern from handoff 002 still works for chunks where the doc snapshot needs the SHA.
- **Volatile in `be32_load`, `g_kernel_pa_*`, `g_ticks`** — load-bearing.
- **Don't add tests for evolving subsystems** — write them when the API stabilizes.
- **Test names use dotted notation** (`<subsystem>.<test_name>`).
- **`.Lexception_return` stays local**. Don't expose it without re-prosecuting F10's threat model.
- **GIC v3 only at v1.0**. Don't silently implement v2 without a test target.

---

## Open questions / future-work tags

Tracked here so they don't get lost across sessions:

- **U-1** (carried, P1-F): Should `extinction()` print a register dump? Could land at P1-H.
- **U-2** (carried): Kernel `dmesg` log buffer. Phase 2.
- **U-3** (resolved at P1-C-extras Part B): KASLR seed entropy.
- **U-4** (carried): `mmu.tla` formal spec sketch.
- **U-5**: Hardened malloc / Scudo for userspace heap. Phase 5 (musl port).
- **U-6** (carried, P1-F): Per-CPU exception stack. Phase 2.
- **U-7** (carried, P1-E): Multi-page slabs (`slab_order > 0`).
- **U-8** (carried, P1-E): SLUB debug mode (red zones, double-free).
- **U-9** (carried, P1-F): Host-side test target. P1-I.
- **U-10** (NEW, P1-G): GICv2 driver path. Lands when there's a Pi 4 testbed.
- **U-11** (NEW, P1-G): Pi 5 GIC at PA > 4 GiB. TTBR0 extension or high-VA mapping.
- **U-12** (NEW, P1-G): SMP redistributor walk for secondary CPUs. Phase 2 with thread machinery.
- **U-13** (NEW, P1-G): SGI (IPI) generation via ICC_SGI1R_EL1. Phase 2 scheduler.
- **U-14** (NEW, P1-G): IRQ-driven UART TX. Mechanism in place; routing via `gic_attach` is post-v1.0.
- **U-15** (NEW, P1-G): Per-IRQ priority hierarchy (currently `0xa0` for everything). Post-v1.0.

---

## How to write the next handoff

Following the pattern of 001, 002, and 003: number sequentially, write at every milestone (P1-H close, P1-I exit, Phase 2 entry, etc.). Sections:

- TL;DR
- Current state of the world
- Read order
- What landed since the previous handoff
- Trip hazards (durable; cumulative)
- Naming conventions established
- What's next + decision tree
- Quick-reference commands
- Posture summary
- Stratum coordination status
- Format ABI surfaces in flight
- Things I would NOT recommend deviating from
- Open questions / future-work tags

The handoff thread is the project's continuity story.

---

## Sign-off

The thylacine runs again — **and now it breathes**. 🐅

(Project motto, used per `memory/project_motto.md` for milestone moments. P1-G earns the breathing milestone — the kernel transitions from "boots and prints once" to "boots, observes time, runs forever." Phase 1 has nine landed chunks; the kernel boots, allocates, faults cleanly, dispatches IRQs, and tests itself. P1-H closes the v1.0 hardening commitment; P1-I closes Phase 1 and the road to Phase 2 opens.)
