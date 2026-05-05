# Handoff 005 — Phase 1 CLOSED

**Date**: 2026-05-04
**Tip commit**: `67b6709` (P1-I-D + closing audit), with hash-fixup commit pending in this session
**Author**: Claude Opus 4.7 (1M context)
**Audience**: the next session (or human collaborator) picking this up.
**Predecessors**: `004-phase1-late-hardening.md` (post-P1-H), `003-phase1-mid-late-irq.md` (post-P1-G), `002-phase1-mid-late.md` (post-test-harness), `001-phase1-mid.md` (P1-C close).

**Phase 1 is CLOSED.** All 11 ROADMAP §4.2 exit criteria met (10 fully, 1 deferred-with-rationale: MTE per ARCH §24.3 to Phase 8). Three audit rounds prosecuted the entire boot path + hardening + memory subsystem; 39 prosecution-grade findings catalogued, all dispositioned (29 fixed, 6 deferred-with-rationale, 4 withdrawn). Phase 2 (process model + EEVDF scheduler + handles + VMO + first formal specs) opens.

---

## TL;DR

- **Phase 1 momentum**: P1-A through P1-I all landed. Boot path through KASLR + W^X + extinction + EL drop + boot-stack guard + physical allocator + SLUB + exception vectors + GIC v3 + ARM generic timer + canaries + PAC + BTI + LSE + UBSan + 10000-iter leak checks + deliberate-fault matrix + closing audit.
- **Posture**: 9/9 in-kernel tests PASS each boot. Boot time ~38 ms (well under 500 ms). 10/10 distinct KASLR offsets. UBSan-clean. Deliberate-fault matrix 3/3 PASS (canary, W^X, BTI all fire under attack). Audit rounds 1+2+3 closed; cumulative closed list spans `audit_r1_closed_list.md` + `r2` + `r3`.
- **ELF**: ~216 KB debug (production); ~232 KB (UBSan); 29 KB flat binary.
- **Next chunk: P2-A** (Phase 2 entry — first deliverable). Scope: process model bootstrap (struct Proc, struct Thread, rfork), EEVDF scheduler skeleton, first formal spec (`scheduler.tla`).

---

## What landed in P1-I (4 sub-chunks)

### `45f23fb` — P1-I-AB: 10000-iter leak check + boot-time measurement + UBSan trapping

- `kernel/test/test_phys.c` + `test_slub.c`: `phys.leak_10k` + `slub.leak_10k` tests cycle a single allocation through alloc/free 10 000 times; verify free count returns to baseline.
- `arch/arm64/start.S` + `kernel/main.c`: capture `CNTPCT_EL0` at `_real_start` entry; banner reports elapsed ms.
- `tools/verify-kaslr.sh`: N-boot script verifying ≥ 70% distinct offsets.
- `THYLACINE_SANITIZE=undefined` CMake var + `tools/build.sh --sanitize=ubsan` + `tools/test.sh --sanitize=ubsan` + `tools/run-vm.sh` honoring `THYLACINE_BUILD_DIR`. UB triggers BRK → `extinction("brk instruction (assertion?)")`. KASAN deferred (custom shadow allocator).

### `a9e0caa` — P1-I-C: deliberate-fault matrix

- `kernel/fault_test.c`: three provoker functions gated by `THYLACINE_FAULT_TEST=<variant>`. Production builds (no variant) reduce to a single `ret`.
  - `canary_smash`: writes past stack-array bounds via inline-asm-laundered pointer (defeats clang's UB-driven DCE). Canary fires at function epilogue.
  - `wxe_violation`: stores to `_kernel_start` (.text RO). Permission fault.
  - `bti_fault`: volatile function-pointer call to a hand-rolled `nop`-prefixed asm target. PSTATE.BTYPE=01 doesn't match → BTI exception EC=0x0D.
- `arch/arm64/exception.c`: new `EC_BTI` (0x0D) case → `extinction("BTI fault (...)")`.
- `tools/test-fault.sh`: bash 3.2-compatible runner. For each variant: build + boot + match expected EXTINCTION:. Confirm-wait on first `^EXTINCTION:` sighting (UART prints char-by-char).
- PAC mismatch deferred (FEAT_FPAC + poison-bit recognition is implementation-defined).

### `67b6709` — P1-I-D + closing audit

- Round-3 prosecutor on the entire Phase 1 surface; 11 findings.
- **2 P1 fixed**: F29 (phys_init reservation overlap; Pi 5 8 GiB DTB collision), F30 (spin_lock_irqsave was no-op stub but IRQs LIVE — real DAIF save/restore now in place).
- **4 P2 fixed**: F31 (mmu_map_device kernel-image rejection), F32 (kfree pointer-boundary validation), F33 (kmem_cache_destroy full_list tracking), F34 (explicit firmware reservation).
- **3 P3 fixed**: F35 (FDT_MAGIC + struct page _Static_asserts), F37 (buddy_free order bound), F39 (apply_relocations strict).
- **2 P3 deferred-with-rationale**: F36 (long-branch PA-LR — re-eval at Phase 2 unwinder), F38 (PAC key correlation — combined with audit-r2 F15+F22 in post-v1.0 hardening pass).
- New reference doc `13-verification.md`. ROADMAP §4.2 exit criteria all checked off.

---

## Trip hazards (cumulative; new at P1-I marked NEW)

(Items 1-29 carry over from handoff 004. Items 30-34 are new at P1-I.)

### NEW at P1-I

30. **`kfree(p)` requires p be a head pointer** (P1-I F32). Slab path: pointer must be at object boundary (`(pa - slab_base) % actual_size == 0`). Large path: pointer must equal page-aligned base. Interior pointers extincts with a specific diagnostic. If a future caller does `void *p = kmalloc(N); ...; kfree((char*)p + offset)`, the failure is loud, not silent.
31. **`kmem_cache_destroy` extincts on live objects** (P1-I F33). The header documented "no live objects remain" as caller responsibility; v1.0 enforces by tracking full slabs in `full_list` and extincting on `nr_full != 0`. Phase 2 callers MUST drain their own caches before destroy.
32. **`spin_lock_irqsave` actually masks IRQs now** (P1-I F30). The Phase 1 stubs lied. Anywhere in a critical section that calls into mm code, IRQs are masked for the duration. Phase 2 SMP adds the LL/SC contention; the IRQ-mask discipline is the v1.0 layer.
33. **`mmu_map_device` rejects ranges hitting kernel-image 2 MiB block** (P1-I F31). Returns false (caller decides what to do). Today only QEMU virt's GIC at 0x08000000 is mapped; future Pi 5 work that adds devices to the same 2 MiB block as kernel needs the per-page mapping path (post-v1.0).
34. **F38 / F15 / F22 cumulative deferred PAC entropy work** (post-v1.0). Canary cookie + PAC keys + KASLR slide all share a single ~64-bit cntpct measurement. Phase 2 should use independent CSPRNG draws for each. Deferred to post-v1.0 hardening pass.

### Carried from handoff 004

(See 004 for full text. Items 1-29: compiler fusion + Device alignment, Linux image header + DTB, W^X PTE constructors, EXTINCTION ABI, `volatile g_kernel_pa_*`, `KASLR_LINK_VA` lives twice, TTBR0 stays active post-KASLR, `l3_kernel` shared, magazine residency, `kpage_alloc` returns PA, recursive fault on stack overflow, struct page is 48 bytes, kmem_cache_destroy [now-strict] / no double-free in SLUB, CMake cache for `THYLACINE_PHASE`, `mmu_map_device` cache assumption [now reinforced with kernel-image rejection], GICv3 only at v1.0, Pi 5 GIC at PA > 4 GiB, `g_dist_base` non-volatile, `.Lexception_return` LOCAL label, timer reload range `[100, INT32_MAX]`, GIC handler table 16 KiB BSS, `kaslr_init` no_stack_protector, `canary_init` one-shot, PAC keys from cntpct, PAC enable order, BR→BLR, PTE_GP=bit 50, canary banner is fold-only.)

---

## What's next

**Phase 2 begins.** Per ROADMAP §5: process model + EEVDF scheduler + handles + VMO + first formal specs (`scheduler.tla`, `namespace.tla`, `handles.tla`, `vmo.tla`).

### Decision tree for the first Phase 2 chunk

**Option A (recommended) — P2-A: process-model bootstrap**.

Sub-deliverables:
- `kernel/proc.{h,c}`: `struct Proc` (per-process state — pgrp, namespace, handle table head). Pre-allocated kernel proc (PID 0) for the boot path. `kmem_cache` for Proc instances.
- `kernel/thread.{h,c}`: `struct Thread` (per-thread state — TCB, stack, scheduler queue links). Pre-allocated kernel thread (TID 0) for the boot path. `kmem_cache` for Thread instances.
- `arch/arm64/context.{h,c}`: context-switch primitive. `cpu_switch_context(prev_thread, next_thread)` saves callee-saved regs to prev's TCB, loads from next's. Pure asm; no scheduler logic yet.
- `kernel/main.c`: replace the `_hang()` fallthrough with `thread_init()` + `proc_init()` + (eventually) the first explicit yield. v1.0 still has no userspace; the boot kernel runs as PID 0 / TID 0.
- First formal spec: `specs/scheduler.tla` sketch. Doesn't have to be TLC-clean — just the state model. Phase 2 P2-B/C iterate on it.
- Estimated 800-1200 LOC; 200-300k tokens.

**Option B — first formal spec only**: write `specs/scheduler.tla` cleanly first; implement against it. Per CLAUDE.md spec-first policy this is the canonically-correct order. The trade-off is no immediate runtime evidence; the spec lands and code follows.

**Option C — boot timing measurement deepening**: profile each boot subsystem; produce a micro-budget per subsystem. No new code; just measurement infrastructure.

**Recommendation**: A with B in parallel — start the spec sketch alongside the implementation; iterate both before P2-A commits. Per ARCH §25.2 (specs gate-tied to phases), `scheduler.tla` is mandatory for Phase 2 close, not Phase 2 entry — but starting the design with the spec produces better code.

---

## Posture summary

| Metric | Value |
|---|---|
| Tip commit | `67b6709` (P1-I-D + closing audit) — hash-fixup pending in this session |
| Phase | **Phase 1 CLOSED** |
| Next chunk | P2-A (Phase 2 entry — process-model bootstrap) |
| Build matrix | default Debug — green; UBSan trapping — green |
| `tools/test.sh` | PASS |
| `tools/test-fault.sh` | PASS (3/3 protections fire under attack) |
| `tools/verify-kaslr.sh -n 10` | PASS (10/10 distinct) |
| In-kernel tests | 9/9 PASS (kaslr, dtb, phys [smoke + leak_10k], slub [smoke + leak_10k], gic, timer, hardening) |
| Specs | 0/9 (Phase 2 introduces first mandatory spec — `scheduler.tla`) |
| LOC | ~3120 C99 + ~440 ASM + ~75 LD + ~330 sh/cmake = ~3965 |
| Kernel ELF | ~216 KB debug |
| Kernel flat binary | ~29 KB |
| Boot time | 38 ms (production), 65 ms (UBSan) — VISION §4 budget 500 ms |
| Page tables | 40 KiB BSS |
| struct page array | 24 MiB BSS |
| GIC handler table | 16 KiB BSS |
| Boot banner reserved | ~26 MiB |
| RAM free at boot | ~2022 MiB / 2048 MiB |
| KASLR offsets | 10/10 distinct verified across 10 boots |
| Canary fold values | 5+ distinct (cookie randomized per boot) |
| Open audit findings | 0 active. Rounds 1+2+3 closed; deferred items tracked at `memory/audit_r3_closed_list.md`. |

---

## Phase 1 by the numbers

- **9 sub-chunks** landed: P1-A (toolchain), P1-B (DTB), P1-C (MMU + W^X + extinction), P1-C-extras A (EL drop + stack guard), P1-C-extras B (KASLR), P1-D (buddy + magazines), P1-E (SLUB), P1-F (vectors + sync), P1-G (GIC + timer + IRQ), P1-H (hardening), P1-I (verification + closing audit).
- **39 audit findings** prosecuted across 3 rounds: r1 (12), r2 (15), r3 (12 — actually 11 numbered F29-F39 plus the cumulative-deferred list).
- **0 P0** at any point.
- **6 P1** raised across the rounds; all fixed by the rounds they were raised in.
- **12 P2** raised; 11 fixed, 1 deferred (F15 PAC entropy → post-v1.0 hardening pass).
- **15 P3** raised; 11 fixed, 4 deferred (F20 fallback constant, F27 EL1PCTEN edge, F36 PA-LR, F38 PAC correlation).
- **6 withdrawn** after deeper trace (F1, F17, F21, F26, F28 + 1 r3 implicit).
- **9 in-kernel tests** at Phase 1 close.
- **3 verification scripts** (`tools/test.sh`, `tools/test-fault.sh`, `tools/verify-kaslr.sh`).
- **2 build matrices** (production + UBSan).
- **13 reference docs** (`docs/reference/00-overview.md` through `13-verification.md`).
- **5 handoff docs** (`001-phase1-mid.md` through this `005-phase1-close.md`).

---

## Stratum coordination

Stratum's Phase 8 (POSIX surface) is in progress; Phase 9 (9P server + Stratum extensions: Tbind/Tunbind/Tpin/Tunpin/Tsync/Treflink/Tfallocate) is Thylacine Phase 4's integration target. **Phase 2 (process model) is independent of Stratum**; Phases 2 + 3 (devices) proceed in parallel with Stratum's 8 + 9.

---

## Format ABI surfaces in flight

(All carry forward from handoff 004 unchanged. Phase 2 will introduce new surfaces — process struct ABI for /proc, scheduler tick rate, ipi numbers — when they land.)

---

## Things I would NOT recommend deviating from (cumulative)

- **Always use the helper macros / API**.
- **`tools/test.sh` is the canonical test gate**.
- **`tools/test-fault.sh` is the canonical hardening-protection gate**.
- **Doc-update-per-PR**.
- **Two commits per substantive landing** (substantive + hash-fixup).
- **Volatile in `be32_load`, `g_kernel_pa_*`, `g_ticks`** — load-bearing.
- **Don't add tests for evolving subsystems** — write them when the API stabilizes.
- **`.Lexception_return` stays local**.
- **GIC v3 only at v1.0**.
- **`kaslr_init` stays `no_stack_protector`**.
- **`_hang`'s `bti jc` stays in place**.
- **`canary:` banner line stays presence-only** (fold, not cookie).
- **`spin_lock_irqsave` REALLY masks IRQs now** — don't revert to no-op (NEW at P1-I F30).
- **`kfree` validates pointer boundaries** — don't relax (NEW at P1-I F32).
- **`kmem_cache_destroy` extincts on `nr_full != 0`** — don't silence (NEW at P1-I F33).
- **`mmu_map_device` rejects kernel-image overlap** — don't relax (NEW at P1-I F31).
- **`apply_relocations` brk-on-unsupported** — don't silently skip (NEW at P1-I F39).

---

## Open questions / future-work tags

(Items U-1 through U-19 carry over. Items U-20 through U-24 are new at P1-I.)

- **U-20** (NEW, P1-I audit F36): long-branch's LR is PA. Re-evaluate when Phase 2's stack-trace / unwinder lands.
- **U-21** (NEW, P1-I audit F38 + r2 F15+F22): canary + PAC + KASLR slide all derive from one cntpct measurement. Post-v1.0 hardening pass: independent CSPRNG draws per use.
- **U-22** (NEW, P1-I): PAC deliberate-fault test deferred. Post-v1.0 with FEAT_FPAC + poison-bit recognition.
- **U-23** (NEW, P1-I): KASAN-equivalent kernel ASan. Freestanding-kernel shadow allocator is significant scope; per ARCH §24.5 production binaries don't have ASan; MTE provides hardware-equivalent at Phase 8.
- **U-24** (NEW, P1-I): boot-time gate (extinct if > 500 ms). Banner reports the time but no hard cutoff. Add when there's a target whose boot is on the edge.

---

## Sign-off

The thylacine has been brought back to life — and now stands hardened and tested. **Phase 1 is closed.** 🐅

(Project motto reserved per `memory/project_motto.md` for milestone moments. Phase 1 close is the canonical such moment: foundation phase complete; the kernel boots, allocates without leaks, fires its hardening protections under attack, has been adversarially prosecuted three times, and verifies its KASLR distribution across 10 boots. **Phase 2 (process model + EEVDF scheduler + handles + VMO + first formal specs) opens.**)
