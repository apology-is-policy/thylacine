# 13 — Phase 1 verification infrastructure (as-built reference)

The verification cadence that closes Phase 1: 10 000-iteration alloc/free leak checks, boot-time measurement, multi-boot KASLR variability, UBSan trapping build, and the deliberate-fault matrix. Each artifact below maps to a `ROADMAP.md §4.2` exit criterion and lands as part of P1-I.

P1-I-AB landed `45f23fb` (verification-infra subset); P1-I-C landed `a9e0caa` (deliberate-fault matrix). Closing audit + Phase 1 exit landing in the same chunk as this doc.

Scope: `kernel/test/test_phys.c` + `test_slub.c` (10000-iter tests), `arch/arm64/start.S` + `kernel/main.c` (boot timing), `tools/verify-kaslr.sh`, `cmake/Toolchain-aarch64-thylacine.cmake` + `CMakeLists.txt` (UBSan), `kernel/fault_test.c` + `arch/arm64/exception.c` + `tools/test-fault.sh` (fault matrix), `tools/build.sh` + `tools/run-vm.sh` + `tools/test.sh` (build dir + sanitize plumbing).

Reference: `ARCHITECTURE.md §24` (hardening), `§25` (verification cadence + audit triggers), `ROADMAP.md §4.2` (Phase 1 exit criteria).

---

## Purpose

Phase 1's earlier sub-chunks (P1-A through P1-H) added kernel mechanisms — boot, MMU, KASLR, allocator, vectors, GIC, timer, hardening flags. Phase 1 closing requires evidence that those mechanisms are SOUND under the conditions the kernel will face in Phase 2 (process model + scheduler) and beyond:

- Allocator paths must not leak across the kernel uptime (10 000 cycles).
- Boot-to-banner must fit in a tight latency budget (< 500 ms).
- KASLR must vary across multiple boots (entropy quality is real).
- The kernel must be UB-free on the boot path (UBSan-clean).
- Hardening protections (canaries, W^X, BTI) must FIRE under attack — not merely exist as compile flags.

P1-I lands the verification artifacts that DEMONSTRATE each of these. Each is a regression gate; a future change that breaks any of them surfaces in tools/test.sh / tools/test-fault.sh / tools/verify-kaslr.sh.

---

## Public API

P1-I doesn't add new kernel API. The artifacts are:

**In-kernel tests** (registered in `kernel/test/test.c`):
- `phys.alloc_smoke` — 256 × order-0 + order-9 + order-10 round-trips (P1-D smoke).
- `phys.leak_10k` — 10 000-iter `alloc_pages(0)` / `free_pages` round-trips; verifies no drift.
- `slub.kmem_smoke` — 1500 × kmalloc(8) + mixed sizes + 8 KiB direct + custom kmem_cache (P1-E smoke).
- `slub.leak_10k` — 10 000-iter `kmalloc(64)` / `kfree` round-trips.

**Boot banner** (`kernel/main.c`):
- `boot-time: <ms> ms (target < 500 ms per VISION §4)` — elapsed `CNTPCT_EL0` → ms via `timer_get_freq()` (CNTFRQ).

**Tooling**:
- `tools/test.sh [--sanitize=ubsan]` — boot test; PASS iff `Thylacine boot OK` line appears within 10 s.
- `tools/test-fault.sh [variant...]` — deliberate-fault matrix: builds + boots each variant; PASS iff the matching `EXTINCTION:` line appears.
- `tools/verify-kaslr.sh [-n N]` — N boots; PASS iff ≥ 70% of KASLR offsets are distinct AND no boot extincted.
- `tools/build.sh kernel [--sanitize=ubsan] [--build-dir=DIR]` — kernel build with optional sanitizer + alternate output dir.

**CMake variables** (top-level `CMakeLists.txt`):
- `THYLACINE_SANITIZE` — `""` (default) or `"undefined"` (UBSan trapping mode).
- `THYLACINE_FAULT_TEST` — `""` (default) or `canary_smash` / `wxe_violation` / `bti_fault`.

---

## Implementation

### 10 000-iteration leak checks

```c
// kernel/test/test_phys.c
void test_phys_leak_10k(void) {
    u64 baseline = phys_free_pages();
    for (unsigned i = 0; i < 10000; i++) {
        struct page *p = alloc_pages(0, KP_ZERO);
        TEST_ASSERT(p != NULL, "alloc_pages(0) returned NULL mid-10k");
        free_pages(p, 0);
    }
    magazines_drain_all();
    u64 after = phys_free_pages();
    TEST_ASSERT(after == baseline,
        "phys_free_pages drift after 10000-iter alloc/free");
}
```

The test cycles a SINGLE pointer (sequential alloc → free), not 10 000 simultaneous (which would overflow the 16 KiB boot stack at 80 KiB just for the array). A leak per cycle would compound across iterations and surface as a non-zero drift after `magazines_drain_all`.

`magazines_drain_all` is essential: per-CPU magazines retain a refill residency (8 pages held mid-cycle). Without the drain, the `phys_free_pages` comparison would be off by 8.

`slub.leak_10k` is the same pattern with `kmalloc(64)`/`kfree`.

### Boot-time measurement

`arch/arm64/start.S`:

```asm
.Lel1_main:
    // 1.5. Capture boot-start counter (P1-I).
    mrs     x22, cntpct_el0
    ...
    // 6.25. Save boot-start CNTPCT.
    adrp    x0, _boot_start_cntpct
    add     x0, x0, :lo12:_boot_start_cntpct
    str     x22, [x0]
```

A new BSS slot `_boot_start_cntpct` (8 bytes) holds the captured counter. Read at the start of `_real_start` (post-EL-drop, before BSS clear — stashed in callee-saved x22 because the BSS clear would zero the global).

`kernel/main.c` reads `CNTPCT_EL0` again post-banner, computes elapsed ticks, converts to milliseconds via `CNTFRQ_EL0` (cached by `timer_init` and exposed as `timer_get_freq()`):

```c
u64 now;
__asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(now));
u64 elapsed_ticks = now - _boot_start_cntpct;
u64 freq = (u64)timer_get_freq();
u64 us = (elapsed_ticks * 1000000UL) / freq;
uart_puts("  boot-time: ");
uart_putdec(us / 1000);
uart_puts(".");
uart_putdec((us % 1000) / 100);
uart_puts(" ms (target < 500 ms per VISION §4)\n");
```

Measured: ~37 ms (production), ~65 ms (UBSan). Both well under the 500 ms VISION §4 budget. The 5-ms tick busy-wait + 9-test execution dominate; the post-banner-print itself is sub-microsecond.

### `tools/verify-kaslr.sh`

Runs N boots (default 10), parses `KASLR offset 0xN` from each banner, PASSes if:
1. No boot extincted (each banner had `Thylacine boot OK`).
2. The set of distinct offsets has size ≥ 70% of N.

The 70% threshold accommodates rare collisions: with 13 bits of entropy (8192 distinct 2 MiB-aligned offsets) and 10 boots, the natural collision probability is ~0.55%. Failing PASS over a single accidental collision in CI would be brittle; 70% is a generous floor.

Verified: 10/10 distinct offsets across 10 boots.

### UBSan trapping build

`-fsanitize=undefined -fsanitize-trap=undefined` is added when `THYLACINE_SANITIZE=undefined`. UB triggers a `BRK` instruction; the kernel's sync exception handler routes `EC_BRK` to `extinction("brk instruction (assertion?)", elr)`. **No `__ubsan_handle_*` runtime is needed** — the trap mode is the entire mechanism.

`tools/build.sh --sanitize=ubsan` and `tools/test.sh --sanitize=ubsan` use a separate build dir (`build/kernel-undefined`) so the production CMake cache isn't clobbered. `tools/run-vm.sh` honors `THYLACINE_BUILD_DIR` for the alternate ELF.

Verified: clean UBSan boot (9/9 in-kernel tests PASS, no UB triggered) confirms no latent UB in the boot path. KASAN (kernel ASan) is **deferred** — it requires a custom shadow allocator in a freestanding kernel, which is significant scope. Per `ARCH §24.5`, production binaries don't have ASan; MTE provides the hardware-equivalent at Phase 8.

### Deliberate-fault matrix

`kernel/fault_test.c` defines three provoker functions, each gated by `#ifdef THYLACINE_FAULT_TEST_<variant>`. `boot_main` calls `fault_test_run()` immediately before printing `Thylacine boot OK`; in production builds (no fault-test set), the function is a single `ret`.

**`canary_smash`**:

```c
__attribute__((noinline))
static char provoke_canary_smash(void) {
    char buf[16];
    char *p = buf;
    __asm__ __volatile__("" : "+r"(p));    // launder; clang loses track
    for (unsigned i = 0; i < 64; i++) {
        p[i] = (char)0xAA;
    }
    return p[0];
}
```

The `asm("":"+r"(p))` launder is **load-bearing**. Without it, clang at `-O2` with `-fstack-protector-strong` proves the writes are OOB, classifies them as UB, and elides them — the canary check is never armed because the array is "unused." Verified by disassembly: laundered version emits 64 strb instructions; non-laundered version emits ~16 sparse writes that miss the canary slot.

The function returns `p[0]` (a value derived from the array) so clang must allocate `buf` on the stack rather than scalarize it.

**`wxe_violation`**:

```c
__attribute__((noinline))
__attribute__((no_stack_protector))
static void provoke_wxe_violation(void) {
    volatile char *target = _kernel_start;
    *target = (char)0xCC;
}
```

Stores a byte to `_kernel_start` (the first byte of `.text`, mapped `PTE_KERN_TEXT` = RX, no W). The MMU raises a permission fault; `exception_sync_curr_el` recognizes FAR ∈ `[_kernel_start, _kernel_end)` AND `fsc_is_permission(fsc)` and emits `extinction("PTE violates W^X (kernel image)")`.

`no_stack_protector` is required: without it, the canary check (which would also fail because the function has no array) might mask the W^X fault — but actually clang doesn't emit a canary for a function without arrays / addr-taken locals. Belt-and-braces.

**`bti_fault`**:

```c
__asm__ (
    ".section .text, \"ax\"\n"
    ".globl _bti_unguarded_target\n"
    ".type _bti_unguarded_target, %function\n"
    "_bti_unguarded_target:\n"
    "    nop\n"
    "    ret\n"
    ".size _bti_unguarded_target, . - _bti_unguarded_target\n"
);

__attribute__((noinline))
__attribute__((no_stack_protector))
static void provoke_bti_fault(void) {
    void (*volatile fp)(void) = _bti_unguarded_target;
    fp();
}
```

The hand-rolled asm target's first instruction is `nop`, NOT `bti j/c/jc`. The volatile function pointer is **load-bearing**: without it, clang at `-O2` devirtualizes `fp()` → `bl _bti_unguarded_target`. `bl` is a direct branch; it does NOT set `PSTATE.BTYPE`, so the BTI check is bypassed. The volatile forces a load + `blr` sequence, and `blr` sets `BTYPE=01` (call type). The target's `nop` doesn't match `bti c`/`bti jc`; SCTLR_EL1.BT0=1 + page has PTE_GP=1 → Branch Target Exception (`ESR_EL1.EC=0x0D`) → `extinction("BTI fault (indirect branch to non-guarded target)")`.

**`pac_mismatch`** is **deferred** to post-v1.0. Forging the saved LR via inline asm produces a poisoned-bit pattern whose recognition depends on FEAT_FPAC presence and implementation-defined behavior. v1.0 verifies PAC via:
1. `hardening.detect_smoke` asserts `SCTLR_EL1.EnIA` is set + APIA key is non-zero.
2. Manual code review confirms `paciasp/autiasp` emission.
3. ARM ARM mandates the hardware enforces.

### `tools/test-fault.sh`

For each variant, the script:
1. Builds the kernel with `-DTHYLACINE_FAULT_TEST=<variant>` in `build/kernel-fault-<variant>`.
2. Boots under QEMU with `THYLACINE_BUILD_DIR` pointing at the alternate dir.
3. Polls the log every 100 ms for: (a) the expected `EXTINCTION:` substring, (b) `Thylacine boot OK` (provoker silent), (c) any `^EXTINCTION:` (wrong diagnostic).
4. **Confirm-wait** on first `^EXTINCTION:` sighting: sleeps 1 s and re-checks the expected substring (UART prints char-by-char; the full message may not yet have flushed at the polling instant).
5. PASSes iff the expected message matched.

Bash 3.2 compatible (no associative arrays — uses `case` for variant → expected mapping; macOS default).

---

## Data structures

P1-I doesn't add new data structures except:
- `_boot_start_cntpct` — 8-byte BSS slot. Single `volatile u64`.
- `_bti_unguarded_target` — 8-byte function in `.text` (only in `bti_fault` builds).
- `__stack_chk_guard` (unchanged from P1-H) — `volatile u64` in `.data`.

---

## State machines

The verification artifacts are stateless functions / scripts. The only "state machine" is the deliberate-fault test variant selection:

```
THYLACINE_FAULT_TEST unset (production)
    fault_test_run() → ret (no-op)

THYLACINE_FAULT_TEST=canary_smash
    fault_test_run() → provoke_canary_smash() → __stack_chk_fail() → extinction()

THYLACINE_FAULT_TEST=wxe_violation
    fault_test_run() → provoke_wxe_violation() → permission fault → extinction()

THYLACINE_FAULT_TEST=bti_fault
    fault_test_run() → provoke_bti_fault() → BTI exception → extinction()
```

The variant is selected at compile time; only one provoker is included in the binary.

---

## Tests

The verification infra IS the test layer. P1-I adds:

| Artifact | What it verifies | ROADMAP §4.2 criterion |
|---|---|---|
| `phys.leak_10k` | 10 000-iter `alloc_pages(0)` round-trip; no leak | "kmalloc/kfree round-trip 10 000 allocations without leak" |
| `slub.leak_10k` | 10 000-iter `kmalloc(64)` round-trip; no leak | (same) |
| Boot-time banner | Elapsed boot ticks → ms; reports vs 500 ms target | "Boot to UART banner < 500 ms" |
| `tools/verify-kaslr.sh` | ≥ 70% distinct KASLR offsets across 10 boots | "KASLR: kernel base address differs across boots (verified across 10 boots)" |
| UBSan trapping build | No UB on boot path | "Sanitizer build runs without false positives on boot path" |
| `canary_smash` fault test | Stack canary fires under deliberate smash | (no specific exit criterion; closes the audit-deferred test) |
| `wxe_violation` fault test | W^X fires under deliberate kernel-text write | "MMU on; kernel VA map correct" + audit verification |
| `bti_fault` fault test | BTI fires under deliberate non-guarded indirect branch | "BTI enabled (deliberate indirect branch to non-BTI target panics cleanly)" |

NOT covered by P1-I (deferred):
- `pac_mismatch` deliberate-fault test — post-v1.0.
- KASAN-equivalent shadow allocator — post-v1.0 / Phase 8 MTE.
- TLA+ specs — Phase 2 onward (mandatory from P2; sketches optional at P1).
- `_FORTIFY_SOURCE` — N/A (freestanding kernel).
- LSE atomic LL/SC fallback — Phase 2 (no atomics in v1.0 kernel).

---

## Error paths

| Condition | Behavior |
|---|---|
| Leak test detects drift | TEST_ASSERT fails; test harness reports FAIL; `boot_main` extincts via the catch-all |
| Boot-time exceeds target | Banner reports the time; no automated gate; future P1-I refinement: `extinction` if > 500 ms |
| `verify-kaslr.sh`: < 70% distinct offsets | Exit 1; CI marks RED |
| `verify-kaslr.sh`: any boot extincts | Exit 1 |
| UBSan: UB triggered on boot path | BRK → `extinction("brk instruction (assertion?)")`; tools/test.sh catches as FAIL |
| `test-fault.sh`: provoker silent | Exit 1 with the kernel having reached "Thylacine boot OK" |
| `test-fault.sh`: wrong extinction | Exit 1 with the unexpected diagnostic dumped |
| `test-fault.sh`: timeout | Exit 1 |

---

## Performance characteristics

| Artifact | Cost |
|---|---|
| `phys.leak_10k` runtime | < 100 ms on QEMU virt (10 000 alloc/free cycles dominated by buddy-list manipulation; magazines absorb most cycles) |
| `slub.leak_10k` runtime | < 50 ms (slab fast path) |
| Boot-time banner overhead | < 1 µs (one MRS + arithmetic) |
| `verify-kaslr.sh -n 10` total | ~5-10 s (10 × test.sh with QEMU startup ~500 ms each) |
| UBSan build size impact | +6 KB ELF (~3% growth on 213 KB production); UB trap instructions inline |
| UBSan boot-time impact | ~30 ms slower (extra runtime checks in the leak loops; production 37 ms → UBSan 65 ms) |
| Deliberate-fault build size | +1-2 KB per variant (provoker + BTI target) |
| `test-fault.sh` total runtime | ~15 s (3 × build + boot) |

All artifacts comfortably fit within the existing test-harness budget.

---

## Status

**Implemented at P1-I**:

- `phys.leak_10k` + `slub.leak_10k` in-kernel tests (P1-I-AB).
- Boot-time measurement (`_boot_start_cntpct` BSS slot + banner readout) (P1-I-AB).
- `tools/verify-kaslr.sh` with 70% distinct-offset gate (P1-I-AB).
- `THYLACINE_SANITIZE=undefined` UBSan trapping build + `tools/test.sh --sanitize=ubsan` plumbing (P1-I-AB).
- `THYLACINE_FAULT_TEST=<canary_smash|wxe_violation|bti_fault>` deliberate-fault provokers in `kernel/fault_test.c` (P1-I-C).
- `tools/test-fault.sh` matrix runner (P1-I-C).
- `arch/arm64/exception.c` `EC_BTI` (0x0D) case for clear BTI fault diagnostic (P1-I-C).

**Not yet implemented** (deferred with explicit rationale):

- **PAC mismatch** deliberate-fault test. Post-v1.0 hardening pass (combined with the dedicated CSPRNG key derivation per audit-r2 F15+F22).
- **KASAN** kernel ASan. Freestanding-kernel shadow allocator is significant scope; per ARCH §24.5 production binaries don't have it. Phase 8's MTE provides the hardware-equivalent.
- **CI workflow** (GitHub Actions). Independent of phase; can land alongside any chunk. Sketched in `docs/handoffs/004-phase1-late-hardening.md` decision tree.
- **Boot-time gate**: banner reports time but doesn't fail the build if > 500 ms. Add when there's a test target whose boot is on the edge.

**Landed**: P1-I-AB at `45f23fb`, P1-I-C at `a9e0caa`, P1-I-D + closing audit at `*(pending)*`.

---

## Caveats

### Anti-DCE patterns are FRAGILE

The deliberate-fault provokers depend on clang NOT optimizing through their UB. Specifically:

- `canary_smash` uses `__asm__("":"+r"(p))` to launder the stack-array pointer. A future clang change that propagates "this asm doesn't actually do anything" through the constraint could re-enable DCE.
- `bti_fault` uses `volatile` on the function pointer to prevent devirtualization. A future clang change that ignores `volatile` for function-pointer tracking would convert `blr` → `bl` and bypass BTI.

Both are documented in `kernel/fault_test.c` as durable comments. If a future test run shows `provoker silent`, the most likely cause is a toolchain change defeating the anti-DCE pattern. Diagnosis: disassemble the provoker (`llvm-objdump -d build/kernel-fault-<variant>/thylacine.elf | grep -A 30 provoke_<name>`) and verify the OOB writes / `blr` instruction.

### UBSan's BRK is recognized but not classified

When UB fires, the BRK instruction triggers `EC_BRK` which the sync handler reports as `"brk instruction (assertion?)"`. There's no UBSan-specific diagnostic; a developer reading `EXTINCTION: brk instruction (assertion?)` has to suspect UBSan trap (vs. an assertion in the code).

Future refinement: read `ESR_EL1.ISS[15:0]` for the `brk` immediate; UBSan uses specific immediates that distinguish it from kernel `assert()`. Add to `exception_sync_curr_el`'s diagnostic. Tracked as P1-I follow-up.

### Boot-time measurement starts at `_real_start`, not at firmware entry

`_boot_start_cntpct` captures `CNTPCT_EL0` at the start of `.Lel1_main` — AFTER the EL2 drop on Pi 5 (a few hundred cycles) and AFTER QEMU's load_aarch64_image stage (negligible). The reported boot time is "kernel boot time", not "firmware-entry-to-banner."

For a Pi 5 boot measurement that includes UEFI initialization, an external timer is needed (out of scope at v1.0; QEMU virt doesn't have one).

### `test-fault.sh` confirm-wait is 1 second

The polling loop sleeps 1 s after observing `^EXTINCTION:` to allow the line to fully UART-print before classifying. On a fast host the line completes in microseconds; on a slow host (heavily loaded CI) it might take longer. If the 1 s wait is too short, the test reports `wrong_extinction` even when the right diagnostic is in flight. Mitigation: increase the wait via env var or reduce poll frequency. Not yet a production issue.

### `verify-kaslr.sh` 70% threshold is a CI compromise

10/10 distinct offsets is the natural expectation; one collision drops to 9/10 = 90% (still PASS). 70% allows up to 3/10 collisions, which is *very* generous (probability ~10⁻⁴ at 13 bits). The threshold is set this way so CI flakes don't fail the gate; tighter thresholds would be a future refinement (e.g., a "two-tier" pass: STRICT requires 100%, LENIENT 70%).

---

## See also

- `docs/reference/01-boot.md` — entry sequence (where boot-time capture lives).
- `docs/reference/05-kaslr.md` — KASLR + canary cookie init (leak/timing tests don't touch this).
- `docs/reference/06-allocator.md` — buddy + magazines (10000-iter test exercises).
- `docs/reference/07-slub.md` — SLUB (10000-iter kmalloc test).
- `docs/reference/12-hardening.md` — what the deliberate-fault matrix verifies.
- `docs/ARCHITECTURE.md §24` — hardening commitment + audit-trigger surfaces.
- `docs/ARCHITECTURE.md §25` — verification cadence + closing-audit policy.
- `docs/ROADMAP.md §4.2` — Phase 1 exit criteria; this doc maps each one to its verification artifact.
