// Stack canary cookie + fail handler.
//
// __stack_chk_guard is the global cookie that -fstack-protector-strong
// reads at function prologue (saves to stack) and re-reads at epilogue
// (compares against the stack value). __stack_chk_fail is invoked on
// mismatch — it must not return.
//
// Initialization sequence:
//   1. At link time __stack_chk_guard = a non-zero magic. Functions
//      that complete before canary_init runs use this value
//      consistently (read at prologue + epilogue), so their canary
//      checks pass.
//   2. canary_init(seed) overwrites the global with a runtime cookie
//      derived from the KASLR seed. The function calling canary_init
//      (kaslr_init, marked no_stack_protector) must NOT have a canary
//      check itself, otherwise the prologue read of the magic value
//      would mismatch the epilogue read of the runtime cookie.
//   3. All callees of kaslr_init that use the magic value ran to
//      completion BEFORE canary_init's write — so their epilogues
//      saw the magic value consistently. Functions called AFTER
//      canary_init returns see the runtime cookie consistently.
//
// The "magic" link-time pattern avoids a zero cookie (which would
// silently disable protection for any function whose stack-stored
// canary happened to be zeroed by uninitialized stack memory). The
// pattern is the published guard-canary value Linux uses for the
// boot-stack pre-init phase: 0x000000000000aff0. (Architecture-
// agnostic; no claim of secrecy at this stage.)

#include <thylacine/canary.h>
#include <thylacine/extinction.h>
#include <thylacine/types.h>

// __stack_chk_guard is read by every canary-protected function at
// prologue + epilogue. Compiler expects an extern symbol named
// exactly this; clang lowers the canary check to a load of this
// symbol. Initialized at link time to a non-zero pattern; replaced
// by canary_init() once kaslr entropy is available.
//
// `volatile` prevents the compiler from caching the value across
// function calls (defensive; the canary check already does an MMIO-
// style global read, but volatile ensures no constant fold).
volatile u64 __stack_chk_guard = 0x000000000000aff0ull;

// Pin the link-time non-zero contract (P1-H audit F18). A non-zero
// initializer keeps __stack_chk_guard in `.data`, not `.bss`. If a
// toolchain regression (or a refactor to `volatile u64
// __stack_chk_guard = 0;`) put it in BSS, start.S's BSS clear would
// zero it BEFORE kaslr_init runs, and any function with a canary
// check called from start.S → kaslr_init's prologue would prologue-
// read 0, then post-canary_init epilogue-read the runtime cookie ≠ 0
// → __stack_chk_fail at boot. The assert is constant-folded to true
// at compile time but documents the contract.
_Static_assert(0x000000000000aff0ull != 0,
               "__stack_chk_guard must have a non-zero link-time "
               "initializer to stay in .data instead of .bss");

// Mix function — Wymix-style 64-bit avalanche. Reused from kaslr.c's
// mix64 in spirit; redefined locally to avoid a circular dependency
// (canary_init runs from kaslr_init, which would create a build
// ordering dance if canary.c imported kaslr.c statics).
static u64 mix(u64 x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

// One-shot guard: a second canary_init call from a future caller (Phase 2
// process bring-up, debug rekey paths, etc.) would mid-frame change
// __stack_chk_guard for any caller that's currently inside a canary-
// protected function — they'd prologue-read the OLD cookie, store on
// stack, run canary_init, epilogue-read the NEW cookie → mismatch →
// __stack_chk_fail. Forbid by extincting on second call (P1-H audit
// F23). This guard variable lives in BSS (initialized false at boot;
// start.S BSS clear runs before kaslr_init).
static bool canary_initialized;

__attribute__((no_stack_protector))
void canary_init(u64 seed) {
    if (canary_initialized) {
        extinction("canary_init called twice (would re-key live frames)");
    }
    // Mix the seed; if the result is still zero (astronomically unlikely
    // — mix is a permutation, so mix(x)==0 iff x==0), fall back to
    // cntpct_el0. A zero cookie would let any function whose stack-
    // stored canary happens to be zero pass its check trivially.
    u64 cookie = mix(seed);
    if (cookie == 0) {
        u64 cnt;
        __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(cnt));
        cookie = mix(cnt | 1ull);          // |1 ensures non-zero pre-mix
        if (cookie == 0) cookie = 0xa5a5a5a5a5a5a5a5ull;
    }
    __stack_chk_guard = cookie;
    canary_initialized = true;
}

u64 canary_get_cookie(void) {
    return __stack_chk_guard;
}

// __stack_chk_fail is the ABI symbol clang generates calls to on a
// canary mismatch. Must not return — the stack is corrupt; nothing
// downstream can trust it.
__attribute__((noreturn))
void __stack_chk_fail(void) {
    extinction("stack canary mismatch (smashed stack)");
}
