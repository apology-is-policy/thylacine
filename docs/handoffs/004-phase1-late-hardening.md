# Handoff 004 — Phase 1 late, post-P1-H (hardening commitment closed)

**Date**: 2026-05-04
**Tip commit**: `e8c9c5c` (P1-H + audit R2 close), with hash-fixup commit pending in this session
**Author**: Claude Opus 4.7 (1M context)
**Audience**: the next session (or human collaborator) picking this up.
**Predecessors**: `003-phase1-mid-late-irq.md` (post-P1-G), `002-phase1-mid-late.md` (post-test-harness), `001-phase1-mid.md` (P1-C close).

P1-H closed the v1.0 hardening commitment from VISION §3 / ARCH §24: stack canaries, PAC, BTI, LSE, stack-clash, NX stack. CFI, MTE, and `_FORTIFY_SOURCE` are explicitly deferred with rationale. Audit round 2 found 4 P2 + 6 P3 actionable findings; 8 fixed in the substantive commit, 4 deferred-with-doc, 4 withdrawn.

A fresh session should read this top-to-bottom; everything in handoff 003 still applies for design intent and project-wide policies, but the implementation state and audit posture have moved.

---

## TL;DR

- **Phase 1 momentum**: P1-A through P1-H all landed. The kernel has the full v1.0 hardening posture: KASLR, W^X, stack canaries, PAC return-address signing (ARMv8.3+), BTI indirect-branch guards (ARMv8.5+), LSE atomics permission (compile-time only at v1.0), stack-clash protection, NX stack.
- **Posture**: 2048 MiB total, 2022 MiB free, 26248 KiB reserved. `tests: 7/7 PASS`, `ticks: 9 (kernel breathing)`, `canary: initialized (fold 0x...)`. ELF ~207 KB; flat binary ~29 KB.
- **Audit R2 closed**: 4 P2 + 6 P3 actionable findings prosecuted; 8 fixed in the same commit, 4 deferred with explicit rationale, 4 withdrawn after deeper trace.
- **Next chunk: P1-I** (Phase 1 exit verification: ASan + UBSan instrumented builds; 10000-iteration alloc/free leak check; host-side sanitizer matrix; CI workflow; deliberate-fault test target; Phase 1 closing audit pass).

---

## What landed in P1-H

### `e8c9c5c` — P1-H: hardening flag enablement (audit R2 close)

**Compile flags** (`cmake/Toolchain-aarch64-thylacine.cmake`):
- `-march=armv8-a+lse+pauth+bti` (permit LSE / PAuth / BTI instructions)
- `-fstack-protector-strong` (canaries on functions with arrays / addr-taken locals)
- `-fstack-clash-protection` (probe guard pages on large frames)
- `-mbranch-protection=pac-ret+bti` (paciasp/autiasp + bti markers)
- `-Wl,-z,noexecstack` (NX stack, explicit)

**New files**:
- `kernel/canary.c`, `kernel/include/thylacine/canary.h` — `__stack_chk_guard` (volatile u64; non-zero link-time initializer keeps it in `.data` not `.bss`); `__stack_chk_fail` extincts; `canary_init(seed)` called from `kaslr_init` (which is `__attribute__((no_stack_protector))`); one-shot guard against re-entry (audit F23).
- `arch/arm64/hwfeat.h`, `arch/arm64/hwfeat.c` — reads `ID_AA64ISAR0/1_EL1` + `ID_AA64PFR1_EL1`; populates `g_hw_features`; `hw_features_describe` produces a comma-separated banner string.
- `kernel/test/test_hardening.c` — `hardening.detect_smoke`: cookie != 0; ID-register self-check; PAC + BTI presence; SCTLR_EL1.{EnIA, BT0} runtime bits set (audit F25).
- `docs/reference/12-hardening.md` — comprehensive hardening reference.

**Modified files**:
- `arch/arm64/start.S` — between BSS clear and `bl kaslr_init`: program APIA / APIB / APDA / APDB keys from cntpct rotations; set SCTLR_EL1.{EnIA, EnIB, EnDA, EnDB, BT0} = 1. Long-branch into TTBR1 changed BR → BLR (PSTATE.BTYPE = 01 matches compiler-emitted `bti c` at boot_main's prologue under FEAT_BTI). `_hang` gains `bti jc` landing pad (audit F16).
- `arch/arm64/kaslr.c` — `kaslr_init` marked `__attribute__((no_stack_protector))`; calls `canary_init(mixed)` after entropy is computed but before `apply_relocations`.
- `arch/arm64/mmu.h`, `mmu.c` — `PTE_GP` (bit 50) added; `PTE_KERN_TEXT` includes it; two new `_Static_assert`s pin bit position + adoption (audit F24).
- `kernel/main.c` — calls `hw_features_detect()`; banner adds `hardening:` / `features:` / `canary:` lines. Canary line shows a 16-bit XOR-fold of the cookie, not the cookie itself (audit F14).
- `CMakeLists.txt`, `kernel/CMakeLists.txt` — phase tag; KERNEL_SRCS extended.
- `docs/REFERENCE.md`, `docs/phase1-status.md`, `docs/reference/01-boot.md` — snapshot + status updates.

### Audit R2 closure

Closed list at `memory/audit_r2_closed_list.md`. Summary:

| # | Sev | Closure |
|---|---|---|
| F14 | P2 | Banner cookie leak — replaced with 16-bit XOR-fold (presence-only). |
| F15 | P2 deferred | PAC keys from cntpct; doc extended with structural-fix rationale. Post-v1.0 work. |
| F16 | P2 | `_hang` lacks BTI marker — added `bti jc`. |
| F17 | withdrawn | SCTLR per-CPU; no SMP race. |
| F18 | P3 | `_Static_assert` non-zero `__stack_chk_guard` initializer. |
| F19 | P3 | `hw_features_describe` NUL clobber — `append_str` reserves NUL slot. |
| F20 | P3 deferred | Canary fallback constant `0xa5a5...` — unreachable in practice. |
| F21 | withdrawn | CRC32 field extraction — code matches comment. |
| F22 | P3 deferred | PAC key rotation correlation — combined with F15 in post-v1.0 pass. |
| F23 | P2 | `canary_init` re-entry guard — added one-shot static guard; second call extincts. |
| F24 | P3 | `_Static_assert` PTE_GP bit position + PTE_KERN_TEXT inclusion. |
| F25 | P2 | hardening test now verifies SCTLR_EL1.{EnIA, BT0} at runtime. |
| F26 | withdrawn | `mix(0)=0` collision — algebraic property correctly handled. |
| F27 | P3 deferred | `cntpct` trap on direct-EL1 entry without `EL1PCTEN` — not exhibited at v1.0. |
| F28 | withdrawn | `canary_init` `mix()` canary mismatch — traced; consistent. |

---

## Trip hazards (cumulative; new at P1-H marked NEW)

(Items 1-22 carry over from handoff 003. Items 23-29 are new at P1-H.)

### NEW at P1-H

23. **`kaslr_init` MUST be marked `__attribute__((no_stack_protector))`**. The function calls `canary_init` mid-body, overwriting `__stack_chk_guard`. A canary check on its own frame would prologue-read the link-time magic and epilogue-read the runtime cookie → `__stack_chk_fail` at boot. If a future refactor removes the attribute, the boot test fails with `EXTINCTION: stack canary mismatch (smashed stack)`.
24. **`canary_init` is one-shot** (per audit F23). Second call extincts: `"canary_init called twice (would re-key live frames)"`. Phase 2 process bring-up / debug rekey paths must NOT call `canary_init` again — they should establish a per-thread canary mechanism instead.
25. **PAC keys are from `cntpct_el0`** — ~20-25 bits of entropy. Acceptable at v1.0 ("raise the bar for forgery"); post-v1.0 hardening pass moves this past the DTB parse to use 64-bit firmware-supplied entropy. The four keys (APIA/APIB/APDA/APDB) are also rotation-correlated; recovering APIA reveals the others. Post-v1.0 fix derives each from a separate hash.
26. **PAC enable order matters**: must happen BEFORE the first `paciasp` runs (i.e., before any C function returns from a `bl` site). The current ordering puts PAC enable in start.S between BSS clear and `bl kaslr_init` — verified correct.
27. **Long-branch into TTBR1 uses BLR not BR**. PSTATE.BTYPE = 01 (call type) matches compiler-emitted `bti c` at boot_main's prologue. With BR, BTYPE = 10 → `bti c` would fault. Discarded LR is harmless because boot_main is `noreturn`.
28. **`PTE_GP` is bit 50** (FEAT_BTI). `PTE_KERN_TEXT` includes it; static_asserts pin both. RO/RW pages don't have GP — they're not executable so BTI doesn't apply.
29. **`canary:` banner line shows a 16-bit fold, not the cookie** (audit F14). Don't change to print the full cookie — that re-opens the serial-console-attacker leak.

### Carried from handoff 003

(See 003 for full text. Numbered identically there: compiler fusion + Device alignment, Linux image header + DTB, W^X PTE constructors, EXTINCTION ABI, `volatile g_kernel_pa_*`, `KASLR_LINK_VA` lives twice, TTBR0 stays active post-KASLR, `l3_kernel` shared, magazine residency, `kpage_alloc` returns PA, recursive fault on stack overflow, struct page is 48 bytes, kmem_cache_destroy doesn't audit live, no double-free in SLUB, CMake cache for `THYLACINE_PHASE`, `mmu_map_device` cache assumption, GICv3 only at v1.0, Pi 5 GIC at PA > 4 GiB, `g_dist_base` non-volatile, `.Lexception_return` LOCAL label, timer reload range `[100, INT32_MAX]`, GIC handler table 16 KiB BSS.)

---

## What's next

### Decision tree for the next chunk

**Option A (recommended) — P1-I: Phase 1 exit verification**.

Sub-deliverables:

- **ASan instrumented build**: clang's `-fsanitize=address` produces a kernel that can detect heap UAF / overflow at runtime. Freestanding kernel needs a custom shadow allocator. Tricky; this is the bulk of P1-I.
- **UBSan instrumented build**: `-fsanitize=undefined` catches signed integer overflow, oversized shifts, etc. Easier than ASan; freestanding-friendly with a custom `__ubsan_handle_*` set.
- **10000-iteration alloc/free leak check**: extension to `phys.alloc_smoke` and `slub.kmem_smoke` — run 10000 round-trips, verify free-page count returns to baseline.
- **Host-side test target**: compile leaf modules (mix64, mix, dtb walker math) with sanitizers under host clang; run as a userspace test program with stub headers. Distinct from the in-kernel tests; provides ASan/UBSan coverage on the most-tested-leaf paths.
- **CI workflow**: GitHub Actions (`make test` + `make test-asan` + `make test-ubsan`).
- **Deliberate-fault test target** (`tools/test-fault.sh`): `THYLACINE_DELIBERATE_FAULT={stack_overflow|pac_mismatch|bti_fault|wxe_violation}` build flags, each producing a kernel that triggers exactly one fault and verifies the right `EXTINCTION:` message.
- **Phase 1 closing audit**: spawn the prosecutor agent on the entire Phase 1 surface (boot path through hardening); load `audit_r{1,2}_closed_list.md` as preamble; expect minimal new findings.

Estimated 800-1500 LOC + significant tooling work; 200-300k tokens.

**Option B — Boot-time measurement**: profile the boot path; verify the < 500 ms target from VISION §4. Currently informally measured at ~50 ms through P1-G, but P1-H's PAC/canary additions need rigorous remeasurement.

**Option C — First TLA+ spec sketch**: e.g., `mmu.tla` or `phys.tla`. Optional at P1; mandatory from Phase 2.

**Recommendation**: A. P1-I closes Phase 1 and unlocks Phase 2's process model. The sanitizer matrix is a long-running deliverable that benefits from starting now while the codebase is small.

### Sub-chunks (after P1-I)

After P1-I, **Phase 2** begins: process model + EEVDF scheduler + handles + VMO + the first formal specs (`scheduler.tla`, `namespace.tla`, `handles.tla`, `vmo.tla`).

---

## Posture summary

| Metric | Value |
|---|---|
| Tip commit | `e8c9c5c` (P1-H + audit R2 close) — hash-fixup pending in this session |
| Phase | Phase 1 — P1-A through P1-H complete |
| Next chunk | P1-I (Phase 1 exit verification) |
| Build matrix | default Debug — green |
| `tools/test.sh` | PASS |
| In-kernel tests | 7/7 PASS (kaslr, dtb, phys, slub, gic, timer, hardening) |
| Specs | 0/9 (Phase 2 introduces first spec — `scheduler.tla`) |
| LOC | ~2980 C99 + ~410 ASM + ~75 LD + ~220 sh/cmake + ~200 test = ~3885 |
| Kernel ELF | ~207 KB debug |
| Kernel flat binary | ~29 KB |
| Page tables | 40 KiB BSS |
| struct page array | 24 MiB BSS |
| GIC handler table | 16 KiB BSS |
| Boot banner reserved | ~26 MiB |
| RAM free at boot | ~2022 MiB / 2048 MiB |
| KASLR offsets verified distinct | 5+ |
| Canary fold values verified distinct | 5+ |
| Tick counter at end of boot | 9 |
| Open audit findings | 0 active. Deferred: F15+F22 (PAC entropy), F20 (fallback constant), F27 (EL1PCTEN edge case). |

---

## Stratum coordination

Unchanged from handoff 003. Stratum is feature-complete on Phases 1-7; Phase 8 (POSIX surface) is in progress. Phase 9 (9P server + extensions) is Thylacine Phase 4's integration target.

---

## Format ABI surfaces in flight

These are stable contracts. New at P1-H marked **NEW**.

| Surface | Where | Contract |
|---|---|---|
| Boot banner success line | `kernel/main.c` | `"Thylacine boot OK\n"` |
| Extinction prefix | `kernel/extinction.c` | `"EXTINCTION: "` |
| (existing P1-A through P1-G surfaces — see handoff 003) | | |
| **NEW** hardening banner line | `kernel/main.c` | `"  hardening: MMU+W^X+extinction+KASLR+vectors+IRQ+canaries+PAC+BTI+LSE (P1-H)"` |
| **NEW** features banner line | `kernel/main.c` | `"  features: <comma-list> (CPU-implemented)"` |
| **NEW** canary banner line | `kernel/main.c` | `"  canary: initialized (fold 0x<16-bit>)"` (NEVER the full cookie) |
| **NEW** canary API | `kernel/include/thylacine/canary.h` | `canary_init(u64 seed)` (one-shot; second call extincts), `canary_get_cookie(void)`. Compiler-ABI symbols `__stack_chk_guard` (volatile u64; non-zero link-time init) and `__stack_chk_fail` (extincts). |
| **NEW** hwfeat API | `arch/arm64/hwfeat.h` | `struct hw_features { atomic, crc32, pac_apa/api/gpa/gpi, bti, mte }`; `g_hw_features`; `hw_features_detect()`, `hw_features_describe(buf, cap)`. |
| **NEW** PTE_GP bit | `arch/arm64/mmu.h` | bit 50 (FEAT_BTI Stage 1 EL1). `PTE_KERN_TEXT` includes it. `_Static_assert`'d. |
| **NEW** SCTLR_EL1 hardening bits | `arch/arm64/start.S` | EnIA(31), EnIB(30), EnDA(27), EnDB(13), BT0(35) all set unconditionally. RES0 on pre-FEAT hardware. |
| **NEW** PAC keys | `arch/arm64/start.S` | APIA/APIB/APDA/APDB keys derived from cntpct_el0 rotations. Documented as v1.0 trade-off. |

---

## Things I would NOT recommend deviating from

- **Always use the helper macros / API**.
- **`tools/test.sh` is the canonical test gate**.
- **Doc-update-per-PR**.
- **Two commits per substantive landing** (substantive + hash-fixup).
- **Volatile in `be32_load`, `g_kernel_pa_*`, `g_ticks`** — load-bearing.
- **Don't add tests for evolving subsystems**.
- **`.Lexception_return` stays local**.
- **GIC v3 only at v1.0**.
- **`kaslr_init` stays `no_stack_protector`** (NEW). Removing it breaks the boot canary chain.
- **`_hang`'s `bti jc` stays in place** (NEW). Removing it forecloses Phase 2+ indirect dispatch into `_hang`.
- **`canary:` banner line stays presence-only** (NEW). Don't print the full cookie.

---

## Open questions / future-work tags

(Items U-1 through U-15 carry over from handoff 003. Items U-16 through U-19 are new at P1-H.)

- **U-16** (NEW, P1-H audit F15): PAC keys from cntpct only. Post-v1.0 hardening pass uses dedicated CSPRNG, with PAC enable moved past the DTB parse.
- **U-17** (NEW, P1-H audit F22): PAC key rotation correlation. Same fix as U-16.
- **U-18** (NEW, P1-H): CFI / MTE / `_FORTIFY_SOURCE` deferral. CFI lands when ThinLTO infrastructure exists; MTE at Phase 8 with SLUB tag-aware integration; `_FORTIFY_SOURCE` is N/A for freestanding.
- **U-19** (NEW, P1-H audit F27): `cntpct` trap on direct-EL1 entry without `CNTHCTL_EL2.EL1PCTEN`. Not exhibited at v1.0; a future bare-metal port may need an explicit runtime probe path.

---

## Sign-off

The thylacine runs again — and is now **hardened**. 🐅

(Project motto from `memory/project_motto.md`. P1-H earns the "hardened" milestone — the kernel boots with full v1.0 SOTA hardening, canary cookie randomized per boot, PAC + BTI active on FEAT-supported hardware, and the IRQ vector path now sits inside a properly-guarded execution context. Phase 1 has ten landed chunks; the kernel boots, allocates, faults cleanly, dispatches IRQs, tests itself, AND defends itself. P1-I closes Phase 1 and the road to Phase 2 opens.)
