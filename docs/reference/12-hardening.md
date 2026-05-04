# 12 — Hardening enablement (as-built reference)

The kernel's compile-time + runtime hardening posture per ARCH §24. P1-H deliverable: stack canaries, PAC return-address signing, BTI indirect-branch guards, LSE atomics permission, stack-clash protection, NX stack. Runtime hardware-feature detection populates a banner-reportable struct.

Scope: `cmake/Toolchain-aarch64-thylacine.cmake` (compile/link flags), `kernel/canary.{c}` + `kernel/include/thylacine/canary.h` (`__stack_chk_guard` + `__stack_chk_fail`), `arch/arm64/start.S` (PAC keys + SCTLR enable), `arch/arm64/hwfeat.{h,c}` (ID-register inspection), `arch/arm64/mmu.h` (PTE_GP for BTI on kernel text), `kernel/main.c` banner extension, `kernel/test/test_hardening.c` (`hardening.detect_smoke`).

Reference: `ARCHITECTURE.md §24` (hardening design intent), `§28` invariants (W^X I-12 reinforced; KASLR I-16 unchanged), ARM ARM (FEAT_PAuth ARMv8.3, FEAT_BTI ARMv8.5, FEAT_LSE ARMv8.1, FEAT_MTE ARMv8.5).

---

## Purpose

Until P1-H, the kernel had MMU + W^X + KASLR + extinction + IRQ — a credible runtime security posture, but not the full SOTA stack ARCH §24 commits to. P1-H closes the v1.0 hardening commitment: every binary the kernel produces (kernel itself; userspace later) is built with the strict hardening flag set, and the compile-time markup is reinforced by runtime CPU-feature enablement.

What's enabled vs deferred at v1.0:

| Feature | Status | Notes |
|---|---|---|
| Stack canaries (`-fstack-protector-strong`) | **live** | `__stack_chk_guard` initialized from KASLR entropy; `__stack_chk_fail` extincts. |
| Stack-clash protection (`-fstack-clash-protection`) | live | Compile-time only; probes guard pages on large frames. Mostly inert in our codebase. |
| PAC return-address signing (`-mbranch-protection=pac-ret`) | live | APIA key set in start.S from cntpct; SCTLR_EL1.EnIA enabled. NOPs on ARMv8.0 hardware. |
| BTI indirect-branch guards (`-mbranch-protection=bti`) | live | SCTLR_EL1.BT0 set; kernel text pages have PTE.GP=1. NOPs on ARMv8.0..8.4 hardware. |
| LSE atomics permission (`-march=armv8-a+lse`) | partial | Compile-time permission. No atomic ops in kernel today; runtime patching deferred to Phase 2 spinlocks. |
| NX stack (`-Wl,-z,noexecstack`) | live | Explicit linker assertion. |
| KASLR (P1-C-extras Part B) | live | Kernel base randomized per boot. |
| W^X (P1-C, I-12) | live | PTE constructors `_Static_assert`'d. |
| **CFI** (`-fsanitize=cfi`) | **deferred** | Needs ThinLTO + careful indirect-call audit. Linux's kCFI is the reference; too risky to enable without dedicated test path. Post-v1.0. |
| **MTE** (`-march=...+memtag`) | **deferred** | Needs SLUB tag-aware integration. Per ARCH §24.3 measurement at Phase 8. The CPU may report MTE in the features banner (we detect it) but the kernel doesn't tag allocations yet. |
| **`_FORTIFY_SOURCE=2`** | **N/A** | Needs hosted libc (`__sprintf_chk` etc.). Freestanding kernel doesn't link against libc. |
| **Hardened malloc / Scudo** | **N/A** | Userspace concern; Phase 5 with musl port. |

The deferred items each have a clear "lands when" condition. Per CLAUDE.md "complexity is permitted only where it is verified", silently enabling without verified test paths would risk silent regressions. The P1-H commit explicitly enumerates these so future audits don't claim "we promised CFI but never delivered" — the deferral is intentional and tracked.

---

## Public API

`kernel/include/thylacine/canary.h`:

```c
void canary_init(u64 seed);
u64  canary_get_cookie(void);
```

`canary_init` is called from `kaslr_init` (which is marked `__attribute__((no_stack_protector))`). It mixes the seed into a runtime cookie and writes `__stack_chk_guard`. If the mixed seed happens to be zero, falls back to `cntpct_el0` to guarantee a non-zero cookie. Idempotent only in the trivial sense.

`canary_get_cookie` is a diagnostic accessor for the test harness — returns the live cookie. The harness verifies it's non-zero (canary_init ran).

ABI symbols emitted by `-fstack-protector-strong` and provided by `kernel/canary.c`:

```c
volatile u64 __stack_chk_guard;       // global cookie (no header)
__attribute__((noreturn))
void __stack_chk_fail(void);          // extincts on canary mismatch
```

`arch/arm64/hwfeat.h`:

```c
struct hw_features {
    bool atomic;     // FEAT_LSE
    bool crc32;      // FEAT_CRC32
    bool pac_apa;    // FEAT_PAuth APIA via QARMA
    bool pac_api;    // FEAT_PAuth APIA via IMPDEF
    bool pac_gpa;    // FEAT_PAuth PACGA via QARMA
    bool pac_gpi;    // FEAT_PAuth PACGA via IMPDEF
    bool bti;        // FEAT_BTI
    u8   mte;        // FEAT_MTE: 0 / 1 inst-only / 2 +tags / 3 +async
};
extern struct hw_features g_hw_features;

void     hw_features_detect(void);
unsigned hw_features_describe(char *buf, unsigned cap);
```

`hw_features_detect` is called from `boot_main` after `slub_init`. Reads `ID_AA64ISAR0_EL1`, `ID_AA64ISAR1_EL1`, `ID_AA64PFR1_EL1`. Idempotent (re-readable; subsequent calls overwrite the struct with the same values). `hw_features_describe` produces a comma-separated string for the banner (e.g., `"PAC,BTI,MTE1,LSE,CRC32"`).

---

## Implementation

### Toolchain flags (cmake/Toolchain-aarch64-thylacine.cmake)

Compile flags added at P1-H:

```
-march=armv8-a+lse+pauth+bti      ; permit LSE / PAuth / BTI instructions
-fstack-protector-strong          ; canaries on functions with arrays / addr-taken locals
-fstack-clash-protection          ; probe guard pages on large frames
-mbranch-protection=pac-ret+bti   ; emit paciasp/autiasp + bti markers
```

Link flags added:

```
-Wl,-z,noexecstack                ; NX stack (explicit, even though static-pie has no PT_GNU_STACK by default)
```

`-march=armv8-a+lse+pauth+bti` is the assembler/compiler permission flag — enables emitting LSE / PAuth / BTI instructions where appropriate. The runtime hardware may or may not implement these features; emitted PAC instructions are NOPs on pre-ARMv8.3 hardware (HINT space), and BTI markers are NOPs on pre-ARMv8.5.

### Stack canary cookie initialization

```c
// kaslr_init (marked no_stack_protector):
//   ... compute mixed seed from DTB or cntpct ...
//   canary_init(mixed);     // overwrites __stack_chk_guard
//   ... apply_relocations + return ...
```

`kaslr_init` is `no_stack_protector` so writing `__stack_chk_guard` mid-function doesn't trip its own canary check. Functions called from `kaslr_init`'s prologue (`mix64`, `dtb_*`, `apply_relocations`) all reach their epilogue BEFORE `canary_init` runs OR run after canary_init. Either way, each individual function's prologue and epilogue read a consistent value.

The fallback to `cntpct_el0` if the mixed seed is zero ensures the cookie is never zero — a zero cookie would silently pass canary checks for any function whose stack-stored canary happened to be zero (e.g., uninitialized stack memory).

### PAC + BTI runtime enable (arch/arm64/start.S)

Inserted between BSS clear and `kaslr_init`:

```asm
// PAC keys from cntpct entropy.
mrs     x0, cntpct_el0
msr     apiakeyhi_el1, x0
ror     x1, x0, #13
msr     apiakeylo_el1, x1
... APIB / APDA / APDB keys (rotations of the same seed) ...

// SCTLR_EL1: set EnIA (31), EnIB (30), EnDA (27), EnDB (13), BT0 (35).
mrs     x0, sctlr_el1
movz    x1, #0x2000, lsl #0      ; bit 13
movk    x1, #0xc800, lsl #16     ; bits 27, 30, 31
movk    x1, #0x0008, lsl #32     ; bit 35
orr     x0, x0, x1
msr     sctlr_el1, x0
isb
```

The 0x00000008c8002000 mask:
- Bit 13 (EnDB) = APDB key auth enable
- Bit 27 (EnDA) = APDA key auth enable
- Bit 30 (EnIB) = APIB key auth enable
- Bit 31 (EnIA) = APIA key auth enable (this is what `-mbranch-protection=pac-ret` actually uses)
- Bit 35 (BT0) = BTI enforcement at EL1 on guarded pages

On ARMv8.0 hardware all these bits are RES0 and writes are silently ignored — so the sequence is zero-cost on older cores and unlocks the feature on 8.3+ / 8.5+.

**Order matters**: PAC must be set up BEFORE the first C function emitting `paciasp` runs, or the auth at function exit would mismatch the sign at function entry. In start.S we set PAC up between the BSS clear (which doesn't use PAC) and the `bl kaslr_init` (which does, because `kaslr_init` is C-compiled with `-mbranch-protection=pac-ret`).

### Long-branch into TTBR1 — BLR not BR

P1-G's `br x0` to boot_main's high VA was changed to `blr x0` at P1-H. Why: BLR sets `PSTATE.BTYPE = 01` (call type), which matches the compiler-emitted `bti c` at boot_main's prologue. BR would set BTYPE=10 (jump type), and a `bti c` target would fault under BTI on FEAT_BTI hardware. The discarded LR is harmless — boot_main is `noreturn` and never reads it.

### PTE_GP on kernel text (arch/arm64/mmu.{h,c})

`PTE_KERN_TEXT` now includes `PTE_GP` (bit 50 = 1). This marks the page as "guarded" — when SCTLR_EL1.BT0 = 1, indirect branches to instructions in this page are checked against `bti` markers at the landing pad. Without GP=1, BTI is not enforced on the page even if SCTLR.BT0 is set.

The other PTE constructors (RW, RO, Device) leave GP=0 — those pages aren't executable anyway.

### hw_features_detect

Reads three system registers and extracts the feature fields per ARM ARM D17.2.x:

| Register | Field | Bits | Meaning |
|---|---|---|---|
| ID_AA64ISAR0_EL1 | Atomic | 23:20 | FEAT_LSE |
| ID_AA64ISAR0_EL1 | CRC32 | 19:16 | FEAT_CRC32 |
| ID_AA64ISAR1_EL1 | APA | 7:4 | FEAT_PAuth APIA QARMA |
| ID_AA64ISAR1_EL1 | API | 11:8 | FEAT_PAuth APIA IMPDEF |
| ID_AA64ISAR1_EL1 | GPA | 27:24 | PACGA QARMA |
| ID_AA64ISAR1_EL1 | GPI | 31:28 | PACGA IMPDEF |
| ID_AA64PFR1_EL1 | BT | 3:0 | FEAT_BTI |
| ID_AA64PFR1_EL1 | MTE | 11:8 | FEAT_MTE level (0..3) |

The feature is "present" iff the field is non-zero. `hw_features_describe` produces a banner-friendly comma-separated list.

### Boot-banner extension

`boot_main` adds three lines:

```
  hardening: MMU+W^X+extinction+KASLR+vectors+IRQ+canaries+PAC+BTI+LSE (P1-H)
  features: PAC,BTI,MTE1,LSE,CRC32 (CPU-implemented)
  canary: initialized (fold 0x<16-bit>)
```

The hardening line lists what was *compiled in*; the features line lists what the *CPU implements*. They diverge gracefully on older hardware (ARMv8.0 → "compiled with PAC/BTI but CPU lacks them, so they're harmless NOPs").

---

## Data structures

`struct hw_features` (~16 bytes BSS). Singleton; populated once at boot. No locking — single-threaded init.

`__stack_chk_guard` is a single `volatile u64` in BSS, initialized at link time to the magic `0x000000000000aff0` (Linux's pre-init guard pattern) and overwritten by `canary_init`.

---

## State machines

### Stack canary lifecycle

```
LINK_TIME_MAGIC (0x000000000000aff0)
    [boot start]
    [start.S sets up PAC keys]
    [bl kaslr_init]
        kaslr_init prologue: NO canary check (no_stack_protector)
        ... compute seed ...
        canary_init(seed)
            __stack_chk_guard ← runtime cookie
        ... apply_relocations (canary-protected; reads runtime cookie) ...
        kaslr_init epilogue: NO canary check
RUNTIME_COOKIE
    [all subsequent canary-protected functions read runtime cookie consistently]
```

Functions whose entire call lifetime predates `canary_init` see the link-time magic at both prologue and epilogue. Functions whose entire call lifetime postdates `canary_init` see the runtime cookie. The transition is atomic (single 8-byte volatile store inside `canary_init`).

### PAC enable lifecycle

```
[start.S enters EL1]
    [BSS clear; APIA key uninitialized = 0]
    [Set APIA / APIB / APDA / APDB keys from cntpct]
    [Set SCTLR_EL1.{EnIA, EnIB, EnDA, EnDB, BT0}]
    [isb]
PAC active (on ARMv8.3+ hardware)
    [bl kaslr_init]
        kaslr_init prologue: paciasp signs LR with APIA + SP context
        ... runs ...
        kaslr_init epilogue: autiasp authenticates LR
        ret
    [all subsequent PAC-protected functions sign/auth consistently]
```

The sequence is critical: PAC must be active BEFORE any function emitting `paciasp` runs. `bl kaslr_init` is the first such call; the PAC enable precedes it.

---

## Spec cross-reference

No formal spec at P1-H. Hardening surfaces are inherently structural — the invariants ("PAC signs return addresses; mismatch panics") are enforced by hardware semantics, not by software state machines we'd model in TLA+.

ARCH §28 invariants reinforced:
- I-12 (W^X): unchanged from P1-C; `pte_violates_wxe` continues to enforce.
- I-16 (KASLR): unchanged from P1-C-extras Part B.

---

## Tests

One leaf-API test at landing (`kernel/test/test_hardening.c`):

| Test | What | Coverage |
|---|---|---|
| `hardening.detect_smoke` | `__stack_chk_guard != 0`; `g_hw_features.{pac_apa, bti}` matches a fresh ID-register read; PAC + BTI both detected | canary_init ran successfully; ID-register field extraction matches ARM ARM; QEMU virt + Pi 5 satisfy our hardware assumptions |

What's NOT tested:

- **Deliberate stack overflow** (would extinct `__stack_chk_fail`). P1-I deliverable (`THYLACINE_DELIBERATE_FAULT=stack_overflow` build flag).
- **PAC mismatch** (forge LR + return). P1-I deliverable.
- **BTI fault** (indirect branch to non-BTI target). P1-I deliverable.
- **CFI**: deferred; would need LTO build path.
- **MTE allocations** (heap tag mismatch catches UAF). Deferred per ARCH §24.3 measurement at Phase 8.

The full deliberate-fault matrix lands at P1-I when the dedicated `tools/test-fault.sh` target arrives.

---

## Error paths

| Condition | Behavior |
|---|---|
| Stack canary mismatch | `__stack_chk_fail` → `extinction("stack canary mismatch (smashed stack)")` |
| PAC auth failure on `autiasp` | hardware modifies upper bits of LR to a poisoned pattern; subsequent `ret` fetches an instruction at a high-bit address → translation fault → `exception_sync_curr_el` → `extinction("unhandled translation fault", far)`. Future P1-I diagnostic: recognize PAC-poisoned addresses and emit `"PAC auth failure on return"`. |
| BTI fault on indirect branch to non-`bti` target | hardware raises Branch Target exception → ESR_EL1.EC = 0x0D → `exception_sync_curr_el` falls through to generic-extinction with EC encoded in ESR. Future P1-I diagnostic. |
| Hw feature ID register reads return zero (very old / non-ARMv8.3 hardware) | `g_hw_features.*` all `false`; banner reports `"(none)"` instead of feature list; PAC / BTI compile-time markers run as NOPs. Boot succeeds; the kernel runs without hardware-enforced hardening but with a clean diagnostic posture. |

---

## Performance characteristics

| Metric | Estimated | Notes |
|---|---|---|
| Canary check overhead per function | ~5 cycles | one global load + one stack store at prologue; one global load + cmp + cbnz at epilogue. Hot paths typically don't have arrays / addr-taken locals so most kernel functions are unaffected by `-fstack-protector-strong`. |
| `paciasp` / `autiasp` overhead | ~1-2 cycles | HINT space on ARMv8.0 (free); ~1-2 cycles on FEAT_PAuth hardware (modern ARM cores have dedicated PAC units). |
| `bti` marker overhead | 0 cycles | Always free (HINT-space NOP on the hot fall-through path). |
| ID register reads in `hw_features_detect` | ~10 cycles | three MRSes + bit extraction. One-time at boot. |
| Kernel ELF growth (P1-G → P1-H) | +11 KB | canary checks + paciasp/autiasp/bti instructions across ~hundreds of functions. ~15% growth on already-debug-info-heavy build. |
| BSS footprint | +24 bytes | `g_hw_features` (~16) + `__stack_chk_guard` (8). |

The hardening overhead is dominated by canary checks; PAC + BTI are essentially free at runtime. ARCH §24's Phase 8 MTE measurement target (≤ 15% on tagged allocations) is for future MTE work, not P1-H.

---

## Status

**Implemented at P1-H**:

- Toolchain hardening flags: `-fstack-protector-strong`, `-fstack-clash-protection`, `-mbranch-protection=pac-ret+bti`, `-march=armv8-a+lse+pauth+bti`, `-Wl,-z,noexecstack`.
- Stack canary infrastructure: `__stack_chk_guard` (BSS, volatile u64) + `__stack_chk_fail` (extincts) in `kernel/canary.c`. `canary_init(seed)` from `kernel/include/thylacine/canary.h`. Initialized in `kaslr_init` (marked `no_stack_protector`).
- PAC keys + SCTLR enable in `arch/arm64/start.S` between BSS clear and `bl kaslr_init`. APIA / APIB / APDA / APDB keys from `cntpct_el0` rotations. SCTLR_EL1.{EnIA, EnIB, EnDA, EnDB, BT0} = 1.
- BTI on kernel text: PTE_GP (bit 50) added to `PTE_KERN_TEXT` in `arch/arm64/mmu.h`.
- Long-branch into TTBR1 changed from BR to BLR in `start.S` (PSTATE.BTYPE=01 matches compiler-emitted `bti c`).
- HW feature detection: `arch/arm64/hwfeat.{h,c}` with ID-register inspection.
- Banner: `hardening:` line lists static features; `features:` line lists CPU-implemented features; `canary:` line shows the live cookie.
- Test: `hardening.detect_smoke` (canary cookie + ID-register self-check + PAC/BTI presence assertion).

**Not yet implemented** (deferred with explicit rationale):

- **CFI** (`-fsanitize=cfi`): needs ThinLTO + indirect-call audit. Linux kCFI is the reference for kernel CFI; v1.0 doesn't have the testing infra to validate. Post-v1.0 hardening pass.
- **MTE** allocations: ARCH §24.3 defers measurement to Phase 8. The CPU may report MTE in features (`MTE1` etc.) — we detect but don't tag. Tag-aware SLUB integration is a Phase 8 deliverable.
- **`_FORTIFY_SOURCE=2`**: requires hosted libc. Freestanding kernel doesn't link against libc.
- **Hardened malloc / Scudo**: userspace; Phase 5 with musl port.
- **Deliberate-fault test target**: P1-I deliverable. Each protection (canary, PAC, BTI, W^X) gets a dedicated build flag that triggers the fault and verifies the right `EXTINCTION:` message.
- **PAC poisoned-address recognition** in fault handler: P1-I refinement (currently shows `"unhandled translation fault"` for PAC failures). The hardware behavior is "ret to a high-bit address that triggers translation fault"; the diagnostic would inspect the FAR and emit `"PAC auth failure on return"`.
- **Atomic primitives with LSE / LL/SC runtime patching**: Phase 2 with spinlocks. The compile flag `+lse` is already set; the patching mechanism (`apply_alternatives()`) lands when atomics arrive.

**Landed**: P1-H at commit `e8c9c5c`.

---

## Caveats

### Canary cookie initialization timing

The `kaslr_init` function MUST be marked `__attribute__((no_stack_protector))`. If a future refactor removes the attribute, `kaslr_init`'s prologue would read the link-time magic, then `canary_init` overwrites the global, then the epilogue reads the runtime cookie — mismatch → `__stack_chk_fail` at boot. Catch-bug: the boot test would fail with `EXTINCTION: stack canary mismatch (smashed stack)`.

### PAC keys are derived from cntpct, not high-entropy (P1-H audit F15)

We seed the APIA / APIB / APDA / APDB keys from the architectural counter at boot. cntpct has limited entropy (advance rate ~24 MHz; boot-time variance is millions of cycles in a few ms — ~20-25 bits of unpredictability, not 64). The four keys are also derived via rotation of the same cntpct value (APIB = ROR(APIA, 13), APDA = ROR(APIB, 17), etc.) — recovering APIA reveals the others (P1-H audit F22 documents this correlation).

**Stronger entropy is available a few hundred cycles later** but isn't currently used: `kaslr_init` collects the DTB-supplied 64-bit `kaslr-seed` / `rng-seed` (firmware-provided entropy) and uses it for both the KASLR slide and the canary cookie. PAC keys could share that source if the enable were moved past the DTB parse. Why we don't:

1. PAC must be set up BEFORE the first canary-protected C function returns. `kaslr_init` is the first C function called from start.S; reversing the order would require a no-PAC bootstrap C path or moving PAC enable into `kaslr_init` itself (with `__attribute__((no_branch_protection))`).
2. The mid-frame SCTLR_EL1.EnIA write would mismatch any function whose `paciasp` ran with EnIA=0 (HINT NOP) and `autiasp` runs with EnIA=1 (real auth against zero-key) — silent boot regression class.

For v1.0 this is acceptable: PAC's threat model is "attacker has corrupted a return address; we want them to need to forge a valid signature." Even low-entropy keys raise that bar significantly compared to no PAC at all. **Post-v1.0 hardening pass** (lands with the dedicated CSPRNG): derive each of APIA/APIB/APDA/APDB from a separate hash of the entropy source (`mix64(seed ^ APIA_DOMAIN)`, `mix64(seed ^ APIB_DOMAIN)`, etc.) so a recovered APIA doesn't trivially reveal the others, AND move the enable past the DTB parse so the seed has 64 bits of firmware-supplied entropy rather than 20 from cntpct.

### MTE detected but not used

`g_hw_features.mte` reports the level (1..3) on FEAT_MTE hardware (e.g., QEMU virt with `-cpu max`). The kernel doesn't tag allocations. A future caller reading `g_hw_features.mte > 0` and assuming "MTE is enforcing" would be wrong — that would be the Phase 8 SLUB tag-aware deliverable.

### `-fstack-clash-protection` is mostly inert

Our codebase has no functions with stack frames > 4 KiB (the boot stack is 16 KiB total but no single function consumes that much). The flag is enabled defensively + future-proof — Phase 2's process model may introduce larger frames; the flag will start being relevant then.

### LSE atomics permission without runtime fallback

`-march=armv8-a+lse+pauth+bti` permits the compiler to emit LSE atomic instructions. Our codebase has no atomic operations today (spinlock.h is a no-op stub at v1.0; Phase 2 introduces real spinlocks). If a future change introduces `__atomic_*` builtins or `_Atomic` types BEFORE Phase 2's atomic primitive layer lands, the compiler may emit LSE ops that trap on ARMv8.0 hardware. Mitigation: Phase 2's `arch/arm64/atomic.S` will provide LL/SC fallback patched at boot via `apply_alternatives()` (per ARCH §24.4).

### CFI deferral is intentional

Skipping `-fsanitize=cfi` is a conscious v1.0 choice, not an oversight. ARCH §24.2 lists CFI in the canonical hardening flag set. The decision to defer is based on:

1. CFI requires `-flto=thin` for cross-TU type recovery; our static-pie kernel build hasn't been validated under LTO.
2. CFI's indirect-call check fires on every function-pointer call; we have function pointers in the GIC dispatch table, the test runner, the test_case fn, and Phase 2 will add many more (handlers, syscall table, scheduler ops). A misconfigured CFI policy silently breaks at runtime.
3. Linux's kCFI is the reference but it's a 2022+ feature with significant per-arch scaffolding. We don't have the test surface to validate.

The deferral is documented HERE (visible to auditors) and in `ARCHITECTURE.md` (when the next ARCH revision absorbs the v1.0 actual scope). Post-v1.0 hardening pass picks it up with the proper testing infrastructure.

### Banner growth

Three new banner lines: `hardening:` (existing, extended), `features:`, `canary:`. The agentic-loop tooling (`tools/test.sh`) only matches against `Thylacine boot OK` and `EXTINCTION:` prefix; the new lines are informational. If a future tooling change starts parsing the banner more aggressively, the contract from `TOOLING.md §10` extends (P1-H additions are stable lines).

---

## See also

- `docs/reference/01-boot.md` — entry sequence (PAC enable in start.S; canary_init slot in kaslr_init).
- `docs/reference/03-mmu.md` — `PTE_KERN_TEXT` now includes PTE_GP.
- `docs/reference/05-kaslr.md` — kaslr_init now also seeds the canary cookie.
- `docs/reference/08-exception.md` — fault handler still extincts on permission/translation faults; PAC + BTI poisoned-address recognition is a P1-I refinement.
- `docs/ARCHITECTURE.md §24` — hardening design intent + the v1.0 commitment.
- ARM Architecture Reference Manual (DDI 0487) — D17.2 (ID registers), D11 (PAC), D8.4 (BTI).
- Linux kernel arch/arm64/include/asm/cpufeature.h — reference for ID-register field layouts.
